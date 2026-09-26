/* ui::Screen - the base class of every full-screen page in the framework.
 *
 * === WHY IT EXISTS ===
 *
 * All six screens repeated the SAME four lines: make yourself focusable (so the
 * buttons reach you), hide Borealis' focus highlight (which would frame the
 * whole page), read the clock once, and override `draw` with the same skeleton.
 *
 * Twenty-four repetitions of identical behaviour - and this is not about
 * elegance: it is exactly what let the "blue glow" survive on FIVE screens while
 * the fix already existed on the sixth. A Borealis quirk absorbed here is
 * absorbed for everyone, once.
 *
 * === WHAT IT ENFORCES, AND WHY ===
 *
 * `draw` is FINAL: a screen does not override it, it overrides `peindre`, which
 * receives the time as a parameter. That is what guarantees a SINGLE clock read
 * per frame - two animations each reading their own drift one frame apart, and
 * you can see it on a highlight tracking a row.
 *
 * Navigation is virtual and DOES NOTHING by default. A progress screen has no
 * focus: it overrides nothing and ignores the buttons without having to say so.
 */
#pragma once

#include <borealis.hpp>

#include "screen.hpp"

namespace ui {

class Screen : public brls::View {
public:
    Screen();

    /* The whole rendering of the screen. `t` is the current instant, in seconds,
     * read ONCE per frame and handed to everything that animates. */
    virtual void paint(NVGcontext *vg, float x, float y, float w, float h,
                         double t) = 0;

    /* --- Navigation ------------------------------------------------------
     * Returning `true` means the button was CONSUMED. `retour()` has one extra
     * consequence: returning false lets Borealis pop the activity. */
    virtual bool up()    { return false; }
    virtual bool down()     { return false; }
    virtual bool left()  { return false; }
    virtual bool right()  { return false; }
    virtual bool activate() { return false; }
    virtual bool back()  { return false; }

    /* === S79 2026-08-29 - THE TRIGGERS SWITCH SECTIONS ===
     *
     * They were not wired anywhere. They are wired here rather than in each
     * screen, for the same reason as the four directions: a quirk or an
     * oversight in one place beats the same one in six.
     *
     * A screen with no sections does not override them and returns `false`: the
     * button is then not consumed and Borealis is free to use it. */
    virtual bool triggerL() { return false; }
    virtual bool triggerR() { return false; }

    /* === S91 2026-08-29 - X AND Y ===
     *
     * Added for the graphic equalizer, where five bands each carry four
     * parameters: the four directions plus the two triggers no longer suffice.
     * Like the rest, they are wired HERE and not in the screen - a screen
     * registering its own actions would eventually register them differently
     * from the others.
     *
     * A screen that does not override them returns `false`: the button is then
     * not consumed and Borealis is free to use it. */
    virtual bool buttonX() { return false; }
    virtual bool buttonY() { return false; }

    /* === S97 - THE FOOTER HINTS ARE BUTTONS ===
     *
     * A screen that returns its list here gets touchable footer hints for free:
     * "A Connect" runs the same function as the A button. Two separate paths
     * would eventually stop doing the same thing, so there is only one - the
     * button path.
     *
     * A screen that returns nothing (a progress screen) has no touchable hints
     * and has nothing to override. */
    virtual ListScreen *innerList() { return nullptr; }

    /* === DEVL-4 2026-09-12 - THE NAME A SCRIPT NAVIGATES BY ==================
     *
     * `devlink state` answered `ecran=inconnu` on every screen: the describing
     * API existed in full and had ZERO callers, so a script could send `nav bas`
     * and `btn a` but could never check WHERE it had landed. That is dead
     * reckoning - it holds until a screen changes order, and then the script
     * quietly does something else.
     *
     * A STABLE SLUG, NOT THE TITLE. The visible title is translated and may be
     * reworded; a script keyed on it breaks when the locale changes, which is
     * the worst kind of breakage - it happens to someone else, later, for a
     * reason that has nothing to do with what they were testing. So: lowercase
     * ASCII, never shown to a user, changed only on purpose.
     *
     * The default is "inconnu" so a screen that forgets to override says so
     * plainly instead of impersonating another one. */
    virtual const char *devName() const { return "inconnu"; }

private:
    /* Publishes `ecran=… nb=… focus=… libelle=…` when it CHANGES. Called from
     * `draw`, so every screen is described without doing anything. */
    void publishDevState();

public:

    void draw(NVGcontext *vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext *ctx) override final;
};

/* Wires an activity's buttons to its screen.
 *
 * Call it from `onContentAvailable`. The four directions, A and B are ALL wired.
 * For B the screen is asked first - if it has a dialog or a mode to close it
 * consumes the press - and otherwise we pop the activity ourselves.
 *
 * B was originally wired only on request, assuming Borealis would provide the
 * default back behaviour. That held as long as our screens were wrapped in an
 * `AppletFrame`; moving to `createContentView()` removed that frame, and three
 * screens became dead ends (S71).
 *
 * The hint labels are NOT set here: they belong to the screen, which alone
 * knows what A means on it. */
void wireNavigation(brls::Activity *activity, Screen *screen);

}  // namespace ui
