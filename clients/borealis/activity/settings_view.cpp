/* SettingsView - see the header for why this screen was ported. */

#include "../device_caps.h"
#include "../ui/haptics.hpp"
#include "../ui/pointer.hpp"
#include "settings_view.hpp"
#include "../../../core/protocol/bitrate.h"   /* B1: the ONE bitrate ladder */

#include "activity/quality_settings_activity.hpp"
#include "activity/pad_settings_activity.hpp"
#include "activity/shadow_app.hpp"
#include "settings.hpp"

extern "C" {
#include "../../../core/media/audio.h"
#include "../../../core/common/log.h"
#include "../../../core/services/journal.h"
}
#include "../ui/i18n.hpp"
#include "../ui/env_note.hpp"
#include "activity/lock_activity.hpp"

extern "C" {
#include "../../../core/services/applock_store.h"
#include "core/services/oauth.h"
}
#include "../ui/sfx.hpp"
#include "core/version.h"          /* S85 - version, build date, git hash */

#include <chrono>

namespace {

/* The same tables as the old screen. They stay local: exposing them would
 * suggest another screen could use them, when they only make sense here. */
const uint32_t SCALES[] = { 75, 100, 125, 150, 200 };
/* B1 - the per-link rates read the SHARED ladder (streaming/bitrate.h). This
 * table used to be its own, with 25 and 75 that no other screen knew: choosing
 * one of them made the quality screen display "Auto" (its lookup returned 0 on
 * a miss, and index 0 is Auto) and the pause menu jump to 5 Mbps. */

struct Lang { const char *code; const char *label; };
const Lang LANGS[] = {
    { "",      nullptr },        /* automatic - label translated at use */
    { "fr",    "Fran\u00E7ais" },
    { "en-US", "English"  },
};

template <size_t N>
int indexOf(const uint32_t (&table)[N], uint32_t value, int fallback)
{
    for (size_t i = 0; i < N; i++) if (table[i] == value) return (int)i;
    return fallback;
}

int languageIndex()
{
    const std::string &cur = Settings::instance().language;
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++)
        if (cur == LANGS[i].code) return (int)i;
    return 0;
}

/* Factories: they save repeating six fields at every entry, and above all they
 * guarantee that a kind never leaves without what it requires (a toggle with no
 * state, a choice with no values).
 *
 * S79 - the `titre()` factory disappeared along with the section separators: a
 * section IS the screen's section now, and a section heading inside a
 * three-line section would no longer separate anything. */
ui::Item toggle(int id, const std::string &t, const std::string &desc, bool on)
{
    ui::Item i; i.kind = ui::Kind::Toggle; i.id = id;
    i.title = t; i.subtitle = desc; i.lit = on;
    return i;
}

ui::Item choice(int id, const std::string &t, const std::string &desc,
                std::vector<std::string> values, int index)
{
    ui::Item i; i.kind = ui::Kind::Choice; i.id = id;
    i.title = t; i.subtitle = desc;
    i.choice = std::move(values);
    i.choice_index = index;
    return i;
}

ui::Item action(int id, const std::string &t, const std::string &desc)
{
    ui::Item i; i.kind = ui::Kind::Action; i.id = id;
    i.title = t; i.subtitle = desc;
    return i;
}

/* A row that SHOWS and does nothing (S85).
 *
 * `actionnable = false` makes it non-focusable (`ui_focusable`, screen.hpp):
 * navigation skips it and A cannot fire on it. That is the point - a row that
 * takes the focus and then does not answer A reads as a malfunction, exactly
 * the counter-case screen.hpp documents for section separators. The kind stays
 * `Action` because that is the one that displays `valeur` on the right; there
 * is nothing to activate behind it.
 *
 * ACCEPTED CONSEQUENCE: a section made ONLY of these rows - "A propos" - has no
 * focus at all. `setItems` returns -1 there, nothing is outlined, and up/down
 * do not consume. That is the documented behaviour, not an oversight. */
ui::Item info(int id, const std::string &t, const std::string &value,
              const std::string &desc = "")
{
    ui::Item i; i.kind = ui::Kind::Action; i.id = id;
    i.title = t; i.value = value; i.subtitle = desc;
    i.actionable = false;
    return i;
}

/* Human-readable size.
 *
 * BOTH halves of the number are passed to the catalogue separately: the decimal
 * separator is a comma in French and a dot in English, and it is the
 * catalogue's job to say so. Coding it here would show "2.4 Mo" to a French
 * reader, and there would be no place to fix it short of touching the code -
 * which is the definition of a hard-coded string. */
std::string humanSize(uint64_t bytes)
{
    if (bytes < 1024ull)
        return ui::tr("settings/size_b", (unsigned long long)bytes);
    if (bytes < 1024ull * 1024ull)
        return ui::tr("settings/size_kb",
                      (unsigned long long)((bytes + 512ull) / 1024ull));

    /* A tenth of a megabyte is enough: what we want to know is whether the log
     * weighs "nothing" or "something", not to count bytes. The arithmetic is
     * done in integers - a `double` would round "1023,96" into "1024,0 Mo". */
    const uint64_t tenths = (bytes * 10ull + 524288ull) / (1024ull * 1024ull);
    return ui::tr("settings/size_mb", (unsigned long long)(tenths / 10ull),
                                      (unsigned long long)(tenths % 10ull));
}

}  // namespace

SettingsView::SettingsView()
{
    /* The screen's only focusable view, alive as long as the activity: that is
     * what makes a dangling focus pointer impossible. */

    screen_.setTitle(ui::tr("settings/title"));
    /* The trigger hint comes FIRST: we read left to right, and the console's
     * convention puts the primary action nearest the right edge. It is
     * indispensable - it is the only place one learns that L and R change
     * section, since there is neither hover nor context menu on a console. */
    screen_.setHints({
        { "LR", ui::tr("settings/section_hint") },
        { "B",  ui::tr("action/back") },
        { "A",  ui::tr("action/ok")   },
    });
    rebuild();
}

bool SettingsView::moveSection(int dir)
{
    if (!screen_.shiftSection(dir)) return false;
    rebuild();
    return true;
}

/* === S79 - THE SECTION TABLE ===
 *
 * It is here, spelled out, and not derived from the old headings: those
 * followed the historical order in which things were added, not an
 * organization. "Audio" and "Decodage" came one after the other without that
 * adjacency meaning anything, and the three sub-pages were bunched at the top
 * instead of being filed under the subject they belong to.
 *
 * So each section carries a SUBJECT, and the sub-pages are filed under it:
 * video quality under "Image", the gamepad and the gestures under
 * "Commandes". Filing things is the only thing this screen has to offer. */
namespace {

struct SectionDef { int id; const char *key; };

const SectionDef SECTIONS[] = {
    { SEC_IMAGE,      "settings/sec_video"     },
    { SEC_AUDIO,      "settings/sec_audio"     },
    { SEC_CONTROLS,   "settings/sec_controls" },
    { SEC_CONNECTION, "settings/sec_connection" },
    { SEC_SESSION,    "settings/sec_session"   },
    { SEC_INTERFACE,  "settings/sec_interface" },
    { SEC_SECURITY,   "settings/sec_security"  },
    { SEC_ADVANCED,   "settings/sec_advanced"    },
    { SEC_ACCOUNT,    "settings/sec_account"    },
    /* S85 - "A propos" comes LAST: nobody consults it, you go there when you
     * have to say which version you are running. Putting it higher would mean
     * crossing it every single time you are looking for something else. */
    { SEC_ABOUT,      "settings/about"         },
};

}  // namespace

/* The content of ONE section. The `switch` has no `default` that would fill in
 * "something": an unknown section returns an EMPTY list, which `ui::ListScreen`
 * shows with its empty-state message. Inventing fallback content would hide
 * precisely the mistake we want to see - a section present in the column and
 * absent from here. */
void SettingsView::build(int r, std::vector<ui::Item> &v)
{
    Settings &s = Settings::instance();

    switch (r) {
    case SEC_IMAGE:
        v.push_back(action(SET_VIDEO, ui::tr("settings/video"),
                           ui::tr("settings/video_desc")));
        v.push_back(toggle(SET_AUTO_RES, ui::tr("settings/auto_res"),
                           ui::tr("settings/auto_res_desc"), s.auto_resolution));
#if SHADOW_HAS_DECODER_CHOICE
        v.push_back(toggle(SET_HW_VIDEO, ui::tr("settings/hw_video"),
                           ui::envNote("SHADOW_HWACCEL", ui::tr("settings/hw_video_desc")), s.hw_decode));
#endif
        v.push_back(toggle(SET_RATE_LINK, ui::tr("settings/rate_link"),
                           ui::tr("settings/rate_link_desc"), s.bitrate_per_link));
        /* 2026-09-02 - the stretch. It existed only as `SHADOW_STRETCH`, read
         * once into a function `static` on the first frame and toggled from a
         * desktop-only menu bar: on console it was reachable by no means at
         * all. Same family as B4 above. */
        v.push_back(toggle(SET_STRETCH, ui::tr("settings/stretch"),
                           ui::envNote("SHADOW_STRETCH", ui::tr("settings/stretch_desc")),
                           s.stretch_image));
        /* S113 - the cursor. Three sources, none of them right everywhere: see
         * the comment on `curseur_source` in settings.hpp. */
        {
            std::vector<std::string> srcs = { ui::tr("settings/cursor_none"),
                                              ui::tr("settings/cursor_image"),
                                              ui::tr("settings/cursor_arrow") };
            v.push_back(choice(SET_CURSOR, ui::tr("settings/cursor"),
                               ui::tr("settings/cursor_desc"), srcs,
                               (int)(s.cursor_source <= 2 ? s.cursor_source : 0)));
        }
        /* The two bitrates appear ONLY if the setting that governs them is on.
         * Showing them greyed out would be steadier on screen, but two inert
         * rows in the middle of a page read as a malfunction. */
        if (s.bitrate_per_link) {
            /* Rung 0 (Auto) is offered here too: "follow the client default on
             * Wi-Fi, cap Ethernet" is a legitimate choice, and leaving it out
             * was the reason these two rows and the quality row could not be
             * expressed in the same vocabulary. */
            std::vector<std::string> labels;
            labels.push_back(ui::tr("quality/auto") + " (" +
                             std::to_string(BITRATE_CLIENT_DEFAULT_MBPS) + " Mb/s)");
            for (int i = 1; i < BITRATE_LADDER_N; i++)
                labels.push_back(std::to_string(BITRATE_LADDER[i]) + " Mb/s");
            v.push_back(choice(SET_RATE_WIFI, ui::tr("settings/rate_wifi"),
                               ui::tr("settings/rate_wifi_desc"),
                               labels, bitrate_index(s.bitrate_wifi_mbps)));
            v.push_back(choice(SET_RATE_ETH, ui::tr("settings/rate_eth"),
                               ui::tr("settings/rate_eth_desc"),
                               labels, bitrate_index(s.bitrate_eth_mbps)));
        }
        break;

    case SEC_AUDIO: {
        std::vector<std::string> vols;
        static const uint32_t STEPS[] = { 0, 25, 50, 75, 100, 125, 150, 200, 250, 300 };
        int idx = 4;
        for (size_t k = 0; k < sizeof STEPS / sizeof STEPS[0]; k++) {
            vols.push_back(std::to_string(STEPS[k]) + " %");
            if (STEPS[k] == s.audio_volume) idx = (int)k;
        }
        v.push_back(choice(SET_VOLUME, ui::tr("menu/volume"),
                           ui::envNote("SHADOW_VOLUME", ui::tr("menu/volume_desc")), vols, idx));
        /* K14 - audio quality is a CODEC: lossy Opus against lossless FLAC. It
         * is negotiated when the session opens, hence saying so explicitly in
         * the description: a setting that looks inert is worse than no setting
         * at all. */
        v.push_back(toggle(SET_AUDIO_HIFI, ui::tr("menu/audio_quality"),
                           ui::envNote("SHADOW_AUDIO_CODEC", ui::tr("menu/audio_quality_desc")), s.audio_hifi));
#ifdef __SWITCH__
        /* AUD-CFG-2 2026-09-11 - console only: SHADOW_HWOPUS is read under
         * HAVE_AUDOUT alone (media/audio.c), so off console this row switched
         * nothing. Rows are handled by id (SET_HW_AUDIO), so removing it shifts
         * no other row. */
        v.push_back(toggle(SET_HW_AUDIO, ui::tr("settings/hw_audio"),
                           ui::envNote("SHADOW_HWOPUS", ui::tr("settings/hw_audio_desc")), s.hw_opus));
#endif
        /* S90 - the equalizer has its own page: five bands with four parameters
         * each do not fit in a shared section, and it needs its response curve
         * next to it. */
        v.push_back(action(SET_EQ, ui::tr("eq/title"), ui::tr("settings/eq_desc")));
        break;
    }

    case SEC_CONTROLS: {
        v.push_back(action(SET_GAMEPAD, ui::tr("settings/gamepad"),
                           ui::tr("settings/gamepad_desc")));
        v.push_back(action(SET_GESTURES, ui::tr("settings/gestures"),
                           ui::tr("settings/gestures_desc")));
        v.push_back(toggle(SET_PAD_ENABLE, ui::tr("settings/pad_enable"),
                           ui::envNote("SHADOW_GAMEPAD_PLUG", ui::tr("settings/pad_enable_desc")), s.gamepad_enabled));
        v.push_back(toggle(SET_MOUSE_REL, ui::tr("menu/mouse_relative"),
                           ui::envNote("SHADOW_INPUT_ABS", ui::tr("menu/mouse_relative_desc")), s.mouse_relative));
        std::vector<std::string> sensitivities;
        static const uint32_t STEPS[] = { 10, 15, 20, 25, 35, 50, 75, 100, 150, 200 };
        int idx = 7;
        for (size_t k = 0; k < sizeof STEPS / sizeof STEPS[0]; k++) {
            sensitivities.push_back(std::to_string(STEPS[k]) + " %");
            if (STEPS[k] == s.touch_sensitivity) idx = (int)k;
        }
        v.push_back(choice(SET_TOUCH_SENS, ui::tr("menu/touch_sens"),
                           ui::tr("menu/touch_sens_desc"), sensitivities, idx));
        v.push_back(choice(SET_KB_LAYOUT, ui::tr("settings/kb_layout"),
                           ui::tr("settings/keyboard"),
                           { "AZERTY", "QWERTY" },
                           s.kb_layout == Settings::KbLayout::Qwerty ? 1 : 0));
        /* CLIP4 - the clipboard, among the controls rather than in "Session":
         * it is something the user DOES with a keyboard, next to the layout and
         * the mouse mode, not something the session is configured with.
         *
         * Hidden where there is no local clipboard to share (console): see
         * SHADOW_HAS_CLIPBOARD in device_caps.h. */
#if SHADOW_HAS_CLIPBOARD
        {
            std::vector<std::string> dirs = { ui::tr("settings/clipboard_off"),
                                              ui::tr("settings/clipboard_both"),
                                              ui::tr("settings/clipboard_to_vm"),
                                              ui::tr("settings/clipboard_to_pc") };
            v.push_back(choice(SET_CLIPBOARD, ui::tr("settings/clipboard"),
                               ui::envNote("SHADOW_CLIPBOARD",
                                           ui::tr("settings/clipboard_desc")),
                               dirs,
                               (int)(s.clipboard_mode <= 3 ? s.clipboard_mode : 1)));
        }
#endif
        break;
    }

    case SEC_CONNECTION:
        v.push_back(toggle(SET_AUTO_CONNECT, ui::tr("settings/auto_connect"),
                           ui::tr("settings/auto_connect_desc"), s.auto_connect));
        /* B4 - the video transport. It existed only as `SHADOW_VIDEO_NET_TCP`,
         * i.e. as a file to drop on the SD card - unreachable from the console,
         * which is the same "a capability that exists with no setting" family
         * corrected on 2026-08-27 for six others. Its description states the
         * cost, because the cost is not guessable: in TCP the server refuses
         * `EnableDynamicBitrate` and the adaptive bitrate stops. */
        v.push_back(toggle(SET_VIDEO_TCP, ui::tr("settings/video_tcp"),
                           ui::envNote("SHADOW_VIDEO_NET_TCP",
                                       ui::tr("settings/video_tcp_desc")),
                           s.video_tcp));
        v.push_back(toggle(SET_REG_INPUT, ui::tr("settings/reg_input"),
                           ui::tr("settings/reg_input_desc"), s.udp_register_input));
        /* NET1 - the address family. In "Connexion" because that is what it
         * governs, and its description carries the COST rather than the
         * mechanism: "half of every retry round spent on an address that never
         * answers" is what a user can act on. */
        {
            std::vector<std::string> fams = { ui::tr("settings/net_auto"),
                                              ui::tr("settings/net_v4"),
                                              ui::tr("settings/net_v6") };
            const int fi = (s.net_family == 4) ? 1 : (s.net_family == 6) ? 2 : 0;
            v.push_back(choice(SET_NET_FAMILY, ui::tr("settings/net_family"),
                               ui::envNote("SHADOW_FORCE_IPV4",
                                           ui::tr("settings/net_family_desc")),
                               fams, fi));
        }
        break;

    case SEC_SESSION:
        /* This setting earns its explanation: "perte de focus" means nothing to
         * someone who presses HOME and watches their session vanish. */
#if SHADOW_HAS_APPLET_EVENTS
        v.push_back(toggle(SET_FOCUS, ui::tr("settings/focus_cut"),
                           ui::tr("settings/focus_cut_desc"), s.cut_when_unfocused));
#endif
        v.push_back(toggle(SET_RECO, ui::tr("settings/reco"),
                           ui::tr("settings/reco_desc"), s.auto_reconnect));
        if (s.auto_reconnect) {
            std::vector<std::string> n;
            for (int k = 1; k <= 5; k++) n.push_back(std::to_string(k));
            v.push_back(choice(SET_RECO_MAX, ui::tr("settings/reco_max"), "",
                               n, (int)s.reconnect_max - 1));
        }
        break;

    case SEC_INTERFACE: {
        /* === S88 2026-08-29 - THE INTERFACE SOUNDS ===
         * In "Interface" and not in "Sound": the Sound section governs what comes
         * from the REMOTE MACHINE - stream volume, codec, hardware decoding.
         * The menu beeps have nothing to do with that, they belong to the
         * application's look and feel. Filing them together would send people
         * here looking for the game volume, and the other way round. */
        v.push_back(toggle(SET_SOUNDS, ui::tr("settings/sounds"),
                           ui::tr("settings/sounds_desc"), s.sounds));
        /* The volume appears ONLY when the sounds are on. Same rule as the two
         * bitrates or the retry count: an inert row in the middle of a page
         * reads as a malfunction. */
        if (s.sounds) {
            std::vector<std::string> vols;
            static const uint32_t STEPS[] = { 20, 40, 60, 70, 80, 100 };
            int idx = 3;
            for (size_t k = 0; k < sizeof STEPS / sizeof STEPS[0]; k++) {
                vols.push_back(std::to_string(STEPS[k]) + " %");
                if (STEPS[k] == s.sounds_volume) idx = (int)k;
            }
            v.push_back(choice(SET_SOUNDS_VOL, ui::tr("settings/sound_volume"),
                               ui::tr("settings/sound_volume_desc"), vols, idx));
        }

#if SHADOW_HAS_RUMBLE
        /* S105 - haptic feedback, next to the sounds: these are the interface's
         * two non-visual feedbacks, and they are set together. This one is
         * always visible, though: unlike the volume it depends on no toggle -
         * "0 %" IS how it is turned off. */
        {
            std::vector<std::string> hv;
            static const uint32_t HAPTIC_STEPS[] = { 0, 20, 40, 60, 80, 100 };
            int idxh = 3;
            for (size_t k = 0; k < sizeof HAPTIC_STEPS / sizeof HAPTIC_STEPS[0]; k++) {
                hv.push_back(std::to_string(HAPTIC_STEPS[k]) + " %");
                if (HAPTIC_STEPS[k] == s.haptics_strength) idxh = (int)k;
            }
            v.push_back(choice(SET_HAPTICS, ui::tr("settings/haptics"),
                               ui::tr("settings/haptics_desc"), hv, idxh));
        }
#endif

        std::vector<std::string> scales;
        for (uint32_t k : SCALES) scales.push_back(std::to_string(k) + " %");
        v.push_back(choice(SET_UI_SCALE, ui::tr("menu/ui_scale"),
                           ui::tr("settings/interface_desc"),
                           scales, indexOf(SCALES, s.ui_scale, 1)));
        std::vector<std::string> languages;
        for (const Lang &l : LANGS)
            languages.push_back(l.label ? l.label : ui::tr("settings/language_auto"));
        v.push_back(choice(SET_LANGUAGE, ui::tr("settings/language"),
                           ui::tr("settings/language_desc"), languages, languageIndex()));
        break;
    }

    case SEC_SECURITY: {
        /* === THE APPLICATION LOCK (2026-09-02) ===
         *
         * The state comes FIRST and as a plain sentence, because "is my console
         * locked?" is the only question this page really answers and it must be
         * answerable without reading three toggles.
         *
         * It is read from the CARD on every rebuild, not from a cached flag:
         * the record is the truth, it can be deleted from a computer, and a
         * page that showed a stale "locked" would be the worst possible lie
         * here. `rebuild()` runs on open, on a section change and after an
         * action - exactly when the answer can have changed. */
        applock_record_t rec;
        const bool armed = applock_store_load(&rec) != 0;
        const uint32_t usable = armed ? applock_usable_methods(&rec) : 0u;

        v.push_back(info(SET_LOCK_STATE, ui::tr("settings/lock_state"),
                         armed ? ui::tr("settings/lock_on") : ui::tr("settings/lock_off"),
                         armed ? ui::tr("settings/lock_state_desc")
                               : ui::tr("settings/lock_state_off_desc")));

        /* One row per method, each saying whether it is set. Choosing one opens
         * the entry screen; choosing one already set REPLACES it, which is why
         * the row does not read as a toggle - a toggle would suggest you can
         * turn a PIN back on without retyping it, and you cannot: we keep no
         * plaintext to turn back on. */
        v.push_back(action(SET_LOCK_PIN, ui::tr("settings/lock_pin"),
                           (usable & APPLOCK_PIN) ? ui::tr("settings/lock_set_change")
                                                  : ui::tr("settings/lock_pin_desc")));
        v.push_back(action(SET_LOCK_PATTERN, ui::tr("settings/lock_pattern"),
                           (usable & APPLOCK_PATTERN) ? ui::tr("settings/lock_set_change")
                                                      : ui::tr("settings/lock_pattern_desc")));
        v.push_back(action(SET_LOCK_PASSWORD, ui::tr("settings/lock_password"),
                           (usable & APPLOCK_PASSWORD) ? ui::tr("settings/lock_set_change")
                                                       : ui::tr("settings/lock_password_desc")));

        /* Only when there is a lock to re-arm: an inert toggle above an unarmed
         * lock reads as a malfunction, the same reasoning as the two per-link
         * bitrates in the Image section. */
        if (armed) {
#if SHADOW_HAS_WAKE_LOCK
            /* On console the applet hook re-arms the lock for real; on the
             * desktop only AF2's `SHADOW_DIAG_RELOCK_S` does, and it needs
             * this switch on. Where neither exists the row would promise a
             * protection that never happens. */
            v.push_back(toggle(SET_LOCK_WAKE, ui::tr("settings/lock_wake"),
                               ui::tr("settings/lock_wake_desc"), s.lock_on_wake));
#endif
            v.push_back(action(SET_LOCK_REMOVE, ui::tr("settings/lock_remove"),
                               ui::tr("settings/lock_remove_desc")));
        }
        break;
    }

    case SEC_ADVANCED:
        /* === S81 - THE LOG LEVEL ===
         * A four-step choice rather than a "debug on/off" toggle: between
         * "nothing" and "everything" there are two useful positions, and the
         * difference between them is several megabytes per session written to
         * the SD card. Each step's label says what it writes, not its number. */
        v.push_back(choice(SET_LOG_LEVEL, ui::tr("settings/log"),
                           ui::envNote("SHADOW_JOURNAL_NIVEAU", ui::tr("settings/log_desc")),
                           { ui::tr("settings/log_warn"),
                             ui::tr("settings/log_info"),
                             ui::tr("settings/log_debug"),
                             ui::tr("settings/log_trace") },
                           (int)s.log_level - 1));
        v.push_back(toggle(SET_DEV_CHANNEL, ui::tr("settings/dev_channel"),
                           ui::tr("settings/dev_channel_desc"), s.dev_channel));
        /* The connect action comes RIGHT AFTER the toggle, and only when the
         * toggle is on: the channel only attempts its connection at startup, so
         * arming it here used to require restarting the application. An inert
         * entry above a switched-off toggle would be a trap; we only show it
         * when it does something. */
        if (s.dev_channel)
            v.push_back(action(SET_DEV_CHANNEL_NOW, ui::tr("settings/dev_channel_now"),
                               ui::tr("settings/dev_channel_now_desc")));

        /* === S86 2026-08-29 - THE LOGS THEMSELVES ===
         *
         * S81 gave the dial (the level) without the instrument: you set the
         * verbosity without being able to see what was written, or to know what
         * it took up on the card, or to start clean before reproducing a defect.
         * The three entries below close that gap.
         *
         * The inventory is read HERE, in `rebuild()`, and NOWHERE else:
         * `journal_taille()` does about ten `stat` calls on the SD card. Putting
         * it in a Borealis `info()` would run it sixty times a second - that is
         * the G47 fault word for word, which dropped the interface to 5 fps.
         * `rebuild()` is called on open, on a section change and after an
         * action: that is, exactly when the number can have changed. */
        {
            journal_size_t jt;
            journal_size(&jt);
            v.push_back(info(SET_LOG_SIZE, ui::tr("settings/log_size"),
                             humanSize(jt.bytes),
                             ui::tr("settings/log_size_desc",
                                    jt.files, journal_sessions_kept(),
                                    humanSize(jt.bytes_current))));
            v.push_back(action(SET_LOG_VIEW, ui::tr("settings/log_view"),
                               ui::tr("settings/log_view_desc")));
            v.push_back(action(SET_LOG_CLEAR, ui::tr("settings/log_clear"),
                               ui::tr("settings/log_clear_desc")));
        }

        /* === 2026-09-02 - WHAT env.txt HAS TAKEN OVER ===
         *
         * The individual rows carry their own warning (see ui/env_note.hpp), but
         * they are scattered over six sections and you only see the warning if
         * you happen to walk past the row. This one row answers the question
         * you actually ask - "is anything overriding me right now?" - without
         * having to guess where to look. It stays visible when the answer is
         * "none": a row that only appears when something is wrong cannot be
         * used to check that nothing is. */
        v.push_back(info(SET_ENV_OVER, ui::tr("settings/env_over"),
                         env_override_count()
                           ? std::to_string(env_override_count())
                           : ui::tr("settings/env_over_none"),
                         env_override_count()
                           ? std::string(env_override_summary())
                           : ui::tr("settings/env_over_desc")));

        /* Last of the section, and after the log entries: it is the most
         * destructive action on this screen, and the one you reach for least
         * often. */
        v.push_back(action(SET_RESET, ui::tr("settings/reset"),
                           ui::tr("settings/reset_desc")));
        break;

    case SEC_ACCOUNT:
        v.push_back(action(SET_LOGOUT, ui::tr("action/logout"),
                           ui::tr("settings/logout_desc")));

        /* === S110 - REPLAY THE JOURNEY WITHOUT CLOSING ===
         * To film the application end to end you have to be able to go back to
         * the beginning WITHOUT the window disappearing: the ordinary logout
         * quits, which cuts the recording in two. Two entries, because they do
         * not answer the same question: replay the loading, or replay the
         * pairing. */
        v.push_back(action(SET_REPLAY, ui::tr("settings/replay"),
                           ui::tr("settings/replay_desc")));
        v.push_back(action(SET_REPLAY_AUTH, ui::tr("settings/replay_auth"),
                           ui::tr("settings/replay_auth_desc")));
#if SHADOW_HAS_DEMO_POINTER
        /* S110 - desktop only: the console has no mouse, and offering a setting
         * with nothing behind it casts doubt on all the others.
         *
         * The condition WAS `#if !defined(__SWITCH__)`, and that is precisely
         * the subtractive form this port keeps paying for: it meant "desktop"
         * only while the Switch was the sole console, so the day the Vita
         * arrived this row silently came back - on a machine with no mouse,
         * for the exact reason the comment above gives. */
        v.push_back(toggle(SET_POINTER, ui::tr("settings/pointer"),
                           ui::tr("settings/pointer_desc"), s.demo_pointer));
#endif
        break;

    /* === S85 2026-08-29 - THE BUILD FINGERPRINT, READABLE ON THE CONSOLE ===
     *
     * THREE rows and not one glued-together string. They answer three different
     * questions - which version, from when, which code - and the only one that
     * matters for a test round trip is the third. Glued into
     * "0.3.0 2026-08-29 01:10:03Z 3b04cc7" they need a magnifying glass, and
     * the hash, the one thing you have to copy out, is lost in the middle.
     *
     * This is the only place where you can check, console in hand, that the
     * .nro that launched is the one you just pushed. The startup error screen
     * said it already, but only when startup FAILS - so never when you need it,
     * which is when everything starts and you doubt the binary.
     */
    case SEC_ABOUT:
        v.push_back(info(SET_A_VERSION, ui::tr("settings/about_version"),
                         SHADOW_VERSION, ui::tr("settings/about_version_desc")));
        v.push_back(info(SET_A_DATE, ui::tr("settings/about_date"),
                         SHADOW_BUILD_DATE, ui::tr("settings/about_date_desc")));
        v.push_back(info(SET_A_HASH, ui::tr("settings/about_hash"),
                         SHADOW_BUILD_HASH, ui::tr("settings/about_hash_desc")));

        /* === WHAT A PUBLISHED GPL BUILD OWES THE PERSON HOLDING IT ========
         *
         * The licence and the source URL are not decoration. GPLv3 asks that a
         * distributed work carry its legal notices, and that whoever has the
         * binary can find the corresponding source - on a console there is no
         * `--version` to run and no directory to browse, so this screen IS the
         * only place either can be read.
         *
         * All four values come from `build_id.h`, generated by CMake from the
         * single identity block in `CMakeLists.txt`. Nothing here is typed a
         * second time - that is what `version.h` exists to prevent. */
        if (SHADOW_BUILD_TAG[0])
            v.push_back(info(SET_A_TAG, ui::tr("settings/about_tag"),
                             SHADOW_BUILD_TAG, ui::tr("settings/about_tag_desc")));
        v.push_back(info(SET_A_LICENSE, ui::tr("settings/about_license"),
                         SHADOW_APP_LICENSE, ui::tr("settings/about_license_desc")));
        v.push_back(info(SET_A_SOURCE, ui::tr("settings/about_source"),
                         SHADOW_APP_URL, ui::tr("settings/about_source_desc")));
        v.push_back(info(SET_A_AUTHOR, ui::tr("settings/about_author"),
                         SHADOW_APP_AUTHOR, ui::tr("settings/about_author_desc")));

        /* === S107 - PLAY TIME ===
         * Shadow caps the hours: knowing where you stand, from the console, is
         * not a convenience. The description says this count is OURS - a figure
         * one believes official and that is not is worse than no figure. */
        {
            auto hhmm = [](uint32_t sec) {
                char b[32];
                snprintf(b, sizeof b, "%u h %02u", sec / 3600, (sec % 3600) / 60);
                return std::string(b);
            };
            v.push_back(info(SET_A_TIME_DAY, ui::tr("settings/time_today"),
                             hhmm(s.time_day_s), ui::tr("settings/time_today_desc")));
            v.push_back(info(SET_A_TIME_MONTH, ui::tr("settings/time_month"),
                             hhmm(s.time_month_s), ui::tr("settings/time_month_desc")));
            v.push_back(info(SET_A_TIME_TOTAL, ui::tr("settings/time_total"),
                             hhmm(s.time_total_s), ui::tr("settings/time_total_desc")));
        }
        break;
    }
}

void SettingsView::rebuild()
{
    /* The column and the content are laid down in the SAME call, and the
     * content is that of the section the column declares open. That is what
     * keeps them from diverging: there exists no place where one could change
     * without the other. */
    std::vector<ui::Section> sections;
    sections.reserve(sizeof SECTIONS / sizeof SECTIONS[0]);
    for (const SectionDef &r : SECTIONS) {
        ui::Section u;
        u.id    = r.id;
        u.title = ui::tr(r.key);
        sections.push_back(std::move(u));
    }
    /* -1 on the first call: setSections then falls back to the first one. */
    screen_.setSections(std::move(sections), screen_.activeSection());

    std::vector<ui::Item> v;
    build(screen_.activeSection(), v);
    screen_.setItems(std::move(v));

    /* The subtitle names the open section. Redundant with the column, and
     * deliberately so: it is what stays readable when the column falls outside
     * the picture on a TV that overscans. */
    for (const SectionDef &r : SECTIONS)
        if (r.id == screen_.activeSection()) {
            screen_.setSubtitle(ui::tr(r.key));
            break;
        }
}

bool SettingsView::apply(const ui::Item &it)
{
    Settings &s = Settings::instance();
    bool rebuild_needed = false;   /* some settings change the LIST itself */

    switch (it.id) {
        case SET_AUTO_CONNECT: s.auto_connect       = it.lit; break;
        case SET_FOCUS:        s.cut_when_unfocused = it.lit; break;
        case SET_HW_VIDEO:     s.hw_decode    = it.lit; break;
        case SET_AUDIO_HIFI:   s.audio_hifi           = it.lit; break;
        case SET_HW_AUDIO:     s.hw_opus        = it.lit; break;
        case SET_MOUSE_REL:    s.mouse_relative       = it.lit; break;
        /* This one makes the retry count appear: it has to rebuild, not just
         * save. */
        case SET_RECO:         s.auto_reconnect = it.lit; rebuild_needed = true; break;
        case SET_DEV_CHANNEL:
            s.dev_channel = it.lit;
            s.dev_channel_known = true;   /* explicit choice: ours to apply now */
            rebuild_needed = true;      /* makes "connecter maintenant" appear */
            /* Takes effect immediately: the toggle renames the destination
             * file, without waiting for a restart. */
            s.save(); s.applyToggles(); return true;
        /* Takes effect on the NEXT connection: the transport is decided in the
         * channel announcement, at bootstrap. The description says so rather
         * than letting it look like a toggle that does nothing. */
        case SET_VIDEO_TCP:    s.video_tcp          = it.lit; break;
        case SET_STRETCH:      s.stretch_image      = it.lit; break;
        case SET_LOCK_WAKE:    s.lock_on_wake       = it.lit; break;
        case SET_PAD_ENABLE:   s.gamepad_enabled    = it.lit; break;
        case SET_REG_INPUT:    s.udp_register_input = it.lit; break;
        case SET_AUTO_RES:     s.auto_resolution    = it.lit; break;

        /* This one makes two entries appear or disappear: it has to rebuild,
         * not just save. */
        /* Turning it on or off changes WHICH value is in force, so it applies
         * live too - otherwise the toggle appears to do nothing until the next
         * connection. */
        case SET_RATE_LINK:
            s.bitrate_per_link = it.lit;
            s.applyBitrateLive();
            rebuild_needed = true;
            break;

        case SET_RECO_MAX:
            if (it.choice_index >= 0 && it.choice_index < 5)
                s.reconnect_max = (uint32_t)it.choice_index + 1;
            break;
        case SET_VOLUME: {
            static const uint32_t STEPS[] = { 0, 25, 50, 75, 100, 125, 150, 200, 250, 300 };
            if (it.choice_index >= 0 && it.choice_index < (int)(sizeof STEPS / sizeof STEPS[0])) {
                s.audio_volume = STEPS[it.choice_index];
                audio_set_volume(s.audio_volume);   /* immediate effect */
            }
            break;
        }
        case SET_TOUCH_SENS: {
            static const uint32_t STEPS[] = { 10, 15, 20, 25, 35, 50, 75, 100, 150, 200 };
            if (it.choice_index >= 0 && it.choice_index < (int)(sizeof STEPS / sizeof STEPS[0]))
                s.touch_sensitivity = STEPS[it.choice_index];
            break;
        }
        case SET_CURSOR:
            if (it.choice_index >= 0 && it.choice_index <= 2)
                s.cursor_source = (uint32_t)it.choice_index;
            break;
        /* CLIP4 - applies LIVE, with no reconnection: `applyToggles` sets
         * SHADOW_CLIPBOARD and the session re-reads it at every poll. The one
         * exception is leaving "off", because at "off" no channel was opened -
         * which is what the row's description says. */
        case SET_CLIPBOARD:
            if (it.choice_index >= 0 && it.choice_index <= 3)
                s.clipboard_mode = (uint32_t)it.choice_index;
            break;
        /* NET1 - takes effect on the NEXT connection: the family is chosen when
         * a socket is created, and the sockets of a live session already exist.
         * The description says so rather than leaving the user to wonder why
         * nothing moved. */
        case SET_NET_FAMILY: {
            static const uint32_t FAMS[] = { 0u, 4u, 6u };
            if (it.choice_index >= 0 && it.choice_index < 3)
                s.net_family = FAMS[it.choice_index];
            break;
        }
        /* B1 - THESE TWO NOW APPLY LIVE, like the quality screen's bitrate.
         * They set the same quantity and only one of them took effect without
         * reconnecting: you changed the Wi-Fi rate mid-session, nothing moved,
         * and there was no way to tell that from a setting that does not work.
         * `bitrate_apply_live` sends whichever value is actually in force. */
        case SET_RATE_WIFI:
            if (it.choice_index >= 0 && it.choice_index < BITRATE_LADDER_N) {
                s.bitrate_wifi_mbps = BITRATE_LADDER[it.choice_index];
                s.applyBitrateLive();
            }
            break;
        case SET_RATE_ETH:
            if (it.choice_index >= 0 && it.choice_index < BITRATE_LADDER_N) {
                s.bitrate_eth_mbps = BITRATE_LADDER[it.choice_index];
                s.applyBitrateLive();
            }
            break;
        /* === S88 - THE TWO SOUND SETTINGS TAKE EFFECT AT ONCE ===
         *
         * And they MAKE THEMSELVES HEARD as they change, which is not an
         * ornament: a volume is set by ear. Without that feedback you would
         * have to leave the screen, navigate elsewhere to hear the result, then
         * come back - that is, set it blind. Same reason as the rumble pulse on
         * the strength setting (G57d).
         *
         * On switch-off nothing is played, obviously: `setActif(false)` also
         * cuts the voices already playing, so the silence is immediate and
         * complete. */
        case SET_SOUNDS:
            s.sounds = it.lit;
            ui::sfx::setEnabled(s.sounds);
            if (s.sounds) ui::sfx::play(ui::sfx::Sound::Confirm);
            rebuild_needed = true;   /* makes the volume appear or disappear */
            break;
        case SET_SOUNDS_VOL: {
            static const uint32_t STEPS[] = { 20, 40, 60, 70, 80, 100 };
            if (it.choice_index >= 0
                && it.choice_index < (int)(sizeof STEPS / sizeof STEPS[0])) {
                s.sounds_volume = STEPS[it.choice_index];
                ui::sfx::setVolume(s.sounds_volume);
                /* The navigation sound rather than another: it is the one that
                 * will be heard the most, so the one the level is judged on. */
                ui::sfx::play(ui::sfx::Sound::Navigation);
            }
            break;
        }
        case SET_POINTER:
            s.demo_pointer = it.lit;
            ui::pointer::setEnabled(s.demo_pointer);
            break;
        case SET_HAPTICS: {
            static const uint32_t HAPTIC_STEPS[] = { 0, 20, 40, 60, 80, 100 };
            if (it.choice_index >= 0
                && it.choice_index < (int)(sizeof HAPTIC_STEPS / sizeof HAPTIC_STEPS[0])) {
                s.haptics_strength = HAPTIC_STEPS[it.choice_index];
                ui::haptics::setStrength((int)s.haptics_strength);
                /* We make it FELT at the moment it is set: a feedback setting
                 * adjusted without perceiving it is set blind. */
                ui::haptics::play(ui::haptics::Intent::ToggleOn);
            }
            break;
        }
        case SET_LOG_LEVEL:
            /* +1: index 0 of the list is level 1 (ALERTE). Level 0 - fatal
             * errors only - is NOT offered: a log where the fallbacks no longer
             * show makes diagnosis impossible, and it is precisely when things
             * do not work that one comes here. */
            if (it.choice_index >= 0 && it.choice_index <= 3)
                s.log_level = (uint32_t)it.choice_index + 1;
            break;
        case SET_KB_LAYOUT:
            s.kb_layout = it.choice_index == 1 ? Settings::KbLayout::Qwerty
                                              : Settings::KbLayout::Azerty;
            break;
        case SET_UI_SCALE:
            if (it.choice_index >= 0 && it.choice_index < (int)(sizeof SCALES / sizeof SCALES[0]))
                s.ui_scale = SCALES[it.choice_index];
            break;
        case SET_LANGUAGE:
            if (it.choice_index >= 0 && it.choice_index < (int)(sizeof LANGS / sizeof LANGS[0])) {
                s.language = LANGS[it.choice_index].code;
                /* The catalogues are loaded once, at startup, and the locale is
                 * fixed before the platform is created: it cannot be switched
                 * hot. We SAY so rather than let it look like a malfunction. */
                brls::Application::notify(ui::tr("settings/restart_needed"));
            }
            break;
        default: return false;
    }
    s.save();

    /* Several of these settings are only read through an environment variable,
     * by C code. Without this reapplication, changing them would take effect
     * only on the NEXT launch - that is, a setting that lies. See
     * Settings::appliquerBascules. */
    s.applyToggles();

    if (rebuild_needed) rebuild();
    return true;
}

bool SettingsView::left()
{
    if (!screen_.left()) return false;
    const ui::Item *it = screen_.focusedItem();
    if (it) apply(*it);
    return true;
}

bool SettingsView::right()
{
    if (!screen_.right()) return false;
    const ui::Item *it = screen_.focusedItem();
    if (it) apply(*it);
    return true;
}

bool SettingsView::activate()
{
    const ui::Item *it = screen_.focusedItem();
    if (!it) return false;

    if (it->kind == ui::Kind::Action) {
        if (it->id == SET_DEV_CHANNEL_NOW) {
            /* Handled here rather than through `on_action_`: this does not
             * leave the screen, unlike the other actions, which push a page. */
            journal_reconnect_sink();
            return true;
        }
        if (it->id == SET_LOG_CLEAR) { askClearLogs(); return true; }
        if (it->id == SET_RESET)     { askReset();     return true; }
        if (it->id == SET_LOCK_PIN || it->id == SET_LOCK_PASSWORD ||
            it->id == SET_LOCK_PATTERN) {
            const unsigned m = it->id == SET_LOCK_PIN      ? APPLOCK_PIN
                             : it->id == SET_LOCK_PASSWORD ? APPLOCK_PASSWORD
                                                           : APPLOCK_PATTERN;
            /* The entry screen is pushed ON TOP and pops ITSELF when it is
             * done, whether the secret was set or abandoned - so we come back
             * here either way, and the mailbox makes the page redraw with the
             * new state. */
            std::shared_ptr<bool> mailbox = purge_done_;
            brls::Application::pushActivity(new LockActivity(
                [mailbox](bool set) {
                    brls::Application::popActivity();
                    /* Seal the token NOW rather than at the next rotation. A
                     * rotation may be days away, and until then the file would
                     * still be readable from a computer - so the setting would
                     * be on and the protection would not. */
                    if (set) oauth_reencrypt_refresh();
                    *mailbox = true;
                }, std::string(), m));
            return true;
        }
        if (it->id == SET_LOCK_REMOVE) { askRemoveLock(); return true; }
        if (on_action_) on_action_(it->id);
        return true;
    }
    if (it->kind == ui::Kind::Toggle) {
        if (!screen_.toggle()) return false;
        /* We re-read the entry AFTER the toggle: `toggle()` changes the state
         * in the list, and applying the `it` from before would save the old
         * value - the setting would then appear to revert on its own at the
         * next display. */
        const ui::Item *after = screen_.focusedItem();
        if (after) apply(*after);
        return true;
    }
    return false;
}

/* === S86 - CLEARING THE LOGS IS CONFIRMED ===
 *
 * It is the only destructive action on this page besides logging out, and in
 * practice it is WORSE: it deletes the trace of the very thing one was trying
 * to understand. One press too many on A, and the incident that had just been
 * reproduced exists nowhere any more.
 *
 * The dialog is built like the SET_LOGOUT one, including for the S66 trap: NO
 * `close()` after a button - `Dialog::buttonClick` already removes the dialog,
 * and a second pop would take this screen with it.
 *
 * The difference is that this one has to REFRESH the page afterwards: without
 * that, the "place occupee" row would go on announcing the megabytes that were
 * just erased - a figure that lies right after the action that changed it. It
 * goes through the shared mailbox (settings_view.hpp) rather than through a
 * capture of `this`. */
void SettingsView::askClearLogs()
{
    std::shared_ptr<bool> mailbox = purge_done_;

    brls::Dialog *dlg = new brls::Dialog(ui::tr("settings/log_clear_question"));
    dlg->setCancelable(true);
    /* Refusing comes FIRST: it is the default answer to a destructive dialog,
     * and the console puts the cursor on the first button. */
    dlg->addButton(ui::tr("action/cancel"), []() { });
    dlg->addButton(ui::tr("settings/log_clear"), [mailbox]() {
        const int n = journal_purge();
        brls::Application::notify(ui::tr("settings/log_cleared", n));
        *mailbox = true;
    });
    dlg->open();
}

/* === 2026-09-02 - RESET, CONFIRMED ===
 *
 * Built like `askClearLogs`, and for the same reasons - including the S66 trap:
 * NO `close()` after a button, `Dialog::buttonClick` already removes the dialog
 * and a second pop would take this screen with it.
 *
 * It also refreshes the page through the shared mailbox: every row on screen
 * displays a value that has just changed, so without the rebuild the list would
 * go on showing the old settings until the section is left and re-entered - the
 * user would conclude the reset did nothing and press again. */
void SettingsView::askReset()
{
    std::shared_ptr<bool> mailbox = purge_done_;

    brls::Dialog *dlg = new brls::Dialog(ui::tr("settings/reset_confirm"));
    dlg->setCancelable(true);
    /* Refusing FIRST: the console puts the cursor on the first button, and the
     * default answer to a destructive dialog is no. */
    dlg->addButton(ui::tr("action/cancel"), []() { });
    dlg->addButton(ui::tr("settings/reset"), [mailbox]() {
        Settings::instance().resetToDefaults();
        brls::Application::notify(ui::tr("settings/reset_done"));
        *mailbox = true;
    });
    dlg->open();
}

/* === 2026-09-02 - REMOVING THE LOCK IS CONFIRMED ===
 *
 * Built like the two dialogs above it. It is destructive in a way the others
 * are not: the secrets cannot be recovered, and a console left unlocked by a
 * mis-press stays unlocked silently until someone notices.
 *
 * It is reachable ONLY from inside the application, i.e. only after the lock
 * has already been opened - which is what makes it safe to offer at all. */
void SettingsView::askRemoveLock()
{
    std::shared_ptr<bool> mailbox = purge_done_;

    brls::Dialog *dlg = new brls::Dialog(ui::tr("settings/lock_remove_question"));
    dlg->setCancelable(true);
    dlg->addButton(ui::tr("action/cancel"), []() { });
    dlg->addButton(ui::tr("settings/lock_remove"), [mailbox]() {
        /* === THE ORDER IS THE WHOLE THING ===
         *
         * The token is sealed under a master key that only a secret can unseal.
         * Removing the lock destroys every copy of that key, so the token has to
         * be read back out and rewritten in the un-sealed form FIRST - while the
         * key is still loaded.
         *
         * Clearing the record before this would leave a file nothing in the
         * world can decrypt: the lock would be gone and the session with it,
         * and the user would be sent back to a QR code for having turned off a
         * setting. The three steps live inside `oauth_unseal_refresh` precisely
         * so the order cannot be reversed from here - which is the mistake made
         * while writing this very function. */
        oauth_unseal_refresh();   /* read, forget the key, write back - in that order */
        applock_store_clear();
        brls::Application::notify(ui::tr("settings/lock_removed"));
        *mailbox = true;
    });
    dlg->open();
}

bool SettingsView::touch(float x, float y)
{
    const int id = screen_.touchAt(x, y);
    if (id < 0) return false;
    return activate();
}

void SettingsView::paint(NVGcontext *vg, float x, float y, float w, float h,
                      double t)
{
    screen_.draw(vg, x, y, w, h, t);

    /* S79 - a section picked WITH A FINGER. We ask about it after drawing, like
     * a tap on an entry: rebuilding the list from inside `draw()` would replace
     * it while we are drawing it, which is exactly the reentrancy this
     * framework avoids everywhere else. */
    if (screen_.sectionConsumed() >= 0) rebuild();

    /* Same thing: the finger activates after drawing, never during. */
    if (screen_.touchRelease() >= 0) activate();

    /* S86 - did the clear dialog act? Same discipline as the two readings
     * above: we rebuild AFTER drawing, never during. */
    if (*purge_done_) { *purge_done_ = false; rebuild(); }
}
