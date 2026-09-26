/* ui::pad - draws a gamepad with its live state.
 *
 * === WHY THIS SCREEN EXISTS ===
 *
 * A full day of gamepad defects, every one of them invisible by any other
 * means:
 *   - four phantom gamepads on the remote machine, because we kept repeating
 *     the plug-in announcement (G49);
 *   - the axes sent at the wrong scale, evdev yielding +/-32767 where we only
 *     transmitted 0..255 (G48);
 *   - Start sent but INVISIBLE, press and release leaving within the same
 *     microsecond (G51).
 *
 * None of the three shows up in a log: you have to compare what the CONSOLE
 * reads against what we SEND. Hence this screen's principle, and its only
 * reason to exist: show BOTH sides at once.
 *
 * `read` comes from the console (libnx), `sent` from what the input module
 * actually transmitted. When the two diverge, the defect is OURS, between the
 * read and the emission - and that is exactly where all three lived.
 *
 * The module is STATELESS: hand it two states and a timestamp, it draws. It
 * reads neither the gamepad nor the clock. That is what makes it usable from
 * any screen, and testable by hand by feeding it values.
 */
#pragma once

#include <nanovg.h>

namespace ui {
namespace pad {

/* The buttons, in `padmap::Btn` order. The drawing code uses them as indices:
 * keep the two tables aligned. */
enum Button {
    A = 0, B, X, Y,
    L, R, ZL, ZR,
    MINUS, PLUS,
    STICK_L, STICK_R,
    UP, DOWN, LEFT, RIGHT,
    NUM_BUTTONS
};

/* A gamepad's state at one instant. Values only: copyable, comparable. */
struct State {
    bool  pressed[NUM_BUTTONS] = {};
    /* Sticks in [-1, 1]. Positive Y = UPWARDS, a display convention: the
     * caller does the conversion, so that the drawing does not depend on
     * whichever sign an input layer picked. */
    float lx = 0.0f, ly = 0.0f;
    float rx = 0.0f, ry = 0.0f;
    /* Analog triggers in [0, 1]. */
    float zl = 0.0f, zr = 0.0f;
    /* True if the source answered. False = "no gamepad", which is NOT the same
     * thing as "everything released" and must be visible as such. */
    bool  present = false;
};

/* Draws the gamepad inside the given frame, at whatever scale fits it.
 *
 * `tint` colours what is active: we use it to tell the two sides apart at a
 * glance (what the console reads / what we send). `t` drives the press
 * animations; it comes from the caller. */
/* `gamepad`: 0 for the one we READ, 1 for the one we SEND. Both can be drawn
 * side by side, and their stick trails must stay separate - otherwise the
 * stroke alternates between two different positions. */
void draw(NVGcontext *vg, float x, float y, float w, float h,
          const State &e, NVGcolor tint, double t, int gamepad = 0);

/* Width-to-height ratio of the drawing. The caller uses it to reserve a frame
 * with the right proportions instead of distorting the gamepad. */
float aspect();

}  // namespace pad
}  // namespace ui
