/* ui::StepScreen - see steps.hpp for why this archetype exists. */

#include "steps.hpp"

#include "../device_mode.hpp"
#include "i18n.hpp"
#include "key_hint.hpp"   /* K21: the footer names keys when there is no pad */
#include "paint.hpp"
#include "theme.hpp"
#include "type.hpp"

#include <math.h>
#include <stdio.h>

namespace ui {

namespace {
/* See screen.cpp: TVs overscan, so we widen the margin when docked. */
const float MARGIN_HANDHELD = 24.0f;
const float MARGIN_DOCKED   = 48.0f;
const float HEADER_H = 96.0f;
const float FOOTER_H = 44.0f;
const float STEP_H   = 46.0f;
const float DOT_R    = 11.0f;   /* radius of the state dot */

/* S80 - the ring. The radius is sized to fit inside the left column at 720p
 * without touching the header or the footer; the stroke width is the one that
 * stays legible from two metres away on a TV, the real reading distance when
 * docked. */
const float RING_COL    = 340.0f;   /* width of the left column */
const float RING_R      = 104.0f;
const float RING_STROKE =  14.0f;

/* How long a step takes to appear. Short enough not to delay reading - you are
 * watching a loading screen, not an animation - and long enough that you SEE
 * the row arrive, which is the whole point. */
const float APPEAR_S = 0.30f;

NVGcolor stateColor(StepState e)
{
    switch (e) {
        case StepState::Done:   return paint::ledActive;
        case StepState::Running: return paint::accentVif;
        case StepState::Failed: return theme::bad;
        default:                 return nvgRGBA(96, 104, 124, 190);
    }
}
}  // namespace

/* === S80 - THE RING ===
 *
 * Its filled portion IS the progress: it is the one thing you look at during a
 * load, and it reads without counting or reading.
 *
 * `p` comes from an animation, so from a clock, and it is CLAMPED before it
 * becomes an angle. A progress value outside [0,1] on applet resume - two frames
 * carrying the same timestamp produce 0/0 in an interpolation - would draw an
 * arc longer than a full turn, which folds back over itself and reads as a
 * rendering glitch. The same guard already protects list scrolling
 * (nav_scroll_borne) and the settings toggle. */
void StepScreen::drawRing(NVGcontext *vg, float cx, float cy, float r,
                          double t)
{
    const int nb = (int)steps_.size();
    if (nb <= 0) return;

    int done = 0, running = 0;
    bool failed = false;
    for (const Step &e : steps_) {
        if (e.state == StepState::Done)   done++;
        if (e.state == StepState::Running) running++;
        if (e.state == StepState::Failed) failed = true;
    }

    /* The RUNNING step counts as a half. Without that the ring stays perfectly
     * still for the whole duration of a step - so for most of the load, since
     * waiting is exactly what a step is - and you can no longer tell "it is
     * progressing" from "it is stuck". */
    const float target = ((float)done + 0.5f * (float)running) / (float)nb;
    ring_anim_ = anim_towards(&ring_anim_, target, t, ANIM_SCROLL_S);
    const float p = anim_clamp01(anim_value(&ring_anim_, t));

    const NVGcolor tint = failed ? theme::bad
                        : (done == nb) ? paint::ledActive
                        : paint::accentVif;

    /* The track. Always complete: it is what tells you what REMAINS, and a ring
     * without its track no longer reads as a proportion. */
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r);
    nvgStrokeWidth(vg, RING_STROKE);
    nvgStrokeColor(vg, nvgRGBA(150, 176, 224, 38));
    nvgStroke(vg);

    if (p > 0.0005f) {
        const float a0 = -NVG_PI * 0.5f;                 /* twelve o'clock */
        const float a1 = a0 + p * NVG_PI * 2.0f;

        /* A halo under the arc: without it a 14-pixel stroke on a midnight blue
         * background does not stand out enough to read from a distance. */
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, r, a0, a1, NVG_CW);
        nvgStrokeWidth(vg, RING_STROKE + 10.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgStrokeColor(vg, nvgTransRGBA(tint, 34));
        nvgStroke(vg);

        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, r, a0, a1, NVG_CW);
        nvgStrokeWidth(vg, RING_STROKE);
        nvgLineCap(vg, NVG_ROUND);
        nvgStrokeColor(vg, tint);
        nvgStroke(vg);
    }

    /* In the centre: the count. "3 / 7" rather than a percentage - seven steps
     * give 14%, 29%, 43%..., numbers you cannot relate to anything. The count
     * relates to the list sitting right next to it. */
    nvgFontFace(vg, theme::font());
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char count_txt[32];
    snprintf(count_txt, sizeof count_txt, "%d / %d", done, nb);
    nvgFontSize(vg, 44.0f);
    nvgFillColor(vg, failed ? theme::bad : theme::title);
    nvgText(vg, cx, cy - 8.0f, count_txt, nullptr);

    nvgFontSize(vg, ui::type::CAPTION);
    nvgFillColor(vg, theme::hint);
    nvgText(vg, cx, cy + 26.0f, ui::tr("connect/steps").c_str(), nullptr);

    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
}

void StepScreen::setStep(size_t i, StepState state, std::string detail)
{
    /* Out of range: ignore. The plan may differ between two versions, and
     * background work aiming at a step that no longer exists must not bring the
     * display down - it would not even know it had failed. */
    if (i >= steps_.size()) return;
    steps_[i].state = state;
    if (!detail.empty()) steps_[i].detail = std::move(detail);
}

void StepScreen::setMessage(std::string m, bool error)
{
    message_ = std::move(m);
    message_error_ = error;
}

void StepScreen::draw(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    paint::shadowBg(vg, x, y, w, h);
    paint::backgroundWaves(vg, x, y, w, h, t);

    const float MARGIN = device::isDocked() ? MARGIN_DOCKED : MARGIN_HANDHELD;
    const float cx = x + MARGIN;
    const float cw = w - MARGIN * 2.0f;

    nvgFontFace(vg, theme::font());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    nvgFontSize(vg, ui::type::SCREEN);
    nvgFillColor(vg, theme::title);
    paint::clampedText(vg, cx, y + 38.0f, cw, y, HEADER_H, title_, false, t);

    if (!subtitle_.empty()) {
        nvgFontSize(vg, ui::type::BODY);
        nvgFillColor(vg, theme::label);
        paint::clampedText(vg, cx, y + 68.0f, cw, y, HEADER_H, subtitle_, false, t);
    }

    const float zone_y = y + HEADER_H;
    const float zone_h = h - HEADER_H - FOOTER_H;
    const int   nb     = (int)steps_.size();

    /* === S80 - THE APPEARANCE INSTANT, WRITTEN EXACTLY ONCE ===
     *
     * `setEtapes()` is called EVERY frame (ConnectingView re-reads its model):
     * storing the instant inside the step would reset it sixty times a second
     * and nothing would ever move. So it lives apart, here, in the only place
     * that knows the time - and it is written only on the FIRST frame where the
     * step stops being pending.
     *
     * The vector is resized when the plan changes: timestamps already known are
     * kept at their index, an added step starts at -1. The plan does not change
     * mid-flight today, but a resize that lost the timestamps would replay every
     * appearance at once. */
    if ((int)appeared_.size() != nb) appeared_.resize((size_t)nb, -1.0);
    for (int i = 0; i < nb; i++)
        if (appeared_[(size_t)i] < 0.0 && steps_[(size_t)i].state != StepState::Waiting)
            appeared_[(size_t)i] = t;

    /* === S80b 2026-08-29 - THE WHOLE THING IS ONE BLOCK, AND IT IS CENTRED ===
     *
     * The first version pinned the ring to the page margin and gave all the rest
     * to the steps. The ring ended up stuck to the left and the steps floated in
     * the middle of a band twice as wide as needed: two elements that answer
     * each other, laid out as if they had never met.
     *
     * They form a BLOCK - a ring and its legend - and it is the block that gets
     * centred. Its width is MEASURED: the ring column plus the longest step
     * title. Measuring rather than hardcoding is what keeps the centring correct
     * when the language changes; a hardcoded width would be right in French and
     * wrong in English, with nothing to flag it. */
    const bool with_ring = cw > RING_COL + 200.0f && zone_h > RING_R * 2.2f;

    float text_w = 0.0f;
    nvgFontFace(vg, theme::font());
    nvgFontSize(vg, ui::type::SECTION);   /* the larger of the two sizes */
    for (const Step &e : steps_) {
        const float tw = paint::textWidth(vg, e.title.c_str());
        if (tw > text_w) text_w = tw;
    }
    /* The dot and its gap to the text are part of the steps column. */
    const float steps_col = DOT_R * 2.0f + 16.0f + text_w;

    float block_w = (with_ring ? RING_COL : 0.0f) + steps_col;
    if (block_w > cw) block_w = cw;       /* never overflow the page */
    const float block_x = cx + (cw - block_w) * 0.5f;

    if (with_ring)
        drawRing(vg, block_x + RING_COL * 0.5f, zone_y + zone_h * 0.5f,
                 RING_R, t);

    const float lx = with_ring ? block_x + RING_COL : block_x;
    const float lw = block_w - (with_ring ? RING_COL : 0.0f);

    /* The steps are CENTRED vertically in the remaining space: a seven-row
     * progress list pinned to the top of a 720-pixel screen leaves a gap that
     * reads as a stalled load. */
    const float total = STEP_H * (float)nb;
    float ey = zone_y + (zone_h - total) * 0.5f;
    if (ey < zone_y) ey = zone_y;

    for (int i = 0; i < nb; i++) {
        const Step &e = steps_[(size_t)i];
        const NVGcolor tint = stateColor(e.state);

        /* === THE APPEARANCE ===
         * A step still pending is only SKETCHED IN: its place is held, so you
         * know how many are left, but it does not read. As soon as it starts it
         * arrives - sliding in a few pixels and taking its full brightness. That
         * motion is what you watch pile up.
         *
         * `p` is CLAMPED: it comes from a subtraction of timestamps, and the
         * clock of an applet coming back from sleep can go backwards. */
        float p = 1.0f;
        if (appeared_[(size_t)i] < 0.0) {
            p = 0.0f;
        } else {
            const double dt = t - appeared_[(size_t)i];
            p = anim_ease_out(anim_clamp01((float)(dt / (double)APPEAR_S)));
        }
        const float slide = (1.0f - p) * 18.0f;
        const float my = ey + STEP_H * 0.5f;
        const float px = lx + DOT_R + slide;

        nvgGlobalAlpha(vg, 0.28f + 0.72f * p);

        if (e.state == StepState::Running) {
            /* The running step spins: together with the ring it is the screen's
             * second motion, and it answers a question the ring does not - WHICH
             * one is moving right now. */
            paint::spinner(vg, px, my, DOT_R, t);
        } else {
            nvgBeginPath(vg);
            nvgCircle(vg, px, my, DOT_R);
            nvgFillColor(vg, nvgTransRGBA(tint, 46));
            nvgFill(vg);
            nvgStrokeWidth(vg, 1.4f);
            nvgStrokeColor(vg, tint);
            nvgStroke(vg);

            if (e.state == StepState::Done) {
                /* A checkmark, drawn by hand: a checkmark CHARACTER would depend
                 * on the embedded font, and ours does not carry it everywhere. */
                nvgBeginPath(vg);
                nvgMoveTo(vg, px - 5.0f, my);
                nvgLineTo(vg, px - 1.5f, my + 3.5f);
                nvgLineTo(vg, px + 5.0f, my - 4.0f);
                nvgStrokeWidth(vg, 2.0f);
                nvgStrokeColor(vg, tint);
                nvgStroke(vg);
            } else if (e.state == StepState::Failed) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, px - 4.0f, my - 4.0f);
                nvgLineTo(vg, px + 4.0f, my + 4.0f);
                nvgMoveTo(vg, px + 4.0f, my - 4.0f);
                nvgLineTo(vg, px - 4.0f, my + 4.0f);
                nvgStrokeWidth(vg, 2.0f);
                nvgStrokeColor(vg, tint);
                nvgStroke(vg);
            }
        }

        /* The line joining a step to the next one: it makes the column read as a
         * PATH being walked rather than a bulleted list. It stops at the last
         * step, and it takes the colour of the step above - so the path colours
         * itself in behind the progress. */
        if (i < nb - 1) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, px, my + DOT_R + 2.0f);
            nvgLineTo(vg, px, my + STEP_H - DOT_R - 2.0f);
            nvgStrokeWidth(vg, 2.0f);
            nvgStrokeColor(vg, nvgTransRGBA(
                e.state == StepState::Done ? paint::ledActive
                                           : nvgRGBA(150, 176, 224, 255), 60));
            nvgStroke(vg);
        }

        /* === S80b - THE DETAIL ONLY SHOWS ON A FAILURE ===
         *
         * It used to carry "7/30", "192.168.x.x:9000", "HTTP 200": technical
         * noise next to readable sentences, on a screen you stare at while
         * waiting. It stays on the FAILED step, and there it is indispensable -
         * it is the code you ask someone to read out to you when helping them
         * remotely, the plain-language explanation already being at the bottom
         * of the page.
         *
         * The detail is measured BEFORE the title: it is what decides how much
         * room is left, otherwise a long title would be drawn over it. */
        float right = lx + lw;
        if (!e.detail.empty() && e.state == StepState::Failed) {
            nvgFontSize(vg, ui::type::CAPTION);
            nvgFillColor(vg, theme::hint);
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgText(vg, right, my, e.detail.c_str(), nullptr);
            right -= paint::textWidth(vg, e.detail.c_str()) + 16.0f;
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        }

        const float tx = px + DOT_R + 16.0f;
        nvgFontSize(vg, e.state == StepState::Running ? ui::type::SECTION
                                                     : ui::type::BODY);
        nvgFillColor(vg, e.state == StepState::Waiting ? theme::label : theme::title);
        paint::clampedText(vg, tx, my, right - tx, ey, STEP_H, e.title, false, t);

        nvgGlobalAlpha(vg, 1.0f);
        ey += STEP_H;
    }

    /* --- Footer: message on the left, hints on the right ------------------ */
    const float py = y + h - FOOTER_H / 2.0f;
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx, y + h - FOOTER_H);
    nvgLineTo(vg, cx + cw, y + h - FOOTER_H);
    nvgStrokeWidth(vg, 1.0f);
    nvgStrokeColor(vg, nvgRGBA(150, 176, 224, 46));
    nvgStroke(vg);

    float edge = cx + cw;
    for (size_t i = hints_.size(); i-- > 0; ) {
        const Hint &hh = hints_[i];
        /* K21 - same rule as `ui/screen.cpp`'s footer: the call sites keep
         * writing the logical button, this is where it becomes a key name. */
        const std::string glyph = hintGlyph(hh.button);
        const float hint_w = paint::hintWidth(vg, glyph.c_str(), hh.label.c_str());
        if (edge - hint_w < cx) break;
        edge -= hint_w;
        paint::hintButton(vg, edge, py, glyph.c_str(), hh.label.c_str());
        edge -= 22.0f;
    }

    if (!message_.empty()) {
        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, message_error_ ? theme::bad : theme::hint);
        paint::clampedText(vg, cx, py, edge - 16.0f - cx, y + h - FOOTER_H, FOOTER_H,
                          message_, false, t);
    }
}

}  // namespace ui
