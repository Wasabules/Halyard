/* ui::touch - the state of the finger, read where it is actually available.
 *
 * Touch does not reach our screens through the Borealis event system: they draw
 * themselves and have no view tree for an event to be delivered to. So we read
 * the touchscreen DIRECTLY, once per frame, exactly as the stream view has
 * always done.
 *
 * That is also what makes the gesture correct: the finger position and the list
 * geometry are then read in the SAME frame. Going through a deferred event would
 * make the gesture answer against a layout from before the scroll - correct
 * until you have scrolled, wrong afterwards, and invisible on a short list.
 *
 * Off console there is nothing to read: `ui_touch_current()` reports no finger
 * and the rest of the framework behaves as if nobody were touching the screen.
 *
 * === THE PS VITA, 2026-09-13 ==========================================
 *
 * The sentence above was written when the Switch was the only console, and the
 * `#ifdef __SWITCH__` under it therefore meant "a console". Reported from real
 * hardware: the touchscreen did nothing at all on the Vita - not a mapping
 * problem, the panel was simply never read.
 *
 * The Vita branch does NOT sample the panel itself. Borealis already does,
 * and - this is the point - it converts the reading through
 * `SceTouchPanelInfo`'s active area, because the front panel does not report
 * in screen pixels. Writing that conversion a second time here is exactly the
 * duplicated path this repo keeps paying for: the two copies drift, and the
 * one that drifts is the one nobody re-measures. So we ask the platform's
 * input manager for the state it already knows how to produce, and inherit its
 * coordinate space - which is also the space the callers hit-test against.
 *
 * `sceTouchPeek` is a peek, not a read: sampling it again on our frame costs
 * nothing and consumes no event.
 */
#ifndef UI_TOUCH_H
#define UI_TOUCH_H

#include <stdbool.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#if defined(__vita__) || defined(__psp2__)
/* C++ only - every caller of this header is a .cpp, checked before writing
 * this. A C caller would break the build here rather than silently lose the
 * touchscreen, which is the failure mode worth having. */
#include <vector>
#include <borealis/core/application.hpp>
#endif

typedef struct {
    bool  active;   /* a finger is touching the screen at this instant */
    float x, y;    /* in screen pixels; meaningless when `actif` is false */
} ui_touch_t;

static inline ui_touch_t ui_touch_current(void)
{
    ui_touch_t t = { false, 0.0f, 0.0f };
#ifdef __SWITCH__
    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1) > 0 && ts.count > 0) {
        t.active = true;
        t.x = (float)ts.touches[0].x;
        t.y = (float)ts.touches[0].y;
    }
#endif
#if defined(__vita__) || defined(__psp2__)
    brls::InputManager* im = brls::Application::getPlatform()
                                 ? brls::Application::getPlatform()->getInputManager()
                                 : nullptr;
    if (im) {
        std::vector<brls::RawTouchState> states;
        im->updateTouchStates(&states);
        for (size_t i = 0; i < states.size(); i++) {
            if (!states[i].pressed) continue;
            t.active = true;
            t.x = states[i].position.x;
            t.y = states[i].position.y;
            break;
        }
    }
#endif
    return t;
}

#endif /* UI_TOUCH_H */
