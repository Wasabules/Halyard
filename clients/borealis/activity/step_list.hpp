/* StepList — the connection step indicator.
 *
 * The connection screen used to stack one Label per step, with an "o", a ">" or
 * an "OK" standing in for an icon and the subtitle glued to the title by a
 * dash. It was legible and nothing more: nothing showed the overall progress,
 * and the running step stood out only by its color.
 *
 * This view draws the list itself: dots joined by a thread, a check or a cross
 * depending on the outcome, an animated ring on the running step, and a
 * progress bar. It takes its colors from ui::theme, like the metrics panel and
 * the pause menu — same application, same visual language.
 *
 * Pleasant side effect: the steps no longer need pointers to Labels. The model
 * is a plain list of titles and states, which removes the dangling pointers the
 * liveness flag had to cover.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

namespace app {

enum class StepState { Pending, Running, Done, Error };

struct Step {
    std::string title;
    std::string detail;   /* detail shown on the right (code, measurement) */
    StepState   state = StepState::Pending;
};

using StepsPtr = std::shared_ptr<std::vector<Step>>;

class StepList : public brls::View {
public:
    explicit StepList(StepsPtr steps);

    void draw(NVGcontext *vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext *ctx) override;

    /* Height to reserve for n steps. */
    static float heightFor(size_t n);

private:
    StepsPtr steps_;
};

}  // namespace app
