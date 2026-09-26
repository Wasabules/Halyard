/* inject.hpp - synthetic input, driven from the development machine.
 *
 * See inject.h for WHY the timing is a separate pure module, and WHERE this is
 * plugged into Borealis. This header is only the C++ face the command handler
 * calls; nothing here decides anything.
 *
 * Every call is safe from any thread: the commands arrive on the log drain
 * thread, the sampling happens on the render thread, and one lock separates
 * them. Nothing blocks: a press is recorded, never awaited.
 */
#ifndef DEVLINK_INJECT_HPP
#define DEVLINK_INJECT_HPP

namespace devlink {

/* `devcmd_index` is a `devcmd_button_t`; `ms` <= 0 takes the default. */
bool pressButton(int devcmd_index, int ms);

/* `devcmd_dir` is a `devcmd_dir_t`. Presses the cross AND the NAV button, as a
 * real controller does - see the comment in inject.cpp. */
bool pressDirection(int devcmd_dir, int ms);

/* Stick position, each coordinate in -1..1 (clamped). */
bool moveStick(bool right, float x, float y, int ms);

/* A touch travelling from (x0,y0) to (x1,y1) over `ms`. A tap passes the same
 * point twice. Coordinates are in the screen's pixels. */
bool touch(float x0, float y0, float x1, float y1, int ms);

/* Releases everything at once. */
void releaseAll();

/* === THE SECOND READER, and why it exists =================================
 *
 * Hooking the platform's input manager was supposed to be "the lowest common
 * layer", and for the menus it is. It is NOT for the stream: `stream_view.cpp`
 * reads `padGetButtons`, `padGetStickPos` and `hidGetTouchScreenStates`
 * DIRECTLY from libnx (K22 says so in its own comment), and so does
 * `ui/hold_exit.hpp`, which is exactly the page a script most needs to drive -
 * holding a button is the only way out of it.
 *
 * Two readers, then, and both must see the same synthetic input. These
 * accessors are the second one: our own code ORs them into what libnx returned.
 * No Borealis patch is involved here - these are our files.
 *
 * Measured the day it was written: injecting only into Borealis moved every
 * menu and left the stream and the hold-to-exit pages completely inert, which
 * looked like "injection does not work" rather than "it works in one of the two
 * places the app reads input". */

/* The held buttons as a libnx `HidNpadButton` mask. 0 off console, and 0 when
 * nothing is being injected - so the caller's `|=` costs nothing at rest. */
unsigned long long injectedNpadMask();

/* The injected stick, in libnx units (-32767..32767). Returns false when that
 * stick is not being driven, in which case the caller keeps the real one. */
bool injectedStick(bool right, int *x, int *y);

/* The injected touch, in console pixels. Same contract. */
bool injectedTouch(float *x, float *y);

}  // namespace devlink

#endif /* DEVLINK_INJECT_HPP */
