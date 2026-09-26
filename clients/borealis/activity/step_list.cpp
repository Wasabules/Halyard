/* StepList — see step_list.hpp. */
#include "step_list.hpp"

#include <chrono>
#include <cmath>

#include "../ui/theme.hpp"

namespace app {

namespace {

const float ROW_H    = 46.0f;
const float BAR_H    = 4.0f;
const float BAR_GAP  = 14.0f;
const float DOT_X    = 18.0f;   /* dot center */
const float DOT_R    = 9.0f;
const float TEXT_X   = 42.0f;

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* This screen is drawn on the application background, not on a panel of our
 * own. It therefore follows the Borealis theme — light by default — and not the
 * ui::theme palette, which is tuned for the dark surfaces of the stream. Taking
 * one for the other gave white text on a white background. Only the state
 * colors stay ours: they read on both backgrounds. */
NVGcolor themed(const char *key)
{
    return brls::Application::getTheme()[key];
}

NVGcolor colorFor(StepState s)
{
    switch (s) {
        case StepState::Done:    return ui::theme::good;
        case StepState::Running: return nvgRGB(49, 130, 246);
        case StepState::Error:   return ui::theme::bad;
        default:                 return themed("brls/text_disabled");
    }
}

void drawCheck(NVGcontext *vg, float cx, float cy, NVGcolor ink)
{
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx - 4.0f, cy);
    nvgLineTo(vg, cx - 1.0f, cy + 3.5f);
    nvgLineTo(vg, cx + 4.5f, cy - 3.5f);
    nvgStrokeColor(vg, ink);
    nvgStrokeWidth(vg, 2.0f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
}

void drawCross(NVGcontext *vg, float cx, float cy, NVGcolor ink)
{
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx - 3.5f, cy - 3.5f);
    nvgLineTo(vg, cx + 3.5f, cy + 3.5f);
    nvgMoveTo(vg, cx + 3.5f, cy - 3.5f);
    nvgLineTo(vg, cx - 3.5f, cy + 3.5f);
    nvgStrokeColor(vg, ink);
    nvgStrokeWidth(vg, 2.0f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
}

}  // namespace

StepList::StepList(StepsPtr steps) : steps_(std::move(steps)) {}

float StepList::heightFor(size_t n)
{
    return BAR_H + BAR_GAP + ROW_H * (float)n;
}

void StepList::draw(NVGcontext *vg, float x, float y, float width, float height,
                    brls::Style style, brls::FrameContext *ctx)
{
    (void)style; (void)ctx; (void)height;
    if (!vg || !steps_ || steps_->empty()) return;

    const std::vector<Step> &steps = *steps_;
    const size_t n = steps.size();

    size_t done = 0;
    bool   failed = false;
    for (const Step &s : steps) {
        if (s.state == StepState::Done)  done++;
        if (s.state == StepState::Error) failed = true;
    }

    /* Progress bar: the one piece of information the list alone did not give —
     * how far along we are, at a glance, without counting rows. */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, BAR_H, BAR_H / 2.0f);
    nvgFillColor(vg, themed("brls/text_disabled"));
    nvgGlobalAlpha(vg, 0.25f);
    nvgFill(vg);
    nvgGlobalAlpha(vg, 1.0f);
    if (done > 0) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width * (float)done / (float)n, BAR_H, BAR_H / 2.0f);
        nvgFillColor(vg, failed ? ui::theme::bad : ui::theme::good);
        nvgFill(vg);
    }

    nvgFontFace(vg, ui::theme::font());
    float ry = y + BAR_H + BAR_GAP;

    for (size_t i = 0; i < n; i++) {
        const Step    &s  = steps[i];
        const float    cy = ry + ROW_H / 2.0f;
        const NVGcolor col = colorFor(s.state);

        /* Vertical thread joining the dots: it makes the list read as a
         * sequence rather than as independent rows. */
        if (i + 1 < n) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, x + DOT_X, cy + DOT_R + 2.0f);
            nvgLineTo(vg, x + DOT_X, cy + ROW_H - DOT_R - 2.0f);
            nvgStrokeColor(vg, (s.state == StepState::Done) ? ui::theme::good
                                                            : themed("brls/text_disabled"));
            nvgStrokeWidth(vg, 2.0f);
            nvgStroke(vg);
        }

        if (s.state == StepState::Running) {
            /* Spinning ring: a running step has to be seen moving, otherwise
             * nothing tells "working on it" apart from "stuck". */
            const float t     = (float)(nowMs() % 1000) / 1000.0f;
            const float start = t * 6.2832f;
            nvgBeginPath(vg);
            nvgCircle(vg, x + DOT_X, cy, DOT_R);
            nvgStrokeColor(vg, themed("brls/text_disabled"));
            nvgStrokeWidth(vg, 2.0f);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgArc(vg, x + DOT_X, cy, DOT_R, start, start + 1.9f, NVG_CW);
            nvgStrokeColor(vg, col);
            nvgStrokeWidth(vg, 2.4f);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);
        } else if (s.state == StepState::Pending) {
            nvgBeginPath(vg);
            nvgCircle(vg, x + DOT_X, cy, DOT_R);
            nvgStrokeColor(vg, themed("brls/text_disabled"));
            nvgStrokeWidth(vg, 2.0f);
            nvgStroke(vg);
        } else {
            nvgBeginPath(vg);
            nvgCircle(vg, x + DOT_X, cy, DOT_R);
            nvgFillColor(vg, col);
            nvgFill(vg);
            /* The check is stroked in the BACKGROUND color: it has to stand
             * out against the filled dot, whatever the theme. */
            const NVGcolor ink = themed("brls/background");
            if (s.state == StepState::Done) drawCheck(vg, x + DOT_X, cy, ink);
            else                            drawCross(vg, x + DOT_X, cy, ink);
        }

        /* The running step stands out bright; the ones still to come fade
         * away, so the eye lands in the right place. */
        NVGcolor titleCol = themed("brls/text_disabled");
        if (s.state == StepState::Running)    titleCol = themed("brls/text");
        else if (s.state == StepState::Done)  titleCol = themed("brls/text");
        else if (s.state == StepState::Error) titleCol = ui::theme::bad;

        nvgFontSize(vg, 19.0f);
        nvgFillColor(vg, titleCol);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, x + TEXT_X, cy, s.title.c_str(), nullptr);

        if (!s.detail.empty()) {
            nvgFontSize(vg, 14.0f);
            nvgFillColor(vg, (s.state == StepState::Error) ? ui::theme::bad
                                                           : themed("brls/text_disabled"));
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgText(vg, x + width - 8.0f, cy, s.detail.c_str(), nullptr);
        }

        ry += ROW_H;
    }
}

}  // namespace app
