/* pad_mouse_hid - the console side of "the Joy-Cons as a mouse".
 *
 * Everything that touches the hardware lives here: the pad, the six-axis
 * sensors, and the clock. The arithmetic lives in `pad_mouse.h`, which is pure
 * and tested offline - see its header for why the split is not tidiness.
 *
 * The whole module is a NO-OP off Switch: the desktop has a real mouse, and the
 * six-axis API does not exist there. The call sites therefore carry no `#if`.
 *
 * === WHY THE SENSOR IS STARTED LAZILY AND STOPPED EXPLICITLY ===
 *
 * `hidStartSixAxisSensor` is a service call, and the sensors keep running - and
 * draining the Joy-Con's battery - until they are stopped. So we start them the
 * first time gyro mode is actually entered, never at boot, and we stop them on
 * the way out. A resource taken and not returned is the class of mistake that
 * costs a console reboot on this platform (KB §7.3).
 */
#pragma once

#include <cstdint>

namespace padmouse {

/* Which mode is ACTIVE right now. This is session state, deliberately not the
 * persisted setting: the combination and the pause menu turn it on and off
 * without rewriting what the user chose as their preferred mode. */
enum class Mode { Off = 0, Stick = 1, Gyro = 2 };

/* Reads the console, converts, and posts the movement, the wheel and the clicks
 * through `shadow_input_post_mouse_*`. Call it once per frame, from the draw
 * path, and ONLY when the stream has the input - the caller owns that decision,
 * exactly as it does for `padforward::poll()`.
 *
 * `pad` is the caller's already-updated PadState, passed as an opaque pointer so
 * this header does not drag <switch.h> into the ones that include it. */
void poll(void *pad, uint64_t now_us);

/* True while the mouse is driving. The caller uses it to stop forwarding the
 * gamepad to the VM: in this mode the buttons ARE the mouse, and sending them
 * as gamepad buttons at the same time would make a game react to a click. */
bool active();

Mode mode();

/* Turns it on with the given mode, or off. Releases every held mouse button on
 * the way out - a click left down would be stuck in the remote machine, with no
 * way to release it once the mode is gone. */
void setMode(Mode m);

/* Cycles Off -> the user's preferred mode -> Off. This is what the button
 * combination and the pause menu entry both call, so the two can never disagree
 * about what "toggle" means. */
void toggle();

/* Called when the stream view goes away: stops the sensors and releases the
 * buttons. */
void shutdown();

/* === PM3 2026-09-02 - THE POINTING TEST ===
 *
 * Reported: "the gyroscope mode is not visible". And it could not be: the gyro
 * moves the REMOTE pointer, so the only way to judge it was to watch a cursor
 * drawn by the remote machine, which itself was pinned in place by PM2. Two
 * defects stacked, and no way to tell which one you were looking at.
 *
 * The test draws a dot at the position the module computes, LOCALLY, and sends
 * nothing to the VM. That separates the three questions that were tangled:
 * does the sensor answer, is the sensitivity right, and does the remote pointer
 * follow. Here you only ask the first two - and you can ask them without a
 * session at all.
 *
 * The point is in NORMALISED coordinates, 0..1 of the video area, so the caller
 * scales it to whatever it is drawing into. */
void setTestMode(bool on);
bool testMode();
/* Returns false while the test is off. */
bool testPoint(float *nx, float *ny);

/* === PM5 - WHAT THE TEST PAGE READS ===
 * Raw figures, not a verdict. A boolean "the gyro works" would be our opinion;
 * the angular velocities and the handle count are the measurement, and the eye
 * decides. `handles` at 0 with `answering` false is a sensor that was never
 * obtained - a different failure from one that answers zeroes. */
struct Diag {
    int   handles;      /* six-axis handles obtained (0 = none) */
    bool  answering;    /* at least one state has been read */
    float gx, gy;       /* last raw angular velocities, the CHOSEN sensor */
    /* PM8 - and each sensor apart, because "both Joy-Cons have a gyroscope" is
     * a claim, and the page must show it rather than assert it. [0] = left,
     * [1] = right; `live` says whether that one answered at all. */
    float g0x, g0y, g1x, g1y;
    bool  live0, live1;
    int   picked;       /* 0 = left, 1 = right */
    float sx, sy;       /* last pointer stick, -1..1 */
    float scroll;       /* last scroll stick */
    bool  btn[3];       /* left, right, middle, as the module sees them */
};
Diag diag();

/* Resets the test point to the centre. The page offers it, because a pointer
 * pushed into a corner cannot be brought back by a sensor that does not
 * answer - and that is precisely when you need it. */
void testRecenter();

}  // namespace padmouse
