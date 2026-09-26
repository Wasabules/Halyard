/* ui::mousetest - the controller-mouse test page, SHARED by its two hosts.
 *
 * Drawn in two places, like the gamepad tester it is modelled on: in the
 * Gamepad settings screen (outside a session) and on top of the stream (from
 * the pause menu). One drawing, two hosts - copying it would let the two
 * versions drift apart, which is the defect this repo has paid for repeatedly.
 *
 * === WHY A PAGE, AND NOT THE DOT IT REPLACES (PM5, 2026-09-02) ===
 *
 * The first attempt drew a single dot over the video. It answered one question
 * - does the pointer move - and the report that followed was "the yellow dot
 * does not move when I orient the Joy-Cons". At that point the dot could not
 * say WHICH of four things had failed: no six-axis handle obtained, a handle
 * that returns no state, a state whose values are zero, or a scale so small the
 * movement is invisible. Four causes, one symptom, and a dot has no room to
 * tell them apart.
 *
 * So the page shows the CHAIN, stage by stage, in raw figures: how many sensor
 * handles were obtained, whether any of them answers, the angular velocities as
 * they arrive, the sticks, the buttons - and then the pointer those produce. A
 * value that stays at zero names its own stage.
 *
 * It shows NUMBERS, not a verdict. "The gyroscope works" would be our opinion;
 * `gx = 0.0000` with two handles obtained is a measurement, and it says
 * something different from `gx = 0.0031` with a pointer that does not visibly
 * move. Those two look identical on a dot and call for opposite fixes.
 *
 * NOTHING IS SENT TO THE REMOTE MACHINE while the page is up - not the
 * movement, not the buttons. It is therefore safe to open mid-game, which is
 * exactly when you want to check a sensitivity.
 */
#pragma once

#include <nanovg.h>

namespace ui {
namespace mousetest {

/* Draws the pointer area and the readouts. Returns TRUE once B has been held
 * long enough to leave; the host then closes its mode.
 *
 * The hold is measured HERE for the same reason as in the gamepad tester: this
 * module already reads the raw pad in order to draw it, so it sees B pressed
 * AND released. Asking the host to arm the measurement from its own button
 * handler failed there in a way worth not repeating - inside the stream that
 * handler is guarded by "is the menu open?", and opening a tester closes the
 * menu, so the arming never happened. */
bool draw(NVGcontext *vg, float x, float y, float w, float h, double t);

}  // namespace mousetest
}  // namespace ui
