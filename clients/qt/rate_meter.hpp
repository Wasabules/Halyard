/* RateMeter - instantaneous rate from a cumulative counter.
 *
 * === MET1 2026-10-03 — A COPY OF BOREALIS'S, ON PURPOSE =====================
 *
 * This is `clients/borealis/ui/rate_meter.hpp`, ported verbatim. It is NOT
 * shared: the two clients share `halyard-core` and nothing else
 * (check-core-independence.py refuses an include crossing between them), and
 * this belongs to neither core nor a protocol - it is a UI helper. Twelve lines
 * of pure arithmetic are cheaper to copy than to hoist into core, and copying
 * keeps each client's HUD its own.
 *
 * Why it exists: a counter that only goes up says nothing about NOW, and
 * dividing by the session duration gives an average since the start. This
 * measures between two REAL changes of the counter, so sampling faster than the
 * counter is published does not read every other interval as zero. The moving
 * average smooths an encoder's frame-to-frame swings.
 *
 * The backwards-step guard (L21) matters here too: core resets its counters at
 * each session, and an unsigned `counter - last_` across a reset is not a small
 * negative, it is ~1.8e19 - a rate of several trillion per second that the
 * smoothing would then hold for seconds. A step back means the source
 * restarted; begin again from there.
 *
 * Pure: <cstdint> only. Tested by tests/test_qt_rate_meter.cpp.
 */
#pragma once

#include <cstdint>

namespace halyard {

class RateMeter {
public:
    explicit RateMeter(float smoothing = 0.7f) : smoothing_(smoothing) {}

    void sample(uint64_t counter, int64_t now_us)
    {
        if (counter < last_) {                 /* the source restarted (L21) */
            last_ = counter;
            last_us_ = now_us;
            value_ = 0.0f;
            return;
        }
        if (counter != last_) {
            if (last_us_ > 0 && now_us > last_us_) {
                const float inst = (float)(counter - last_) * 1000000.0f
                                 / (float)(now_us - last_us_);
                value_ = (value_ > 0.0f)
                             ? value_ * smoothing_ + inst * (1.0f - smoothing_)
                             : inst;
            }
            last_ = counter;
            last_us_ = now_us;
        } else if (last_us_ > 0 && now_us - last_us_ > IDLE_US) {
            value_ = 0.0f;                      /* a real zero, not a sampling gap */
        }
    }

    float value() const { return value_; }
    void  reset() { value_ = 0.0f; last_ = 0; last_us_ = 0; }

private:
    static const int64_t IDLE_US = 3000000;
    float    smoothing_;
    float    value_   = 0.0f;
    uint64_t last_    = 0;
    int64_t  last_us_ = 0;
};

}  // namespace halyard
