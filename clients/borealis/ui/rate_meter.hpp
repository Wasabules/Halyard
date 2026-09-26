/* ui::RateMeter - instantaneous rate derived from a cumulative counter.
 *
 * A counter that only ever goes up says nothing about what is happening NOW, and
 * dividing it by the session duration gives an average since the start. This
 * class measures between two REAL changes of the counter: sampling at a fixed
 * period a counter that is published less often gave every other interval a
 * value of zero, hence a reading that flipped between 0 and its true value.
 *
 * The moving average smooths what is left: an encoder's bitrate varies sharply
 * from frame to frame, a key frame weighing several times an intermediate one.
 */
#pragma once

#include <cstdint>

namespace ui {

class RateMeter {
public:
    /* `smoothing` = share of the old value kept, between 0 and 1. */
    explicit RateMeter(float smoothing = 0.7f) : smoothing_(smoothing) {}

    /* Call as often as you like: only changes count. `now_us` is a monotonic
     * clock in microseconds. */
    void sample(uint64_t counter, int64_t now_us)
    {
        /* === L21 2026-08-29 - A COUNTER THAT GOES BACKWARDS IS NOT A DELTA ===
         *
         * `counter` and `last_` are UNSIGNED: if the counter restarts at zero,
         * `counter - last_` is not "minus 5000", it is
         * 18,446,744,073,709,546,616. The displayed rate then jumps to several
         * trillion per second, and the exponential smoothing keeps it there for
         * several seconds.
         *
         * This case did not exist while nothing ever reset the counters - it is
         * L17, by fixing exactly the measurements that SURVIVED their own
         * session, that made it reachable. Fixing one flaw uncovered the next:
         * the reset was right, it was the reader that assumed a monotonic
         * counter without checking.
         *
         * A step backwards means one thing only: the source restarted. We start
         * again from there, without inventing a delta. */
        if (counter < last_) {
            last_    = counter;
            last_us_ = now_us;
            value_   = 0.0f;
            return;
        }
        if (counter != last_) {
            if (last_us_ > 0 && now_us > last_us_) {
                float inst = (float)(counter - last_) * 1000000.0f
                           / (float)(now_us - last_us_);
                value_ = (value_ > 0.0f) ? value_ * smoothing_ + inst * (1.0f - smoothing_)
                                         : inst;
            }
            last_    = counter;
            last_us_ = now_us;
        } else if (last_us_ > 0 && now_us - last_us_ > IDLE_US) {
            /* Nothing at all for three seconds: this is a real zero, not a gap
             * in the sampling. */
            value_ = 0.0f;
        }
    }

    float value() const { return value_; }

private:
    static const int64_t IDLE_US = 3000000;

    float    smoothing_;
    float    value_   = 0.0f;
    uint64_t last_    = 0;
    int64_t  last_us_ = 0;
};

}  // namespace ui
