/* ui::haptics — see haptics.hpp. */
#include "haptics.hpp"

#include <cstdlib>

extern "C" {
#include "../../../core/protocol/rumble_hid.h"
}

namespace ui {
namespace haptics {

namespace {

/* Module state, kept together. No function-level `static`: this outlives a
 * screen, and this repo has paid for four outages in that family. */
struct State {
    int    strength = 0;      /* 0..100 */
    bool   enabled  = true;
    double until    = 0.0;    /* switch-off time, 0 = nothing running */
    bool   running  = false;
    /* Last known instant, set by the heartbeat. `play` does not receive the
     * clock — its callers are button handlers, not render functions — and
     * threading it through would make every call site carry it. */
    double now      = 0.0;
};
State g;

/* Each intent is a (low, high) pair and a duration. The two motors do not feel
 * the same: the low one is felt in the palm, the high one under the fingers.
 * Separating them is what keeps two short patterns distinguishable — at equal
 * duration and strength only their balance differs, and the hand reads it. */
struct Pattern { unsigned char low, high; double duration; };

Pattern patternFor(Intent i)
{
    switch (i) {
        /* The limit is DRY and rather low: it imitates a mechanical stop. Made
         * long it would read as an error; it is a plain "no". */
        case Intent::Limit:     return { 200,  40, 0.045 };
        /* The two toggles differ by the DOMINANT motor, not by duration: nobody
         * compares two forty-millisecond durations, but a low rumble and a high
         * one are told apart instantly. */
        case Intent::ToggleOn:  return {  60, 180, 0.035 };
        case Intent::ToggleOff: return { 180,  60, 0.035 };
        /* A keystroke must sit at the edge of perception: a key that thumps is
         * tiring within ten words. */
        case Intent::Key:       return {  40,  90, 0.018 };
        /* The mode change is the only LONG pattern, which is what takes it out
         * of the confirmation family. */
        case Intent::Mode:      return { 150, 150, 0.110 };
    }
    return { 0, 0, 0.0 };
}

}  // namespace

void setStrength(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    g.strength = percent;
}

void setEnabled(bool on) { g.enabled = on; }

void play(Intent what)
{
    if (!g.enabled || g.strength <= 0) return;
    if (g.until > 0.0) return;          /* one already running: no stacking */

    const Pattern p = patternFor(what);
    if (p.duration <= 0.0) return;

    const int lo = (int)p.low  * g.strength / 100;
    const int hi = (int)p.high * g.strength / 100;
    if (lo == 0 && hi == 0) return;

    /* The switch-off is armed BEFORE the hardware call: if that call fails, the
     * heartbeat still clears the state, and we never end up with a module that
     * believes itself busy forever — a motor stuck on is the one defect this
     * module could not get away with. */
    g.running = true;
    g.until   = g.now + p.duration;
    rumble_hid_apply((unsigned char)lo, (unsigned char)hi);
}

void tick(double t)
{
    g.now = t;
    if (!g.running) return;
    /* A clock that goes backwards (session restarted, screen recreated) would
     * leave the motor on: we switch off in that case too, rather than wait for
     * a deadline that will never arrive. */
    if (t >= g.until || t < g.until - 2.0) {
        rumble_hid_apply(0, 0);
        g.running = false;
        g.until   = 0.0;
    }
}

}  // namespace haptics
}  // namespace ui
