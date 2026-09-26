/* ui::Screen - see the header. */

#include "haptics.hpp"
#include "pointer.hpp"
#include "screen_base.hpp"

#include "../devlink/devlink.hpp"
#include "i18n.hpp"
#include "sfx.hpp"

#include "settings.hpp"

#include <chrono>

namespace ui {

Screen::Screen()
{
    /* Focusable: this is what makes the buttons reach us. */
    this->setFocusable(true);
    this->setGrow(1.0f);

    /* And right away, Borealis' focus highlight is hidden. This view covers the
     * whole screen: its halo would frame the entire page, permanently, while
     * indicating nothing since there is never another focus candidate. The
     * useful focus is INTERNAL and the framework draws it, on the right row.
     * Forgetting this line on five screens is what produced the "blue glow":
     * putting it HERE makes it impossible to forget. */
    this->setHideHighlight(true);
}

/* === DEVL-4 2026-09-12 - DESCRIBING THE SCREEN, IN ONE PLACE ===============
 *
 * Wired HERE rather than screen by screen: every page outside the stream goes
 * through this `draw`, so one call site covers them all and a new screen is
 * described the day it is written - it only has to name itself (`devName`).
 * Eleven separate call sites would have drifted on the first one that forgot.
 *
 * PUBLISHED ONLY ON CHANGE. `publishState` copies a string, and this runs 60
 * times a second: rebuilding the line every frame would spend real time saying
 * the same thing. The comparison is on three integers - the name POINTER (the
 * slugs are literals, so identity is enough and it costs no strcmp), the focus
 * and the item count - which is exactly what changes when a script navigates.
 *
 * The label is read only when one of the three moved. A list that refreshes
 * asynchronously changes its count, so that case republishes too.
 *
 * `static` here is MODULE state, not session state: it is a "what did we last
 * say" cache, it holds nothing that must be reset between two sessions, and a
 * stale value costs one redundant publication. The repo's rule aims at the
 * other kind - see CLAUDE.md - and this is the side of it that is legitimate. */
void Screen::publishDevState()
{
    static const char *last_name  = nullptr;
    static int         last_focus = -2;
    static int         last_count = -1;

    const char *name = this->devName();
    ListScreen *list = this->innerList();
    const int focus = list ? list->focus() : -1;
    const int count = list ? (int)list->size() : 0;

    if (name == last_name && focus == last_focus && count == last_count) return;
    last_name = name; last_focus = focus; last_count = count;

    /* The focused row's text, when there is one. It is what lets a script say
     * "I am on Neo" rather than "I am on row 2" - the row moves, the name does
     * not. `focusedItem` is the only way in and it cannot lie about an empty
     * list, which is why nothing here indexes `items_` by hand.
     *
     * Note for whoever writes the script: this text is what is DISPLAYED, so it
     * is translated for anything but a machine name. Assert on `ecran=` and
     * `focus=`, which are stable; read `libelle=` to know where you are. */
    const char *label = "";
    if (list) {
        const auto *it = list->focusedItem();   /* ui::Item, named by the header */
        if (it) label = it->title.c_str();
    }

    devlink::publishState(devlink::composeState(name, focus, count, label));
}

void Screen::draw(NVGcontext *vg, float x, float y, float w, float h,
                 brls::Style style, brls::FrameContext *ctx)
{
    (void)style; (void)ctx;

    /* ONE clock read per frame, handed to everything that animates. Two separate
     * reads drift one frame apart, and you can see it on a highlight tracking a
     * row. */
    const double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    devlink::noteFrame(t);
    publishDevState();

    /* S90 - the equalizer follows the dock state. The probe is just a boolean
     * comparison as long as nothing changes; it lives here because EVERY screen
     * outside the stream goes through this `draw`, so docking or undocking the
     * console while navigating applies the right profile with nothing to notify.
     * During a session `stream_view` carries it - it does not come through
     * here. */
    Settings::followConsoleMode();

    /* The haptic feedback tick: this is what TURNS THE MOTORS OFF. Placed here
     * because EVERY screen outside the stream goes through this `draw` - a
     * module whose shutdown depended on one particular screen would stay on as
     * soon as you left that screen. */
    haptics::tick(t);

    paint(vg, x, y, w, h, t);

    /* S110 - the demo pointer, ON TOP of everything else: drawn any earlier it
     * would slide under the panels exactly when you want to see it. No effect
     * when the setting is off, and nonexistent on console. */
    pointer::draw(vg, t);

    /* S97 - was a footer hint just tapped? We ask AFTER rendering: acting while
     * the screen is being drawn would be reentrancy, which is what we avoid
     * everywhere else.
     *
     * The button label names the action, as on a controller. The triggers are
     * deliberately absent: they switch sections, which a tap on the rail
     * already does. */
    if (ListScreen *l = innerList()) {
        const std::string b = l->hintReleased();
        if (!b.empty()) {
            if      (b == "A") activate();
            else if (b == "B") back();
            else if (b == "X") buttonX();
            else if (b == "Y") buttonY();
        }
    }
}

void wireNavigation(brls::Activity *activity, Screen *screen)
{
    if (!activity || !screen) return;

    /* `allowRepeating` on the directions: holding a direction must scroll,
     * otherwise a twenty-entry list is walked one press at a time. */
    activity->registerAction("", brls::ControllerButton::BUTTON_UP,
        [screen](brls::View *) { return screen->up(); }, true, true);
    activity->registerAction("", brls::ControllerButton::BUTTON_DOWN,
        [screen](brls::View *) { return screen->down(); }, true, true);
    activity->registerAction("", brls::ControllerButton::BUTTON_LEFT,
        [screen](brls::View *) { return screen->left(); }, true, true);
    activity->registerAction("", brls::ControllerButton::BUTTON_RIGHT,
        [screen](brls::View *) { return screen->right(); }, true, true);

    /* S79 - triggers: previous / next section. With repeat, like the
     * directions: holding one must walk through the sections. */
    activity->registerAction("", brls::ControllerButton::BUTTON_LB,
        [screen](brls::View *) { return screen->triggerL(); }, true, true);
    activity->registerAction("", brls::ControllerButton::BUTTON_RB,
        [screen](brls::View *) { return screen->triggerR(); }, true, true);

    /* S91 - X and Y. No repeat: they change a TYPE or a quality factor, not a
     * value you sweep through. */
    activity->registerAction("", brls::ControllerButton::BUTTON_X,
        [screen](brls::View *) { return screen->buttonX(); });
    activity->registerAction("", brls::ControllerButton::BUTTON_Y,
        [screen](brls::View *) { return screen->buttonY(); });

    /* A has no repeat: activating something twice by accident is far worse than
     * having to press twice.
     *
     * === S88 - THE SOUND IS TIED TO THE RESULT ===
     * `activer()` returns false when there is nothing to activate - an
     * information row, a separator. Playing the sound BEFORE the call would make
     * it chime on a gesture that did nothing, which is the exact opposite of
     * what audio feedback is for: confirming that something happened. */
    activity->registerAction("", brls::ControllerButton::BUTTON_A,
        [screen](brls::View *) {
            const bool done = screen->activate();
            if (done) ui::sfx::play(ui::sfx::Sound::Confirm);
            return done;
        });

    /* === S71 2026-08-27 - B IS ALWAYS WIRED, AND IT POPS ===
     *
     * It used to be wired only on request, assuming Borealis would provide the
     * default back behaviour. That was true AS LONG AS our screens were wrapped
     * in an `AppletFrame`: that frame, and it alone, registered B -> pop. Moving
     * to `createContentView()` removed the frame - so nothing handled B any
     * more, and three screens became dead ends you could only leave by killing
     * the application.
     *
     * So we ask the screen first: if it has something to close (a dialog, a
     * mode) it consumes the press. Otherwise we pop it ourselves. Going back no
     * longer depends on a frame we no longer use. */
    activity->registerAction(ui::tr("action/back"), brls::ControllerButton::BUTTON_B,
        [screen](brls::View *) {
            if (screen->back()) {             /* the screen had something to close */
                ui::sfx::play(ui::sfx::Sound::Back);
                return true;
            }
            /* Two sounds here, and it is not redundancy: `Retour` says "the
             * button did something", `Fermer` says "a page went away". The case
             * just above pops nothing - a mode was closed - so it must not play
             * the second one. */
            ui::sfx::play(ui::sfx::Sound::Back);
            ui::sfx::play(ui::sfx::Sound::Close);
            brls::Application::popActivity();
            return true;
        });

    brls::Application::giveFocus(screen);
}

}  // namespace ui
