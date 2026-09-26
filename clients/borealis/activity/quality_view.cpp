/* QualityView - see the header. */

#include "quality_view.hpp"

#include "../device_caps.h"

#include "settings.hpp"
#include "../ui/i18n.hpp"
#include "../ui/env_note.hpp"

extern "C" {
#include "../../../core/protocol/ctrl_session.h"   /* the bitrate applied live (CFG-4: not the frame rate) */
#include "../../../core/protocol/bitrate.h"       /* B1: the ONE bitrate ladder */
}

#include <chrono>

namespace {

/* === Tables copied over from the old screen, comments included ===
 * Several of these values are reverse-engineering HYPOTHESES: erasing them
 * would pass a guess off as an established fact. */
struct OptU32 { const char *label; uint32_t value; };

/* B1 - THE LADDER IS NO LONGER WRITTEN HERE. It used to be one of three that
 * disagreed (see streaming/bitrate.h): this screen offered 80/150, the pause
 * menu stopped at 50, and the settings offered 25/75 that neither of the other
 * two knew. Whichever screen you left, the next one misread your choice. */
const OptU32 FPS_OPTS[] = {
    {nullptr, 0},   /* quality/auto */
    {"30 fps", 30}, {"60 fps", 60}, {"90 fps", 90},
    {"120 fps", 120}, {"144 fps", 144},
};
struct OptRes { const char *label; uint32_t w; uint32_t h; };
const OptRes RES_OPTS[] = {
    {"1280x720 (720p)",   1280,  720},
    {"1600x900 (900p)",   1600,  900},
    {"1920x1080 (1080p)", 1920, 1080},
    {"2560x1440 (1440p)", 2560, 1440},
};
/* === K13 2026-08-27 - WIRE VALUES, MEASURED AT LAST ===
 * The three previous numbers (H.264=2, H.265=1, AV1=3) were guesses from the B1
 * campaign, AND they were written into a field that had no effect. Working
 * through the guided capture of 21/08 gives the enum used on the WIRE:
 *   H.264 = 0  (field ABSENT: that is the proto3 default, the official client
 *               writes nothing at all - so there is no separate "automatic"
 *               setting, H.264 IS the default)
 *   H.265 = 1  (`18 01`, observed)
 *   AV1   = 2  (by elimination)
 * The account allows all three: `capabilities ... codecs=h264,h265,av1`. */
const OptU32 CODEC_OPTS[] = {
    {"H.264", 0},
    {"H.265", 1},
    /* AV1: the value 2 is DEDUCED, not observed. The descriptor pool of the
     * official binary holds only the NAMES of the enum, sorted alphabetically
     * ("AV1H264H265") - hence with no ordinals. We know the account allows it
     * (our own logs: `codecs=h264,h265,av1`), we do not know which number asks
     * for it. That is exactly the status "H.265=1" and "AV1=3" had before K13,
     * and they were wrong AND written into the wrong field. So the label says
     * so, rather than letting anyone believe otherwise.
     * On Switch there is no hardware AV1 decoding at all anyway: the Tegra X1 is
     * 2015 Maxwell, hardware AV1 arrives with Ampere. */
    {nullptr, 2},   /* quality/codec_av1 - translated where it is shown */
};
const OptU32 PROFILE_OPTS[] = {
    {nullptr, 0},   /* quality/auto */
    {nullptr, 1},   /* quality/profile_speed   - CI_BODY_5 hardcoded */
    {nullptr, 2},   /* quality/profile_quality */
};

template <size_t N>
int indexU32(const OptU32 (&t)[N], uint32_t val)
{
    for (size_t i = 0; i < N; i++) if (t[i].value == val) return (int)i;
    return 0;
}

/* Null labels are TRANSLATED at the point of use: quality/auto,
 * quality/profile_speed, quality/profile_quality. Hardcoding them here would
 * make the screen monolingual, which is exactly what we have just fixed
 * elsewhere. */
std::vector<std::string> labels(const OptU32 *t, size_t n, const char *key_auto,
                                const char *key1 = nullptr, const char *key2 = nullptr)
{
    std::vector<std::string> v;
    for (size_t i = 0; i < n; i++) {
        if (t[i].label) { v.push_back(t[i].label); continue; }
        if (i == 0)                 v.push_back(ui::tr(key_auto));
        else if (i == 1 && key1)    v.push_back(ui::tr(key1));
        else if (i == 2 && key2)    v.push_back(ui::tr(key2));
        else                        v.push_back(ui::tr(key_auto));
    }
    return v;
}

ui::Item toggleItem(int id, const std::string &t, const std::string &desc, bool on)
{
    ui::Item i; i.kind = ui::Kind::Toggle; i.id = id;
    i.title = t; i.subtitle = desc; i.lit = on;
    return i;
}

/* A line that INFORMS without offering anything. It exists for 4:4:4 on
 * console: hiding a feature the hardware refuses makes it look as though it did
 * not exist and invites a search for it; offering it would be a trap. So we
 * show it, switched off, with the reason. */
ui::Item noticeItem(const std::string &t, const std::string &desc)
{
    ui::Item i; i.kind = ui::Kind::Toggle; i.id = 0;
    i.title = t; i.subtitle = desc; i.lit = false; i.actionable = false;
    return i;
}

ui::Item choiceItem(int id, const std::string &t, const std::string &desc,
                    std::vector<std::string> values, int index)
{
    ui::Item i; i.kind = ui::Kind::Choice; i.id = id;
    i.title = t; i.subtitle = desc;
    i.choice = std::move(values); i.choice_index = index;
    return i;
}

}  // namespace

QualityView::QualityView()
{
    screen_.setTitle(ui::tr("quality/title"));
    screen_.setSubtitle(ui::tr("quality/subtitle"));
    screen_.setHints({ { "B", ui::tr("action/back") } });
    rebuild();
}

void QualityView::rebuild()
{
    Settings &s = Settings::instance();
    std::vector<ui::Item> v;

    {
        /* The labels are built from the shared ladder, so this screen cannot
         * offer a rung the others do not know. Rung 0 is Auto, and its label
         * SAYS what Auto is: there is no "let the server decide" on this wire -
         * the announcement always carries a number, and that number is the
         * desktop client's default. Calling it "Auto" without saying so is what
         * made the setting look broken when it was merely undocumented. */
        std::vector<std::string> rates;
        rates.push_back(ui::tr("quality/auto") + " (" +
                        std::to_string(BITRATE_CLIENT_DEFAULT_MBPS) + " Mb/s)");
        for (int i = 1; i < BITRATE_LADDER_N; i++)
            rates.push_back(std::to_string(BITRATE_LADDER[i]) + " Mb/s");

        /* When the per-link setting governs the bitrate, this row no longer
         * decides anything: saying so is the whole point, because the previous
         * version let you change a value that connect() then overwrote without
         * a word. */
        const std::string desc = ui::envNote("SHADOW_BITRATE_MBPS",
                                 s.bitrate_per_link
                                   ? ui::tr("quality/bitrate_overridden")
                                   : ui::tr("quality/bitrate_desc"));
        v.push_back(choiceItem(QUAL_BITRATE, ui::tr("quality/bitrate"), desc,
                               rates, bitrate_index(s.max_bitrate_mbps)));
    }

    /* === A CONTROL THAT LIES IS WORSE THAN AN ABSENT ONE ================
     *
     * Frame rate and resolution are settable where the server's grant is the
     * only limit. On the PS Vita they are not: the panel is 960x544 at 60 Hz,
     * and the hardware decoder takes its geometry from the descending ladder in
     * `h264_decoder.c`, by what it can actually allocate - 1080p is refused
     * outright. Offering the choice would let someone pick 1080p120, watch the
     * screen show 960x544 at 60, and reasonably conclude the app is broken.
     *
     * They are REMOVED, not replaced by an explanatory line. That was the first
     * attempt and it was wrong twice over: the notice still read as a row you
     * could act on, and 4:4:4 kept rendering with an on/off suffix. A setting
     * the console overrides has no place on the screen at all.
     *
     * Same treatment for the codec (`SceAvcdec` is H.264 only) and for 4:4:4,
     * which NEITHER console can do - the Switch's inability was already known
     * (K13/K14) and was written `#ifdef __SWITCH__`; it is a capability now. */
#if SHADOW_HAS_MODE_CHOICE
    v.push_back(choiceItem(QUAL_FPS, ui::tr("quality/fps"),
                           ui::envNote("SHADOW_FPS", ui::tr("quality/fps_desc")),
                           labels(FPS_OPTS, sizeof FPS_OPTS / sizeof FPS_OPTS[0], "quality/auto"),
                           indexU32(FPS_OPTS, s.target_fps)));

    {
        std::vector<std::string> res;
        int idx = 2;
        for (size_t i = 0; i < sizeof RES_OPTS / sizeof RES_OPTS[0]; i++) {
            res.push_back(RES_OPTS[i].label);
            if (RES_OPTS[i].w == s.display_width && RES_OPTS[i].h == s.display_height)
                idx = (int)i;
        }
        v.push_back(choiceItem(QUAL_RES, ui::tr("quality/resolution"),
                               ui::tr("quality/resolution_desc"), res, idx));
    }
#endif   /* SHADOW_HAS_MODE_CHOICE */

    /* The codec choice goes the same way: `SHADOW_HAS_DECODER_CHOICE` is 0 on
     * the Vita because `SceAvcdec` decodes H.264 and nothing else. */
#if SHADOW_HAS_DECODER_CHOICE
    v.push_back(choiceItem(QUAL_CODEC, ui::tr("quality/codec"),
                           ui::envNote("SHADOW_CODEC", ui::tr("quality/codec_desc")),
                           labels(CODEC_OPTS, sizeof CODEC_OPTS / sizeof CODEC_OPTS[0],
                                  "quality/auto", nullptr, "quality/codec_av1"),
                           indexU32(CODEC_OPTS, s.codec)));
#endif   /* SHADOW_HAS_DECODER_CHOICE */

    /* K17 - 4:4:4. On console the hardware refuses it at four independent
     * points: we announce it as unavailable rather than offer it. */
#if SHADOW_HAS_CHROMA_444
    v.push_back(toggleItem(QUAL_444, ui::tr("quality/chroma444"),
                           ui::envNote("SHADOW_REG_F5", ui::tr("quality/chroma444_desc")),
                           s.color_444));
#endif

    /* L14 - waiting for the vertical blank. Placed here and not under the
     * "Avance" section because it is a PICTURE setting, judged by eye like the
     * others on this screen. */
    v.push_back(toggleItem(QUAL_VSYNC, ui::tr("quality/vsync"),
                           ui::envNote("SHADOW_VSYNC", ui::tr("quality/vsync_desc")),
                           s.vsync_stream));

    v.push_back(choiceItem(QUAL_PROFILE, ui::tr("quality/profile"),
                           ui::envNote("SHADOW_PROFILE_ID", ui::tr("quality/profile_desc")),
                           labels(PROFILE_OPTS, sizeof PROFILE_OPTS / sizeof PROFILE_OPTS[0],
                                  "quality/auto", "quality/profile_speed", "quality/profile_quality"),
                           indexU32(PROFILE_OPTS, s.profile_id)));

    screen_.setItems(std::move(v));
}

bool QualityView::apply(const ui::Item &it)
{
    Settings &s = Settings::instance();
    const int i = it.choice_index;
    /* B2 - the guard applies to the CHOICES only. A toggle carries no choice
     * index, and returning false here would make every toggle on this screen
     * inert a second time, through a different door. */
    if (it.kind != ui::Kind::Toggle && i < 0) return false;

    switch (it.id) {
        case QUAL_BITRATE:
            if (i >= 0 && i < BITRATE_LADDER_N)
                s.max_bitrate_mbps = BITRATE_LADDER[i];
            break;
        case QUAL_FPS:
            if (i < (int)(sizeof FPS_OPTS / sizeof FPS_OPTS[0]))
                s.target_fps = FPS_OPTS[i].value;
            break;
        case QUAL_RES:
            if (i < (int)(sizeof RES_OPTS / sizeof RES_OPTS[0])) {
                s.display_width  = RES_OPTS[i].w;
                s.display_height = RES_OPTS[i].h;
            }
            break;
        case QUAL_CODEC:
            if (i < (int)(sizeof CODEC_OPTS / sizeof CODEC_OPTS[0]))
                s.codec = CODEC_OPTS[i].value;
            break;
        case QUAL_PROFILE:
            if (i < (int)(sizeof PROFILE_OPTS / sizeof PROFILE_OPTS[0]))
                s.profile_id = PROFILE_OPTS[i].value;
            break;
        case QUAL_444:
            s.color_444 = it.lit;
            break;
        case QUAL_VSYNC:
            /* L14 - takes effect on the next entry into the stream: the stream
             * view sets the interval when it is built and restores it on the
             * way out. Changing it under a running session would touch the
             * presentation chain while it is turning. */
            s.vsync_stream = it.lit;
            break;
        default: return false;
    }
    s.save();

    /* === 2026-08-27 - WITHOUT THIS, TWO SETTINGS WOULD DO NOTHING ===
     * `quality/codec` and `quality/profile` are only read out of `SHADOW_CODEC`
     * and `SHADOW_PROFILE_ID` by `ctrl_session.c` - `ctrl_session_glue_params`
     * does not carry them. On console, where no variable is ever set, changing
     * them had NO effect at all: you believed you had tried something.
     * So we re-apply on every change, and not only at startup, otherwise the
     * choice would take effect on the next launch only. */
    s.applyToggles();

    /* The bitrate, on the other hand, applies WITHOUT reopening the session -
     * the official client does the same. The others, the frame rate included
     * (CFG-4 2026-09-11: the live message has had no frame-rate field since
     * S18), wait for the next open, which each entry's description says.
     * applyBitrateLive() is the one path for a live bitrate: it resolves Auto
     * (B1: Auto is SENT, not "leave unchanged") and the per-link value, which
     * this call used to bypass. */
    if (it.id == QUAL_BITRATE)
        s.applyBitrateLive();

    return true;
}

/* B2 - A on this screen. Mirrors SettingsView::activate for the one kind that
 * needs it here: a toggle. The choices are cycled with left/right and have
 * nothing to do on A.
 *
 * The entry is RE-READ after `toggle()`: that call changes the state held in the
 * list, and applying the `it` captured before would save the OLD value - the
 * setting would then appear to revert on its own at the next display. Same trap,
 * same wording, as the settings screen. */
bool QualityView::activate()
{
    const ui::Item *it = screen_.focusedItem();
    if (!it || it->kind != ui::Kind::Toggle) return false;
    if (!screen_.toggle()) return false;
    const ui::Item *after = screen_.focusedItem();
    if (after) apply(*after);
    return true;
}

bool QualityView::left()
{
    if (!screen_.left()) return false;
    const ui::Item *it = screen_.focusedItem();
    if (it) apply(*it);
    return true;
}

bool QualityView::right()
{
    if (!screen_.right()) return false;
    const ui::Item *it = screen_.focusedItem();
    if (it) apply(*it);
    return true;
}

void QualityView::paint(NVGcontext *vg, float x, float y, float w, float h,
                     double t)
{
    screen_.draw(vg, x, y, w, h, t);

    /* A choice is not activated by touch: it is cycled. Touching a line only
     * SELECTS it - the gesture has already done that - and the value is then
     * changed with left/right. */
    (void)screen_.touchRelease();
}
