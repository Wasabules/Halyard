/* ConnectingView — see the header. */

#include "connecting_view.hpp"

#include <chrono>

namespace {

/* Translation from the old model's vocabulary into the framework's. It is
 * written out rather than done with a cast: the two enums are close today, but
 * nothing guarantees they stay that way, and a silent cast would break without
 * saying a word. */
ui::StepState convert(app::StepState s)
{
    switch (s) {
        case app::StepState::Running: return ui::StepState::Running;
        case app::StepState::Done:    return ui::StepState::Done;
        case app::StepState::Error:   return ui::StepState::Failed;
        default:                      return ui::StepState::Waiting;
    }
}

}  // namespace

ConnectingView::ConnectingView(app::StepsPtr steps)
    : steps_(std::move(steps))
{
}

void ConnectingView::setTitle(const std::string &t, const std::string &subtitle)
{
    screen_.setTitle(t);
    screen_.setSubtitle(subtitle);
}

void ConnectingView::setMessage(const std::string &m, bool error)
{
    screen_.setMessage(m, error);
}

void ConnectingView::paint(NVGcontext *vg, float x, float y, float w, float h,
                        double t)
{

    /* We re-read the model on EVERY frame rather than subscribing to its
     * changes: the background thread writes into it knowing nothing about the
     * display, and that is precisely what stops a callback from holding a
     * reference to a view that could disappear. */
    if (steps_) {
        std::vector<ui::Step> v;
        v.reserve(steps_->size());
        for (const app::Step &s : *steps_) {
            ui::Step e;
            e.title  = s.title;
            e.detail = s.detail;
            e.state   = convert(s.state);
            v.push_back(std::move(e));
        }
        screen_.setSteps(std::move(v));
    }
    screen_.draw(vg, x, y, w, h, t);
}
