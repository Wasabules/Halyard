/* GesturesView - what each touch gesture does.
 *
 * Nine rows: tap / double-tap / long press, for one, two and three fingers.
 * Each row cycles through the same list of actions.
 *
 * The warning carried by a "double-tap" row is not decorative: giving the
 * double-tap an action of its own FORCES the single tap to wait out the window
 * (0.4 s) before acting, since before that it cannot be told apart. It is the
 * only setting on this screen that costs latency, and the screen says so.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"
#include "../ui/screen.hpp"

class GesturesView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "gestes"; }

    GesturesView();

    void rebuild();

    bool up()   override { return screen_.up(); }
    bool down()    override { return screen_.down(); }
    bool left() override;
    bool right() override;

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

    /* S97 - the footer of this screen is touchable. */
    ui::ListScreen *innerList() override { return &screen_; }

private:
    ui::ListScreen screen_;
    bool apply(const ui::Item &it);
};
