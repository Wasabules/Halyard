/* ConnectingView — the connection screen, drawn by the in-house framework.
 *
 * It takes over the DISPLAY only. The model — a `shared_ptr` to a vector of
 * steps that the background thread updates — stays exactly the one from before,
 * and that is deliberate: those 800 lines of bootstrap sequence are the most
 * battle-tested part of the repo, and touching them for a change of appearance
 * would be a bad trade.
 *
 * The view READS that model every frame and translates it into `ui::Step`. The
 * copy covers seven small structs: it costs nothing, and it spares the
 * framework any dependency on the old screen's vocabulary.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"

#include "step_list.hpp"      /* app::StepsPtr, app::StepState — the model */
#include "../ui/steps.hpp"

class ConnectingView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "connexion"; }

    explicit ConnectingView(app::StepsPtr steps);

    void setTitle(const std::string &t, const std::string &subtitle);
    void setMessage(const std::string &m, bool error);
    void setHints(std::vector<ui::StepScreen::Hint> h) { screen_.setHints(std::move(h)); }

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

private:
    app::StepsPtr  steps_;
    ui::StepScreen screen_;
};
