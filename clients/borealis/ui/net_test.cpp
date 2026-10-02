/* ui::nettest - see net_test.hpp for what this page refuses to show. */
#include "net_test.hpp"

#include "hold_exit.hpp"   /* HOLD-1: leaving this page works off console too */
#include "i18n.hpp"
#include "key_hint.hpp"    /* K21: name the key, not the button */
#include "paint.hpp"
#include "theme.hpp"
#include "screen.hpp"
#include "../device_mode.hpp"

extern "C" {
#include "../../../core/services/stats.h"
#include "../../../core/protocol/path_probe.h"
}

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <cstdio>
#include <cstring>

namespace ui {
namespace nettest {

namespace {

/* MODULE state, not a function `static`: it is written by the host that
 * samples and read by the drawing, and that family of defects is the one
 * CLAUDE.md names first. */
path_probe_t g_probe;
double       g_last_sample_t = 0.0;
uint64_t     g_prev_bytes    = 0;
uint32_t     g_prev_exp = 0, g_prev_mis = 0;
bool         g_primed = false;

/* The best and the worst the session has shown, kept for the display. */
float g_rtt_min_ms = 0.0f, g_rtt_max_ms = 0.0f;

void row(NVGcontext *vg, float x, float y, float w,
         const char *label, const char *value, NVGcolor tint)
{
    nvgFontSize(vg, 17.0f);
    nvgFontFace(vg, ui::theme::font());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, ui::theme::label);
    nvgText(vg, x, y, label, nullptr);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tint);
    nvgText(vg, x + w, y, value, nullptr);
}

}  // namespace

void reset()
{
    path_probe_reset(&g_probe);
    g_prev_bytes = 0; g_prev_exp = 0; g_prev_mis = 0;
    g_primed = false;
    g_rtt_min_ms = g_rtt_max_ms = 0.0f;
}

void sample(double t)
{
    session_stats_t ws;
    session_stats_get(&ws);

    if (ws.ctrl_rtt_us > 0) {
        const float ms = ws.ctrl_rtt_us / 1000.0f;
        if (g_rtt_min_ms == 0.0f || ms < g_rtt_min_ms) g_rtt_min_ms = ms;
        if (ms > g_rtt_max_ms) g_rtt_max_ms = ms;
    }

    if (t - g_last_sample_t < 1.0) return;

    /* The FIRST pass is not a sample: it has no previous interval, so its
     * "bitrate" would be everything the session has received since it began,
     * divided by one second - a huge, false number that would become the
     * advised ceiling. */
    if (!g_primed) {
        g_primed = true;
        g_last_sample_t = t;
        g_prev_bytes = ws.rtp_video_bytes;
        g_prev_exp = ws.chunks_expected; g_prev_mis = ws.chunks_missing;
        return;
    }

    const double dt = t - g_last_sample_t;
    g_last_sample_t = t;

    /* Unsigned on both sides: a counter that GOES BACKWARDS means the session
     * restarted, not that the bitrate is negative. This is the L21 family,
     * where an unsigned subtraction once displayed billions. */
    if (ws.rtp_video_bytes < g_prev_bytes || ws.chunks_expected < g_prev_exp) {
        reset();
        return;
    }

    const double dbytes = (double)(ws.rtp_video_bytes - g_prev_bytes);
    const uint32_t dexp = ws.chunks_expected - g_prev_exp;
    const uint32_t dmis = ws.chunks_missing  - g_prev_mis;
    g_prev_bytes = ws.rtp_video_bytes;
    g_prev_exp = ws.chunks_expected; g_prev_mis = ws.chunks_missing;

    if (dexp == 0) return;   /* nothing received over the interval: not a sample */

    path_probe_add(&g_probe, (float)(dbytes * 8.0 / dt / 1e6),
                   (float)dmis / (float)dexp);
}

bool draw(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    session_stats_t ws;
    session_stats_get(&ws);

    ui::paint::shadowBg(vg, x, y, w, h);
    ui::paint::backgroundWaves(vg, x, y, w, h, t);

    const float margin = device::isDocked() ? UI_MARGIN_DOCK : UI_MARGIN_HANDHELD;
    const float px = x + margin, pw = w - margin * 2.0f;
    ui::drawHeader(vg, px, y, pw, ui::tr("net/title"), ui::tr("net/subtitle"), "", t);

    float ry = y + UI_HEADER_H + 24.0f;
    const float step = 30.0f;
    char v[96];

    /* --- What the link carries: the verdict, or its absence ---------------- */
    nvgFontSize(vg, 15.0f);
    nvgFontFace(vg, ui::theme::font());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, ui::theme::sectionT);
    nvgText(vg, px, ry, ui::tr("net/capacity").c_str(), nullptr);
    ry += 26.0f;

    switch (path_probe_verdict(&g_probe)) {
        case PROBE_CEILING_FOUND: {
            std::snprintf(v, sizeof v, "%.1f Mb/s", (double)path_probe_suggest_mbps(&g_probe));
            row(vg, px, ry, pw, ui::tr("net/suggested").c_str(), v, ui::theme::good);
            ry += step;
            std::snprintf(v, sizeof v, "%.1f Mb/s", (double)g_probe.lossy_min_mbps);
            row(vg, px, ry, pw, ui::tr("net/first_loss").c_str(), v, ui::theme::warn);
            ry += step;
            break;
        }
        case PROBE_NO_CEILING:
            /* "no ceiling found UP TO X", and never "no ceiling": the second
             * wording invites you to climb, when nothing above X has been
             * measured at all. */
            std::snprintf(v, sizeof v, "%.1f Mb/s", (double)g_probe.clean_max_mbps);
            row(vg, px, ry, pw, ui::tr("net/no_ceiling").c_str(), v, ui::theme::good);
            ry += step;
            break;
        default:
            row(vg, px, ry, pw, ui::tr("net/insufficient").c_str(),
                ui::tr("net/keep_playing").c_str(), ui::theme::label);
            ry += step;
            break;
    }
    std::snprintf(v, sizeof v, "%u  (%u %s)", g_probe.samples, g_probe.lossy,
                  ui::tr("net/with_loss").c_str());
    row(vg, px, ry, pw, ui::tr("net/samples").c_str(), v, ui::theme::value);
    ry += step + 14.0f;

    /* --- L'aller-retour ---------------------------------------------------- */
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, ui::theme::sectionT);
    nvgText(vg, px, ry, ui::tr("net/rtt_section").c_str(), nullptr);
    ry += 26.0f;

    if (ws.ctrl_rtt_avg_us > 0) {
        const float avg = ws.ctrl_rtt_avg_us / 1000.0f;
        std::snprintf(v, sizeof v, "%.0f ms", (double)avg);
        row(vg, px, ry, pw, ui::tr("net/rtt_avg").c_str(), v,
            avg < 30.0f ? ui::theme::good : avg < 80.0f ? ui::theme::warn : ui::theme::bad);
        ry += step;
        /* The p90 beside the mean: a good mean with a bad p90 FEELS bad, and the
         * mean alone hides it. */
        std::snprintf(v, sizeof v, "%.0f ms", ws.ctrl_rtt_p90_us / 1000.0);
        row(vg, px, ry, pw, ui::tr("net/rtt_p90").c_str(), v, ui::theme::value);
        ry += step;
        std::snprintf(v, sizeof v, "%.0f ms", ws.ctrl_rtt_jitter_us / 1000.0);
        row(vg, px, ry, pw, ui::tr("net/rtt_jitter").c_str(), v, ui::theme::value);
        ry += step;
        std::snprintf(v, sizeof v, "%.0f / %.0f ms", (double)g_rtt_min_ms, (double)g_rtt_max_ms);
        row(vg, px, ry, pw, ui::tr("net/rtt_range").c_str(), v, ui::theme::value);
        ry += step + 14.0f;
    } else {
        row(vg, px, ry, pw, ui::tr("net/rtt_avg").c_str(),
            ui::tr("net/no_session").c_str(), ui::theme::label);
        ry += step + 14.0f;
    }

    /* --- L'integrite ------------------------------------------------------- */
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, ui::theme::sectionT);
    nvgText(vg, px, ry, ui::tr("net/integrity").c_str(), nullptr);
    ry += 26.0f;

    if (ws.chunks_expected > 0) {
        const float loss = 100.0f * (float)ws.chunks_missing / (float)ws.chunks_expected;
        std::snprintf(v, sizeof v, "%.2f %%", (double)loss);
        row(vg, px, ry, pw, ui::tr("net/loss").c_str(), v,
            loss < 0.3f ? ui::theme::good : loss < 1.0f ? ui::theme::warn : ui::theme::bad);
        ry += step;
        /* The lower bound is SAID so. A picture whose opening chunk is lost
         * enters neither of the two counters behind the rate above. */
        std::snprintf(v, sizeof v, "%u", ws.chunks_orphan_lost);
        row(vg, px, ry, pw, ui::tr("net/loss_extra").c_str(), v, ui::theme::label);
        ry += step;
        std::snprintf(v, sizeof v, "%u", ws.frames_trunc);
        row(vg, px, ry, pw, ui::tr("net/frames_trunc").c_str(), v, ui::theme::value);
        ry += step;
        /* Separates OUR fault from the network's. */
        /* ING-1 2026-09-11 - only where it is counted. On Windows and on the
         * Switch nothing ever wrote this counter (no SO_RXQ_OVFL), and a
         * permanent green 0 read as "our buffers never overflow". */
        if (ws.kernel_drops_valid) {
            std::snprintf(v, sizeof v, "%u", ws.kernel_drops);
            row(vg, px, ry, pw, ui::tr("net/kernel_drops").c_str(), v,
                ws.kernel_drops ? ui::theme::bad : ui::theme::good);
            ry += step;
        }
    } else {
        row(vg, px, ry, pw, ui::tr("net/loss").c_str(),
            ui::tr("net/no_session").c_str(), ui::theme::label);
        ry += step;
    }

    /* --- What this page does NOT claim ------------------------------------- */
    nvgFontSize(vg, 13.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, ui::theme::hint);
    nvgTextBox(vg, px, y + h - 96.0f, pw, ui::tr("net/caveat").c_str(), nullptr);

    /* --- Leaving, the same mechanism as the other testers ------------------ */
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 150));
    nvgText(vg, x + w * 0.5f, y + h - 26.0f,
            ui::tr("net/hint", ui::hintGlyph("B")).c_str(), nullptr);

    /* HOLD-1 2026-09-12 - this measurement used to sit inside `#ifdef
     * __SWITCH__`, so off console it was NOT COMPILED: `draw()` could never
     * return true, and this page has no other way out - `closeNetTest()` is
     * called from nowhere. Reported on the Windows client as "I cannot leave
     * the menu but I can hear the game carrying on behind it": the game was
     * indeed still running, and B was reaching it as a right click.
     *
     * The timing is in `ui/hold_exit.h` and pinned by `tests/test_hold_exit.c`;
     * the button read is in `ui/hold_exit.hpp`. Releasing still cancels. */
    float progress = 0.0f;
    if (ui::holdExitStep(ui::HOLD_NET, t, &progress)) {
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        return true;
    }
    if (progress > 0.0f) {
        const float bw = 160.0f;
        nvgBeginPath(vg);
        nvgRect(vg, x + w * 0.5f - bw * 0.5f, y + h - 16.0f, bw * progress, 3.0f);
        nvgFillColor(vg, ui::paint::accentVif);
        nvgFill(vg);
    }
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    return false;
}

}  // namespace nettest
}  // namespace ui
