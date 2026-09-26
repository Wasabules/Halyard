// StreamActivity - full-screen video stream display + native modal menu.
//
// The menu is a nanovg overlay drawn straight inside StreamView (no borealis
// view, no Dialog -> zero focus/animation race, zero pushActivity).
//
// The + or B button opens/closes the menu. While the menu is open:
//   up/down : move through the entries
//   A       : activate the current entry ("Continuer" / stats toggle /
//             "Infos" / "Quitter")
//   B       : close the menu (same as "Continuer")
//
// Entries:
//   0 : "Continuer" (close menu)
//   1 : toggle "Afficher infos perf" - FPS/resolution overlay during the stream
//   2 : "Infos VM" (info-only)
//   3 : "Quitter" the stream (signalAbort -> worker pops StreamActivity)

#include <cstdio>
#include "activity/stream_activity.hpp"
extern "C" {
#include "../../../core/services/atomic_file.h"   /* shadow_file_remove/_rename */
}
#include "activity/shadow_app.hpp"
#include "settings.hpp"
#include "../ui/i18n.hpp"
#include "../ui/env_note.hpp"
#include "../device_mode.hpp"
#include "../../../core/input/pad_mouse_hid.hpp"   /* PM1 - the Joy-Cons as a mouse */

#include <cmath>
#include <cstdio>

extern "C" {
#include "../../../core/protocol/eq.h"
}

extern "C" {
#include "core/common/log.h"
/* S81 - this module's category. See shadow/journal.h: it is declared here,
 * never inferred from the text of the messages. */
#define salog(...) JOURNAL_INFO_(JOURNAL_CAT_UI, __VA_ARGS__)
#define sadbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_UI, __VA_ARGS__)

#include "../../../core/input/shadow_input.h"
#include "../../../core/media/audio.h"        /* output volume */
}

extern "C" {
#include "../../../core/protocol/ctrl_session.h"
#include "../../../core/protocol/bitrate.h"   /* B1: the ONE bitrate ladder */   /* bitrate applied live */
#include "../../../core/protocol/ctrl_gamepad.h"   /* D1: debug page of the pause menu */
#include "../../../core/services/config.h"            /* D1: SHADOW_DATA_DIR (hwaccel marker) */
}

StreamActivity::StreamActivity() {}

StreamActivity::~StreamActivity() {
    salog("[NAV] vue-flux DETRUITE");
    stream_view_set_active(nullptr);
    /* Restore Borealis' globalQuit for the other activities (boot, login,
     * vm_list, where Plus quitting the app is what we want). */
    brls::Application::setGlobalQuit(true);
}

brls::View* StreamActivity::createContentView() {
    streamView = new StreamView();
    return streamView;
}

/* Values offered for the video settings. 0 means "auto", i.e. the desktop
 * client's defaults, which the server already knows. */
/* B1 - the bitrate ladder is no longer written here: it is the shared one
 * (streaming/bitrate.h). This table stopped at 50 while the quality screen went
 * to 150, so a value chosen there was not on this one - `cycleIndex` did not
 * find it, left its index at 0, and one press on RIGHT dropped 40 Mbps to 5. */
static const uint32_t FRAMERATES[] = { 0, 30, 60, 90, 120, 144 };
static const uint32_t RESOLUTIONS[][2] = {
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 },
};

/* Steps through a list of values, wrapping around. Returns the next index. */
template <typename T, size_t N>
static size_t cycleIndex(const T (&values)[N], T current, int dir)
{
    size_t idx = 0;
    for (size_t i = 0; i < N; i++)
        if (values[i] == current) { idx = i; break; }
    return (idx + (dir >= 0 ? 1 : N - 1)) % N;
}

/* === S90c - ONE SINGLE STEP FUNCTION FOR ALL FOUR PARAMETERS ===
 *
 * The notch tables are the ones from the settings screen, and they live HERE
 * rather than being copied into four lambdas: four copies eventually diverge,
 * and we would end up with a pause menu offering frequencies the settings
 * screen does not know about.
 *
 * Touching a band SWITCHES the current mode's profile to "Personnalise".
 * Without that, you would be adjusting a band while listening to a ready-made
 * profile - that is, hearing nothing change, and concluding the menu is
 * broken. */
static void streamMenuEqStep(int band, int param, int dir)
{
    static const float FREQS[] = {
        30, 40, 60, 80, 100, 120, 160, 200, 250, 315, 400, 500, 630, 800,
        1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000,
        12500, 16000
    };
    static const float QS[]    = { 0.3f, 0.5f, 0.707f, 1.0f, 1.4f, 2.0f, 3.0f, 4.0f, 6.0f };
    static const float GAINS[] = { -12, -9, -6, -4.5f, -3, -1.5f, 0, 1.5f, 3, 4.5f, 6, 9, 12 };

    if (band < 0 || band >= EQ_BANDS) return;

    Settings &c = Settings::instance();
    eq_band_t b[EQ_BANDS];
    if (c.eqReadBands(b) == 0) eq_preset(EQ_PRESET_HANDHELD, b);

    const int step = (dir >= 0) ? 1 : -1;
    switch (param) {
    case 0: {
        int t = (int)b[band].type + step;
        if (t < 0) t = EQ_TYPE_COUNT - 1;
        if (t >= EQ_TYPE_COUNT) t = 0;
        b[band].type = (eq_type_t)t;
        break;
    }
    case 1: {
        const int n = (int)(sizeof FREQS / sizeof FREQS[0]);
        int i = 0; float e = 1e9f;
        for (int k = 0; k < n; k++) { const float d = fabsf(FREQS[k] - b[band].freq);
                                      if (d < e) { e = d; i = k; } }
        i = (i + (step > 0 ? 1 : n - 1)) % n;
        b[band].freq = FREQS[i];
        break;
    }
    case 2: {
        const int n = (int)(sizeof QS / sizeof QS[0]);
        int i = 0; float e = 1e9f;
        for (int k = 0; k < n; k++) { const float d = fabsf(QS[k] - b[band].q);
                                      if (d < e) { e = d; i = k; } }
        i = (i + (step > 0 ? 1 : n - 1)) % n;
        b[band].q = QS[i];
        break;
    }
    case 3: {
        const int n = (int)(sizeof GAINS / sizeof GAINS[0]);
        int i = 0; float e = 1e9f;
        for (int k = 0; k < n; k++) { const float d = fabsf(GAINS[k] - b[band].gain_db);
                                      if (d < e) { e = d; i = k; } }
        i = (i + (step > 0 ? 1 : n - 1)) % n;
        b[band].gain_db = GAINS[i];
        break;
    }
    default: return;
    }

#ifdef __SWITCH__
    const bool dock = device::isDocked();
#else
    const bool dock = true;
#endif
    uint32_t &p = dock ? c.eq_preset_docked : c.eq_preset_handheld;
    p = (uint32_t)EQ_PRESET_CUSTOM;

    c.eqWriteBands(b);   /* saves AND applies: see Settings */
}

static void buildVideoPage(StreamView *sv, ui::Page &page)
{
    (void)sv;
    page.choice(ui::tr("menu/bitrate"),
                ui::envNote("SHADOW_BITRATE_MBPS", ui::tr("menu/bitrate_desc")),
        [] {
            /* B1 - shows the value IN FORCE, not the global field. With the
             * per-link setting on, this menu displayed a number that connect()
             * had already overridden: you read 30, the session ran at 15, and
             * nothing said so. */
            const uint32_t v = Settings::instance().effectiveBitrateMbps();
            if (v == 0) return ui::tr("menu/auto");
            return std::to_string(v) + " Mb/s";
        },
        [](int dir) {
            /* B1 - and it WRITES the field that is in force. Editing the global
             * value while the per-link setting governs the session meant the
             * change survived until the next connection and then vanished. */
            auto &cfg = Settings::instance();
            const uint32_t next = bitrate_step(cfg.effectiveBitrateMbps(), dir);
            if (!cfg.bitrate_per_link)
                cfg.max_bitrate_mbps = next;
            else if (device::linkType() == device::LinkType::Ethernet)
                cfg.bitrate_eth_mbps = next;
            else
                cfg.bitrate_wifi_mbps = next;
            cfg.save();
            /* The official client applies the bitrate without restarting the
             * session; we do the same - Auto included, since Auto is a number
             * on this wire and not an absence. */
            cfg.applyBitrateLive();
        });

    /* CFG-4 2026-09-11 - the frame rate waits for the next connection, like
     * the resolution: it travels only in the channel announcement. This used
     * to say it applied live "in the same message as the bitrate" - that
     * message has carried no frame-rate field since S18. */
    page.choice(ui::tr("menu/fps"),
                ui::envNote("SHADOW_FPS", ui::tr("menu/fps_desc")),
        [] {
            uint32_t v = Settings::instance().target_fps;
            if (v == 0) return ui::tr("menu/auto");
            return std::to_string(v);
        },
        [](int dir) {
            auto &cfg = Settings::instance();
            cfg.target_fps = FRAMERATES[cycleIndex(FRAMERATES, cfg.target_fps, dir)];
            cfg.save();
        });

    /* Explicit send: the bitrate already leaves on every change, but you cannot
     * see the message go. This entry gives a definite gesture - and it is what
     * you need when the session was reopened since the last setting change.
     * (CFG-4: only the bitrate - the frame rate has no live field.) */
    page.action(ui::tr("menu/apply"), ui::tr("menu/apply_desc"), [] {
        auto &cfg = Settings::instance();
        if (!ctrl_session_active()) {
            brls::Application::notify(ui::tr("menu/no_session"));
            return;
        }
        cfg.applyBitrateLive();   /* CFG-4: the bitrate is all this message carries */
        /* CFG-5 2026-09-12 - SAY WHAT WENT OUT, not that something did.
         * Reported: "I can apply video settings right away but I do not feel
         * they are applied properly." The message
         * read "Settings sent to the session" whatever happened, so a press
         * that sent the bitrate and a press that sent the bitrate you had NOT
         * changed looked identical - and the two settings that genuinely do not
         * travel (frame rate, resolution) were on the same page. Naming the
         * value turns the notification into a check you can make yourself. */
        const uint32_t v = cfg.effectiveBitrateMbps();
        brls::Application::notify(v ? ui::tr("menu/applied_bitrate", v)
                                    : ui::tr("menu/applied"));
    }, /*closesMenu=*/false);

    /* Its description now says that it waits: the server does not renegotiate
     * the resolution mid-session, so changing it here produced NOTHING
     * visible - you changed it, you looked, you concluded it was broken. */
    page.choice(ui::tr("menu/resolution"), ui::tr("menu/resolution_next"),
        [] {
            auto &cfg = Settings::instance();
            return std::to_string(cfg.display_width) + "x"
                 + std::to_string(cfg.display_height);
        },
        [](int dir) {
            auto &cfg = Settings::instance();
            const size_t n = sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0]);
            size_t idx = 0;
            for (size_t i = 0; i < n; i++)
                if (RESOLUTIONS[i][0] == cfg.display_width) { idx = i; break; }
            idx = (idx + (dir >= 0 ? 1 : n - 1)) % n;
            cfg.display_width  = RESOLUTIONS[idx][0];
            cfg.display_height = RESOLUTIONS[idx][1];
            cfg.save();
        });

    /* 2026-09-02 - the stretch, reachable DURING the stream: it trades the
     * black bars for a distorted picture, and that trade can only be judged
     * with the picture in front of you. It takes effect on the next frame, so
     * the menu stays open to let you compare. */
    /* S119 - link quality, on the VIDEO page and right under the bitrate:
     * that is exactly where one wonders "why is my picture degrading", and
     * until now the answer lived in a log you had to pull over FTP after
     * quitting. */
    page.action(ui::tr("net/title"), ui::tr("net/menu_desc"),
                [sv] { sv->openNetTest(); });

    page.toggle(ui::tr("settings/stretch"),
                ui::envNote("SHADOW_STRETCH", ui::tr("settings/stretch_desc")),
                [] { return Settings::instance().stretch_image; },
                [] { Settings &c = Settings::instance();
                     c.stretch_image = !c.stretch_image; c.save(); });
}

/* Contents of the pause menu.
 *
 * The activity fills it in, because it alone knows what "Quitter" means
 * (confirmation dialog, stopping the session). The view only draws it and
 * navigates it. Every entry carries its own action: there is no longer an
 * index enum to keep in sync with a switch, as there was back when adding one
 * line meant touching four places.
 */
static void buildPauseMenu(StreamView *sv)
{
    ui::OverlayMenu &m = sv->pauseMenu();
    m.clear();

    /* Volume is a persistent setting: it must take effect from the very first
     * frame, not only after a trip through the menu. */
    audio_set_volume(Settings::instance().audio_volume);

    m.setTitle(ui::tr("menu/title"));

    /* The root stays short: what you come looking for mid-game, and nothing
     * else. Settings and counters sit one level down - flat, they drowned
     * "Continuer" and "Quitter" in the middle of a dozen lines. */
    m.action(ui::tr("menu/continue"), ui::tr("menu/continue_desc"),
             [sv] { sv->closeMenu(); });

    buildVideoPage(sv, m.submenu(ui::tr("menu/video"), ui::tr("menu/video_desc")));

    ui::Page &display = m.submenu(ui::tr("menu/display"), ui::tr("menu/display_desc"));
    display.toggle(ui::tr("menu/perf_panel"), ui::tr("menu/perf_panel_desc"),
               [sv] { return sv->perfStatsEnabled(); },
               [sv] { sv->menuToggleStats(); });

    /* The sections go one level deeper still: there are five of them, and they
     * only matter once the panel is on. */
    ui::Page &sections = display.submenu(ui::tr("menu/sections"),
                                     ui::tr("menu/sections_desc"));
    for (size_t i = 0; i < sv->hud().sectionCount(); i++) {
        /* S106 - A shows or hides the WHOLE section, Y opens the detail of its
         * rows. Two distinct questions: "do I show it?" and "with which
         * rows?". Putting both on the same button would force us to pick which
         * one is the main one, and the other would become a chore.
         *
         * The rows are enumerated when the menu is OPENED, not here: a section
         * only knows its own rows after a first render, and the menu is built
         * before that. The `prepareDetail` hook is what fills them in. */
        sections.toggleDetail(sv->hud().sectionTitle(i), "",
                              [sv, i] { return sv->hud().sectionEnabled(i); },
                              [sv, i] { sv->toggleHudSection(i); },
                              sv->hud().sectionTitle(i),
                              [sv, i](ui::Page &p) {
                                  for (size_t r = 0; r < sv->hud().rowCount(i); r++)
                                      p.toggle(sv->hud().rowLabel(i, r), "",
                                               [sv, i, r] { return sv->hud().rowVisible(i, r); },
                                               [sv, i, r] { sv->toggleHudRow(i, r); });
                              });
    }

    /* The charts get their own page: you often want one without the matching
     * numbers, or the other way round. */
    ui::Page &charts = display.submenu(ui::tr("menu/charts"), ui::tr("menu/charts_desc"));
    for (size_t i = 0; i < sv->hud().chartCount(); i++) {
        charts.toggle(sv->hud().chartTitle(i), "",
                      [sv, i] { return sv->hud().chartEnabled(i); },
                      [sv, i] { sv->toggleHudChart(i); });
    }

    /* S93 - the layout edit mode. Placed AFTER the section lists: you first
     * choose what to show, then you place what you chose. */
    /* An ACTION, not a toggle: entering edit mode closes the menu, otherwise
     * you would be placing blocks underneath a menu that covers them. You
     * leave it by holding Start, which the description says - a mode with no
     * visible way out is a trap. */
    display.action(ui::tr("menu/edit_pos"), ui::tr("menu/edit_pos_desc"),
               [sv] { sv->editPositions(true); sv->closeMenu(); });
    display.action(ui::tr("menu/reset_pos"), ui::tr("menu/reset_pos_desc"),
               [sv] { sv->reinitPositions(); });

    display.choice(ui::tr("menu/opacity"), ui::tr("menu/opacity_desc"),
        [] { return std::to_string(Settings::instance().hud_opacity) + " %"; },
        [](int dir) {
            auto &cfg = Settings::instance();
            /* In steps of ten: a hundred notches for a setting judged by eye
             * would take thirty presses to cross the range. */
            int v = (int)cfg.hud_opacity + (dir >= 0 ? 10 : -10);
            if (v < 10)  v = 10;
            if (v > 100) v = 100;
            cfg.hud_opacity = (uint32_t)v;
            cfg.save();
        });

    display.choice(ui::tr("menu/refresh"), ui::tr("menu/refresh_desc"),
        [] { return std::to_string(Settings::instance().hud_refresh_ms) + " ms"; },
        [sv](int dir) {
            auto &cfg = Settings::instance();
            static const uint32_t STEPS[] = { 200, 250, 500, 1000 };
            const size_t n = sizeof(STEPS) / sizeof(STEPS[0]);
            size_t idx = 2;
            for (size_t i = 0; i < n; i++)
                if (STEPS[i] == cfg.hud_refresh_ms) { idx = i; break; }
            cfg.hud_refresh_ms = STEPS[(idx + (dir >= 0 ? 1 : n - 1)) % n];
            cfg.save();
            sv->hud().setRefreshMs((int)cfg.hud_refresh_ms);
        });

    display.choice(ui::tr("menu/ui_scale"), ui::tr("menu/ui_scale_desc"),
        [] { return std::to_string(Settings::instance().ui_scale) + " %"; },
        [](int dir) {
            auto &cfg = Settings::instance();
            static const uint32_t STEPS[] = { 75, 100, 125, 150, 200 };
            const size_t n = sizeof(STEPS) / sizeof(STEPS[0]);
            size_t idx = 1;
            for (size_t i = 0; i < n; i++)
                if (STEPS[i] == cfg.ui_scale) { idx = i; break; }
            cfg.ui_scale = STEPS[(idx + (dir >= 0 ? 1 : n - 1)) % n];
            cfg.save();
        });

    /* === S73/S74 - THE PAD TESTER, IN THE SUBMENU THAT ALREADY EXISTS ===
     * It only lived in the Controller settings screen, hence outside any
     * session - and its right-hand half, "what we send", is always dark there
     * since the gamepad channel only exists during a stream. Here the
     * comparison means something.
     *
     * `submenu()` CREATES a page on every call: adding this through a second
     * `m.submenu("Controles")` built a SECOND submenu with the same name and
     * different entries. So we chain onto the one that already exists. */
    /* === PM9 2026-09-02 - THE MOUSE GETS ITS OWN PAGE ===
     *
     * "Controls" had grown to nine entries, five of them about the controller
     * mouse - a feature one either uses or never opens. A page you have to
     * scroll through to reach the line you came for costs something on every
     * visit, and the cost falls on the entries that have nothing to do with the
     * newcomer.
     *
     * `submenu` nests, so grouping is the fix rather than a rewrite. The parent
     * is held by REFERENCE: the pages live on the heap behind `unique_ptr`, so
     * the vector that holds them may grow without any of them moving - taking a
     * `Page&` here is safe, and it is what lets the chain be split in two
     * without the child swallowing the rest. */
    ui::Page &ctrl = m.submenu(ui::tr("menu/controls"), ui::tr("menu/controls_desc"));
    ctrl.action(ui::tr("pad/test"), ui::tr("pad/test_desc"),
                [sv] { sv->openPadTest(); });

    ctrl.submenu(ui::tr("menu/pad_mouse"), ui::tr("menu/pad_mouse_group_desc"))
        /* === PM1 - THE JOY-CONS AS A MOUSE ===
         * Here as well as on the L3+R3 combination, and both call the SAME
         * `toggle()`: two entry points that computed "toggle" separately would
         * eventually disagree about what is on, and the one that is wrong is
         * the one holding a mouse button down.
         *
         * It stays open (`closesMenu = false`) because you turn it on to USE it:
         * closing the menu on the same press would hand the pointer back to a
         * game the instant it appears, with the two sticks still where your
         * thumbs left them. */
        .toggle(ui::tr("menu/pad_mouse"), ui::tr("menu/pad_mouse_desc"),
                [] { return padmouse::active(); },
                [] { padmouse::toggle(); })
        /* PM5 - the pointing test is a PAGE, like the gamepad tester, and it is
         * opened the same way. The first version was a toggle that drew a dot
         * over the video; it could say "the pointer does not move" and nothing
         * more, while the causes are four and call for opposite fixes. */
        .action(ui::tr("menu/pad_mouse_test"), ui::tr("menu/pad_mouse_test_desc"),
                [sv] { sv->openMouseTest(); })
        /* === NO GYROSCOPE HERE, SO NEITHER OF THESE TWO ROWS ============
         *
         * The mode selector only ever flips between stick and gyro - it never
         * turns the mouse off, that is elsewhere - so on a console with no gyro
         * it is a switch with one working position. The source selector below
         * it picks WHICH Joy-Con aims, which is meaningless without detachable
         * controllers.
         *
         * The Vita is not short of a motion sensor; nothing here is wired to
         * `SceMotion`. So this is a "not implemented", not a "cannot" - see
         * `SHADOW_HAS_GYRO` in device_caps.h, which is where it changes if
         * someone wires it. */
#if SHADOW_HAS_GYRO
        .choice(ui::tr("menu/pad_mouse_mode"), ui::tr("menu/pad_mouse_mode_desc"),
                [] {
                    return Settings::instance().pad_mouse_mode == 2
                             ? ui::tr("menu/pad_mouse_gyro")
                             : ui::tr("menu/pad_mouse_stick");
                },
                [](int dir) {
                    (void)dir;
                    Settings &c = Settings::instance();
                    c.pad_mouse_mode = (c.pad_mouse_mode == 2) ? 1u : 2u;
                    c.save();
                    /* Applied AT ONCE when the mouse is already running: the
                     * mode is judged by feel, and having to turn it off and on
                     * again to compare the two would make it impossible. */
                    if (padmouse::active())
                        padmouse::setMode(c.pad_mouse_mode == 2 ? padmouse::Mode::Gyro
                                                                : padmouse::Mode::Stick);
                })
        /* PM8 - which Joy-Con aims. Next to the mode, because it only means
         * anything in gyroscope mode and the two are chosen together. */
        .choice(ui::tr("menu/pad_mouse_gyro_src"), ui::tr("menu/pad_mouse_gyro_src_desc"),
                [] {
                    switch (Settings::instance().pad_mouse_gyro_source) {
                        case 1:  return ui::tr("menu/pad_mouse_gyro_left");
                        case 2:  return ui::tr("menu/pad_mouse_gyro_right");
                        case 3:  return ui::tr("menu/pad_mouse_gyro_both");
                        case 4:  return ui::tr("menu/pad_mouse_gyro_split_dg");
                        case 5:  return ui::tr("menu/pad_mouse_gyro_split_gd");
                        default: return ui::tr("menu/pad_mouse_gyro_auto");
                    }
                },
                [](int dir) {
                    Settings &c = Settings::instance();
                    const int n = 6;   /* PM9: auto, left, right, both, 2 splits */
                    int i = (int)c.pad_mouse_gyro_source + (dir >= 0 ? 1 : n - 1);
                    c.pad_mouse_gyro_source = (uint32_t)(i % n);
                    c.save();
                })
#endif  /* SHADOW_HAS_GYRO */
        .choice(ui::tr("menu/pad_mouse_sens"), ui::tr("menu/pad_mouse_sens_desc"),
                [] { return std::to_string(Settings::instance().pad_mouse_sens) + " %"; },
                [](int dir) {
                    static const uint32_t S[] = { 25, 50, 75, 100, 150, 200, 300, 400 };
                    Settings &c = Settings::instance();
                    c.pad_mouse_sens = S[cycleIndex(S, c.pad_mouse_sens, dir)];
                    c.save();
                    /* No re-arming needed: the config is read from the settings
                     * on every frame, so the change is felt on the next one. */
                });

    ctrl
        .choice(ui::tr("menu/touch"), ui::tr("menu/touch_desc"),
                [] {
                    switch (Settings::instance().touch_mode) {
                        case TouchMode::Absolute: return ui::tr("menu/touch_absolute");
                        case TouchMode::Relative: return ui::tr("menu/touch_relative");
                        default:                  return ui::tr("menu/touch_off");
                    }
                },
                [sv](int dir) { sv->menuCycleTouchMode(dir); })
        .toggle(ui::tr("menu/scroll_invert"), ui::tr("menu/scroll_invert_desc"),
                [] { return Settings::instance().touch_scroll_invert; },
                [] { Settings &c = Settings::instance();
                     c.touch_scroll_invert = !c.touch_scroll_invert; c.save(); })
        /* Mouse mode - reachable IN-GAME, because that is where you notice the
         * problem. A game that captures the mouse recenters it every frame;
         * our absolute positions, computed from our own tracking which ignores
         * that recentring, then drift enormously and the mouse becomes
         * unusable. On the remote desktop, conversely, absolute is the only
         * mode that puts the cursor where it belongs. */
        /* Pointing sensitivity, adjustable IN-GAME: it takes controller in
         * hand to tell whether it is too fast, and several tries to land on
         * the right notch. */
        .choice(ui::tr("menu/touch_sens"), ui::tr("menu/touch_sens_desc"),
                [] { return std::to_string(Settings::instance().touch_sensitivity) + " %"; },
                [](int dir) {
                    static const uint32_t STEPS[] = { 10, 15, 20, 25, 35, 50, 75, 100, 150, 200 };
                    const int n = (int)(sizeof(STEPS) / sizeof(STEPS[0]));
                    auto &cfg = Settings::instance();
                    int i = 7;   /* 100 % */
                    for (int k = 0; k < n; k++) if (STEPS[k] == cfg.touch_sensitivity) i = k;
                    i = (i + (dir >= 0 ? 1 : n - 1)) % n;
                    cfg.touch_sensitivity = STEPS[i];
                    cfg.save();
                })
        /* AUD-CFG-2 2026-09-11 - the env.txt note, as on the Settings screen's
         * row: SHADOW_INPUT_ABS outranks this toggle. */
        .toggle(ui::tr("menu/mouse_relative"),
                ui::envNote("SHADOW_INPUT_ABS", ui::tr("menu/mouse_relative_desc")),
                [] { return Settings::instance().mouse_relative; },
                [] {
                    auto &cfg = Settings::instance();
                    const bool on = !cfg.mouse_relative;
                    cfg.mouse_relative = on;
                    shadow_input_set_mouse_relative(on ? 1 : 0);

                    /* On Switch the mouse comes from the TOUCHSCREEN, and the
                     * touch mode decides everything: in "absolute", each
                     * finger position becomes a screen position - which makes
                     * no sense when a game captures the mouse and recenters
                     * it. "Relative" mode sends finger movements, like a
                     * trackpad, and it is the only usable one in-game.
                     *
                     * This toggle used to act somewhere else, on a path the
                     * touchscreen does not take: it therefore changed NOTHING
                     * on console. It now switches the touch mode as well, so
                     * that one switch does what it says. We remember the
                     * previous mode in order to restore it when going back to
                     * the desktop. */
#ifdef __SWITCH__
                    static TouchMode previous = TouchMode::Absolute;
                    if (on) {
                        if (cfg.touch_mode != TouchMode::Relative) previous = cfg.touch_mode;
                        cfg.touch_mode = TouchMode::Relative;
                    } else {
                        cfg.touch_mode = previous;
                    }
#endif
                    cfg.save();
                })
        /* === Sticks, adjustable IN-GAME (2026-08-27) ===
         * The vertical axis direction is a matter of taste and of genre:
         * flight simulators expect it inverted relative to shooters. The
         * setting already existed, but only in the Controller settings page -
         * hence outside the stream, whereas it is controller in hand that you
         * notice "it goes up when I push down". */
        .toggle(ui::tr("menu/pad_invert_y"), ui::tr("menu/pad_invert_y_desc"),
                [] { return Settings::instance().pad_invert_y; },
                [] {
                    auto &cfg = Settings::instance();
                    cfg.pad_invert_y = !cfg.pad_invert_y;
                    cfg.save();
                })
        /* The deadzone is judged by feel, not by the number: too low and the
         * character drifts on its own, too high and small movements are eaten.
         * You have to try it, hence its place here. */
        .choice(ui::tr("menu/pad_deadzone"), ui::tr("menu/pad_deadzone_desc"),
                [] { return std::to_string(Settings::instance().pad_deadzone) + " %"; },
                [](int dir) {
                    static const uint32_t STEPS[] = { 0, 5, 10, 15, 20, 25, 30 };
                    const int n = (int)(sizeof(STEPS) / sizeof(STEPS[0]));
                    auto &cfg = Settings::instance();
                    int i = 2;   /* 10 % */
                    for (int k = 0; k < n; k++) if (STEPS[k] == cfg.pad_deadzone) i = k;
                    cfg.pad_deadzone = STEPS[(i + (dir >= 0 ? 1 : n - 1)) % n];
                    cfg.save();
                });

    /* === Audio (2026-08-27) ===
     * Volume is the only audio setting that is entirely ours: neither the
     * server nor Opus exposes one, and the console's system volume does not
     * make up for a remote machine whose Windows mixer is turned down. It is
     * adjusted DURING the stream, because you judge it by listening. */
    ui::Page &audio = m.submenu(ui::tr("menu/audio"), ui::tr("menu/audio_desc"));
    /* Hardware Opus decoding is judged by COMPARING on the same scene: exactly
     * the kind of setting that must be toggleable without leaving the stream.
     * It only existed in the settings screen, hence outside any session.
     *
     * === AUD-CFG-2 2026-09-11 - WHAT YOU SEE IS WHAT RUNS ===
     * Console only: SHADOW_HWOPUS is read under HAVE_AUDOUT alone
     * (media/audio.c), so on desktop this row switched nothing. And the audio
     * rows now carry the env.txt note, as the Settings screen's rows always
     * did: with SHADOW_VOLUME=0 left in env.txt the output was silent while
     * this page read 100 % and every press appeared to work - no counter shows
     * it, the gain zeroes the samples after decode. The value shown stays the
     * SAVED one, as on every other forced row; the note says it is not the one
     * running. */
#ifdef __SWITCH__
    audio.toggle(ui::tr("settings/hw_audio"),
                 ui::envNote("SHADOW_HWOPUS", ui::tr("menu/hw_audio_next")),
                 [] { return Settings::instance().hw_opus; },
                 [] {
                     auto &cfg = Settings::instance();
                     cfg.hw_opus = !cfg.hw_opus;
                     cfg.save();
                     cfg.applyToggles();
                 });
#endif
    /* Audio quality is a CODEC, not a bitrate: lossy Opus versus lossless
     * FLAC. It is negotiated when the session opens, so the label says
     * explicitly that you have to reconnect - a setting that appears to do
     * nothing is worse than a missing setting. */
    audio.choice(ui::tr("menu/audio_quality"),
                 ui::envNote("SHADOW_AUDIO_CODEC", ui::tr("menu/audio_quality_desc")),
        [] { return ui::tr(Settings::instance().audio_hifi ? "menu/audio_hifi"
                                                           : "menu/audio_std"); },
        [](int dir) {
            (void)dir;
            auto &cfg = Settings::instance();
            cfg.audio_hifi = !cfg.audio_hifi;
            cfg.save();
            cfg.applyToggles();
        });
    audio.choice(ui::tr("menu/volume"),
                 ui::envNote("SHADOW_VOLUME", ui::tr("menu/volume_desc")),
        [] { return std::to_string(Settings::instance().audio_volume) + " %"; },
        [](int dir) {
            /* Tight notches below 100 (that is where you want fine control)
             * and wider ones above: the boost is there to rescue a source that
             * is too quiet, not to dose it to the percent. */
            static const uint32_t STEPS[] = { 0, 25, 50, 75, 100, 125, 150, 200, 250, 300 };
            const int n = (int)(sizeof(STEPS) / sizeof(STEPS[0]));
            auto &cfg = Settings::instance();
            int i = 4;   /* 100 % */
            for (int k = 0; k < n; k++) if (STEPS[k] == cfg.audio_volume) i = k;
            i = (i + (dir >= 0 ? 1 : n - 1)) % n;
            cfg.audio_volume = STEPS[i];
            cfg.save();
            audio_set_volume(cfg.audio_volume);
        });

    /* === CFG-5 2026-09-12 - THE SAME GESTURE AS THE VIDEO PAGE ===
     * Reported: "I do not have this option in the AUDIO section". The video page
     * has had an explicit "apply now" since CFG-4 and this one had nothing,
     * which reads as "audio settings do not apply live" - the opposite of the
     * truth. Volume and equaliser are OURS and take effect on the next samples
     * decoded; it is the codec that waits for a reconnection, and the row says
     * so rather than leaving you to find out.
     *
     * No `ctrl_session_active()` guard here, unlike the video page: this sends
     * nothing on the wire. It re-applies a local gain, which is exactly why it
     * is worth having - it also repairs the case where env.txt forced
     * SHADOW_VOLUME and the setting on screen was never the one running. */
    audio.action(ui::tr("menu/apply"), ui::tr("menu/apply_audio_desc"), [] {
        auto &cfg = Settings::instance();
        audio_set_volume(cfg.audio_volume);
        cfg.applyEq();
        brls::Application::notify(ui::tr("menu/applied_audio"));
    }, /*closesMenu=*/false);

    /* === S90c 2026-08-29 - THE EQUALIZER IS SET BY LISTENING ===
     *
     * This is the sound setting that most needs to be here. A profile or a
     * band cannot be judged from its label: "peak 1800 Hz Q 1.2 +3 dB" means
     * nothing to anyone, and what counts is the sum of the five bands, not
     * each one taken apart. Setting it from the settings screen, hence outside
     * a session, means setting it without hearing it.
     *
     * The first row is a REMINDER, and it is not decorative: the active
     * profile depends on the console mode (see Settings::appliquerEq), so
     * nothing on screen would otherwise tell you which of the two is playing.
     *
     * The profile choice applies to the CURRENT mode - the one you are
     * hearing. Offering both here would force you to know which one you are in
     * to adjust the right one, whereas the whole point is to keep turning
     * until it sounds right. */
    {
        /* The equalizer moved one level down, into "Audio": it used to sit at
         * the root of the pause menu, next to "Continuer" and "Quitter",
         * although it only concerns sound. A root that takes in everything we
         * add ends up drowning the two entries you look for in a hurry. */
        ui::Page &eq = audio.submenu(ui::tr("eq/title"), ui::tr("menu/eq_desc"));

        eq.info(ui::tr("menu/eq_active"), [] {
            const Settings &c = Settings::instance();
#ifdef __SWITCH__
            const bool dock = device::isDocked();
#else
            const bool dock = true;
#endif
            const uint32_t p = dock ? c.eq_preset_docked : c.eq_preset_handheld;
            return ui::tr(eq_preset_key((eq_preset_t)p)) + "  ("
                 + ui::tr(dock ? "eq/mode_docked" : "eq/mode_handheld") + ")";
        });

        eq.choice(ui::tr("eq/profile"), ui::tr("menu/eq_profile_desc"),
            [] {
                const Settings &c = Settings::instance();
#ifdef __SWITCH__
                const bool dock = device::isDocked();
#else
                const bool dock = true;
#endif
                return ui::tr(eq_preset_key((eq_preset_t)(dock ? c.eq_preset_docked
                                                               : c.eq_preset_handheld)));
            },
            [](int dir) {
                Settings &c = Settings::instance();
#ifdef __SWITCH__
                const bool dock = device::isDocked();
#else
                const bool dock = true;
#endif
                uint32_t &p = dock ? c.eq_preset_docked : c.eq_preset_handheld;
                const int n = (int)EQ_PRESET_COUNT;
                p = (uint32_t)(((int)p + (dir >= 0 ? 1 : n - 1)) % n);
                c.save();
                c.applyEq();
            });

        eq.toggle(ui::tr("eq/auto_trim"), ui::tr("eq/auto_trim_desc"),
            [] { return Settings::instance().eq_auto_trim; },
            [] {
                Settings &c = Settings::instance();
                c.eq_auto_trim = !c.eq_auto_trim;
                c.save();
                c.applyEq();
            });

        /* The five bands, one page each. They are always present - even when
         * the active profile is not "Personnalise" - because touching them is
         * exactly how you decide to switch to it, and a submenu that appears
         * and disappears depending on a neighbouring setting is more confusing
         * than a submenu with no immediate effect. The profile label, just
         * above, says which one is playing. */
        for (int i = 0; i < EQ_BANDS; i++) {
            char t[32];
            std::snprintf(t, sizeof t, "%s %d", ui::tr("eq/band").c_str(), i + 1);
            ui::Page &b = eq.submenu(t);

            b.choice(ui::tr("eq/type"), "",
                [i] {
                    eq_band_t bd[EQ_BANDS];
                    Settings::instance().eqReadBands(bd);
                    return ui::tr(eq_type_key(bd[i].type));
                },
                [i](int dir) { streamMenuEqStep(i, 0, dir); });
            b.choice(ui::tr("eq/freq"), "",
                [i] {
                    eq_band_t bd[EQ_BANDS];
                    Settings::instance().eqReadBands(bd);
                    char v[24];
                    if (bd[i].freq >= 1000.0f)
                        std::snprintf(v, sizeof v, "%.1f kHz", (double)(bd[i].freq / 1000.0f));
                    else
                        std::snprintf(v, sizeof v, "%.0f Hz", (double)bd[i].freq);
                    return std::string(v);
                },
                [i](int dir) { streamMenuEqStep(i, 1, dir); });
            b.choice(ui::tr("eq/q"), ui::tr("eq/q_desc"),
                [i] {
                    eq_band_t bd[EQ_BANDS];
                    Settings::instance().eqReadBands(bd);
                    char v[16]; std::snprintf(v, sizeof v, "%.2f", (double)bd[i].q);
                    return std::string(v);
                },
                [i](int dir) { streamMenuEqStep(i, 2, dir); });
            b.choice(ui::tr("eq/gain"), "",
                [i] {
                    eq_band_t bd[EQ_BANDS];
                    Settings::instance().eqReadBands(bd);
                    char v[16]; std::snprintf(v, sizeof v, "%+.1f dB", (double)bd[i].gain_db);
                    return std::string(v);
                },
                [i](int dir) { streamMenuEqStep(i, 3, dir); });
        }
    }

    m.submenu(ui::tr("menu/info"), ui::tr("menu/info_desc"))
        .info(ui::tr("menu/stream_format"), [sv] { return sv->videoFormatText(); })
        .info(ui::tr("menu/stream_rate"),   [sv] { return sv->videoRateText(); })
        /* What the console DETECTED. The "resolution follows the mode" and
         * "bitrate follows the link" settings change the stream without saying
         * so: without these two rows you could not tell whether the 720p comes
         * from a setting or from a fault, nor why the bitrate moved when you
         * dropped the console into its dock. */
        .info(ui::tr("menu/device_mode"),
              [] { return device::isDocked() ? ui::tr("menu/mode_docked")
                                             : ui::tr("menu/mode_handheld"); })
        .info(ui::tr("menu/device_link"),
              [] { return std::string(device::linkLabel()); });

    /* === D1 2026-08-22 - DEBUG PAGE IN THE PAUSE MENU ===
     * The developer menu (devui) is desktop-only: on console there is no way
     * at all to trigger an action while running. So we gather here what we
     * need to try controller in hand, without rebuilding and without going
     * through FTP - starting with re-announcing the gamepad, since that is the
     * current hypothesis (the VM only creates the gamepad if it receives the
     * announcement at the right moment). */
    ui::Page &dbg = m.submenu(ui::tr("menu/debug"), ui::tr("menu/debug_desc"));
    dbg.action(ui::tr("menu/dbg_replug"), ui::tr("menu/dbg_replug_desc"),
               [] { ctrl_gamepad_plug(); });
    dbg.action(ui::tr("menu/dbg_idr"), ui::tr("menu/dbg_idr_desc"),
               [] { ctrl_session_request_idr(); });
    dbg.action(ui::tr("menu/dbg_refresh"), ui::tr("menu/dbg_refresh_desc"),
               [] { ctrl_session_request_refresh(); });
    /* This row used to say "gamepad read / channel open": on console the first
     * half is ALWAYS true, and the second says nothing about the ANNOUNCEMENT
     * - which is what creates the gamepad on the remote machine. So it could
     * not answer "the gamepad is no longer declared after a reconnection". It
     * can now. */
    dbg.info(ui::tr("menu/dbg_pad_state"), [] {
        char buf[128];
        ctrl_gamepad_diagnostic(buf, sizeof buf);
        return std::string(buf);
    });
    /* Switch hardware <-> software without FTP: we drop or remove the marker,
     * which is read the next time the decoder starts. */
    dbg.action(ui::tr("menu/dbg_hw_toggle"), ui::tr("menu/dbg_hw_toggle_desc"), [] {
        const char *mk = SHADOW_DATA_DIR "hwaccel.disabled";
        FILE *f = fopen(mk, "rb");
        if (f) { fclose(f); shadow_file_remove(mk); brls::Logger::info("[D1] materiel REARME (relancer)"); }
        else {
            f = fopen(mk, "wb");
            if (f) { fprintf(f, "toggled from the debug menu\n"); fclose(f); }
            brls::Logger::info("[D1] decodage LOGICIEL arme (relancer)");
        }
    });

    m.separator();
    /* UX2 B7 2026-05-18 - confirm before cutting off: quitting by accident
     * cost you the session. */
    m.danger(ui::tr("menu/quit"), ui::tr("menu/quit_desc"), [] {
        brls::Dialog *dlg = new brls::Dialog(ui::tr("quit/question"));
        dlg->setCancelable(true);
        /* S66 - see vm_list_activity.cpp: `buttonClick` already removes the
         * dialog, a `close()` here would pop the screen underneath. */
        dlg->addButton(ui::tr("quit/stay"), []() { /* S66: nothing to do */ });
        /* S39 2026-08-25: CLOSE the box before signalling.
         *
         * "stay" called `close()`, "leave" did not: the box stayed on top of
         * the stream, and since nothing was logged there was no way to tell
         * whether the stop had even been requested. Reported symptom: "the
         * stream does not close at all, I have to open another application to
         * force it shut". So we now trace both steps. */
        dlg->addButton(ui::tr("quit/leave"), []() {
            /* S66 - no `close()` here either: `Dialog::buttonClick` already
             * removes the dialog before calling this callback. The `close()`
             * added one pop, which removed the stream view BEFORE the worker
             * thread did - hence one pop too many across the whole exit
             * sequence. */
            salog("[S39] leaving the stream, requested from the menu");
            ShadowApp::instance().signalAbort();
            salog("[S39] drapeau d'arret leve");
        });
        dlg->open();
    });
}

void StreamActivity::onContentAvailable() {
    stream_view_set_active(streamView);
    brls::Logger::info("StreamActivity ready, registered as active stream view");

    /* Disable Borealis' globalQuit (BUTTON_START -> Application::quit) so that
     * the Plus button can serve DS4 Options / hold=menu. We use our own
     * MENU_QUIT to leave the stream cleanly. */
    brls::Application::setGlobalQuit(false);

    StreamView *sv = streamView;

    // + (Start) button: handled directly in StreamView::draw() so we can tell
    // a short press (= sends DS4 Options over shadow-controller) from a long
    // press >= 500 ms (= opens the Shadow menu). We keep only a silent binding
    // here to stop Borealis from swallowing the button.
    this->registerAction(
        "",
        brls::ControllerButton::BUTTON_START,
        [sv](brls::View *) {
            /* No-op: the real logic polls padGetButtons in draw(). */
            (void)sv;
            return true;
        }, /*hidden=*/true);

    /* - (Minus) button: K5 2026-08-27 - the logic now lives in
     * StreamView::draw(), which tells a short press (Select button forwarded
     * to the machine) from a long press (on-screen keyboard), exactly like
     * "+". We keep a SILENT binding to stop Borealis from swallowing the
     * button, without which draw() would never see it. */
    this->registerAction(
        ui::tr("action/keyboard"),
        brls::ControllerButton::BUTTON_BACK,
        [sv](brls::View *) {
            (void)sv;
            return true;   /* consumed, the real logic is in draw() */
        }, /*hidden=*/true);

    // B button: closes the menu if it is open. Otherwise let it through (= the
    // right click, sent by the stream's own pad handling). Start (+)
    // stays the only trigger that opens the menu on the Borealis side.
    this->registerAction(
        ui::tr("action/close_menu"),
        brls::ControllerButton::BUTTON_B,
        [sv](brls::View *) {
            /* Tester open: B is CONSUMED and does nothing. It must show up as
             * an ordinary button - that is the whole point of the tester - and
             * above all it must not travel to the remote machine as a right
             * click while you are testing it. The hold that closes the tester
             * is measured in ui/pad_test. */
            /* HOLD-1 2026-09-12 - the other two testers were missing from this
             * list, and on desktop that is what the user heard: B was not
             * consumed, so every press meant to leave the link-quality page
             * travelled to the VM as a right click while the page stayed open.
             * All three now behave the same way - the hold that closes them is
             * measured inside the page, from the button's raw state, which the
             * action system never sees. */
            if (sv->padTestOpen() || sv->netTestOpen() || sv->mouseTestOpen())
                return true;
            if (sv->isMenuOpen()) {
                sv->menuBack();   // go up one page, closes at the root
                return true;
            }
            return false;  // not consumed -> the pump can right-click
        });

    // Up: move up the menu list
    this->registerAction(
        "",
        brls::ControllerButton::BUTTON_UP,
        [sv](brls::View *) {
            if (sv->isMenuOpen()) {
                /* S97: the view reads the direction every frame (stick +
                 * repeat). We CONSUME without acting, otherwise each press
                 * would count twice. */
                return true;
            }
            return false;
        });

    // Down: move down the menu list
    this->registerAction(
        "",
        brls::ControllerButton::BUTTON_DOWN,
        [sv](brls::View *) {
            if (sv->isMenuOpen()) {
                /* S97: the view reads the direction every frame (stick +
                 * repeat). We CONSUME without acting, otherwise each press
                 * would count twice. */
                return true;
            }
            return false;
        });

    // A: activates the selected entry. Every entry carries its own action, so
    // there is no index left to match against a switch.
    this->registerAction(
        ui::tr("action/validate"),
        brls::ControllerButton::BUTTON_A,
        [sv](brls::View *) {
            if (!sv->isMenuOpen()) return false;
            sv->menuActivate();
            return true;
        });

    /* S106 - Y opens the DETAIL of the focused entry, when it has one.
     * Registered alongside the other menu buttons, and returning `false`
     * outside the menu so it does not steal Y from the screen underneath. */
    this->registerAction(
        "",
        brls::ControllerButton::BUTTON_Y,
        [sv](brls::View *) {
            if (!sv->isMenuOpen()) return false;
            return sv->menuDetail();
        }, /*hidden=*/true);

    // Left / Right: adjust the selected entry (touch mode, sections).
    this->registerAction(
        "",
        brls::ControllerButton::BUTTON_LEFT,
        [sv](brls::View *) {
            if (!sv->isMenuOpen()) return false;
            /* S97: the view reads the direction every frame (stick + repeat).
             * We CONSUME without acting, otherwise each press would count
             * twice. */
            return true;
        }, /*hidden=*/true);
    this->registerAction(
        "",
        brls::ControllerButton::BUTTON_RIGHT,
        [sv](brls::View *) {
            if (!sv->isMenuOpen()) return false;
            /* S97: the view reads the direction every frame (stick + repeat).
             * We CONSUME without acting, otherwise each press would count
             * twice. */
            return true;
        }, /*hidden=*/true);

    buildPauseMenu(sv);
}
