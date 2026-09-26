/* ui::padtest - the gamepad test screen, SHARED by its two hosts.
 *
 * It is drawn in two places: in the Gamepad screen (outside a session) and on
 * top of the stream (during a session). Two different contexts, but ONE
 * drawing - copying it would let the two versions drift apart, which is exactly
 * the defect this repo paid for three times in one day (the clamped text, the
 * command parser, the focus guard).
 *
 * WHY TWO HOSTS. The tester compares what the CONSOLE reads against what we
 * SEND. But what we send only exists during a session: in the Gamepad screen
 * its right half is therefore always dark, and it cannot do the one thing it
 * exists for. It earns its keep during a session - hence its place in the
 * pause menu. Outside a session it is still useful for checking the button
 * mapping, which the left half alone shows.
 *
 * === S87 2026-08-29 - TWO HOSTS, TWO LAYOUTS ===
 *
 * The drawing used to be identical everywhere: two gamepads side by side, each
 * on half the width. In the Gamepad screen the right half is ALWAYS dark -
 * there is no session, so nothing is being sent - and the two gamepads split
 * the screen so that one of them could never show anything. The useful one was
 * half the size it should have been.
 *
 * The drawing stays unique; it is the LAYOUT that follows the host:
 *   `Mode::Simple`     - a single gamepad, large, centred, with the numeric
 *                        stick readout and a rumble test;
 *   `Mode::Comparison` - both, side by side, as before.
 *
 * This is not a setting: it is a property of the host. The Gamepad screen can
 * compare NOTHING, and the pause menu only exists during a session.
 */
#pragma once

#include <nanovg.h>

namespace ui {
namespace padtest {

/* Draws the two gamepads, the channel diagnostic and the exit hint.
 *
 * RETURNS TRUE once the user has held B long enough to leave. The host only has
 * to close its mode.
 *
 * The hold is measured HERE, deliberately: this module already reads the raw
 * gamepad state in order to draw it, so it sees B both pressed AND released.
 * The previous version asked the host to "arm" the measurement from its own
 * button handler - a fragile arrangement that failed exactly as one might
 * fear: inside the stream that handler is guarded by "is the menu open?", and
 * opening the tester CLOSES the menu. The arming therefore never happened, and
 * holding B did nothing. Reading the button where we draw it leaves nothing to
 * arm. */
enum class Mode {
    Simple,       /* Gamepad screen: what the console reads, in full size */
    Comparison,   /* pause menu: read versus sent */
};

bool draw(NVGcontext *vg, float x, float y, float w, float h, double t,
              Mode mode = Mode::Comparison);

}  // namespace padtest
}  // namespace ui
