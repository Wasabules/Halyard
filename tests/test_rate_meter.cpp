/* test_rate_meter - `ui/rate_meter.hpp`, the panel's rate measurement.
 *
 * === WHAT THIS SUITE EXISTS TO PREVENT (L21, 2026-08-29) ===
 *
 * The panel showed "decoded" at several THOUSAND BILLION frames per second after
 * a reconnection. Cause: `counter` and `last_` are unsigned, and
 * `counter - last_` on a counter that RESTARTS is not "minus 5000" but
 * 18,446,744,073,709,546,616. The exponential smoothing then held that value for
 * several seconds.
 *
 * The case was unreachable as long as nothing reset the counters between two
 * streams. It is the fix for THAT defect (L17) that opened it: the reader assumed
 * a monotonic counter without ever checking it.
 *
 * The counter-case is therefore the first test in this file, and it is written
 * with the REAL values from the incident. */
#include "../clients/borealis/ui/rate_meter.hpp"

#include <cstdio>
#include <cmath>

static int checks = 0, failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) { failures++; std::printf("  FAIL: %s\n", what); }
}

static void proche(float obtenu, float expected, float tol, const char *what)
{
    checks++;
    if (std::fabs(obtenu - expected) > tol) {
        failures++;
        std::printf("  FAIL: %s - got %.3f, expected %.3f +/- %.3f\n",
                    what, obtenu, expected, tol);
    }
}

int main()
{
    std::printf("== the panel's rate measurement (RateMeter) ==\n");

    /* --- THE COUNTER-CASE: the counter restarts at zero -------------------- */
    {
        ui::RateMeter m;
        int64_t t = 1000000;
        /* A running session: 60 frames per second for 5 s. */
        for (uint64_t n = 0; n <= 300; n += 60) { m.sample(n, t); t += 1000000; }
        check(m.value() > 30.0f && m.value() < 90.0f,
                 "a normal session returns a plausible rate");

        /* Reconnection: the counters are reset (L17). */
        m.sample(0, t);
        check(m.value() < 1.0f,
                 "a counter that GOES BACKWARDS resets the rate to zero, "
                 "it does not invent a delta of 1.8e19");
        check(!(m.value() > 1e6f),
                 "no absurd value after a restart (the reported defect)");

        /* And the measurement resumes cleanly afterwards. */
        t += 1000000; m.sample(50, t);
        proche(m.value(), 50.0f, 1.0f,
               "the first measurement after a restart is exact");
    }

    /* --- The basic computation --------------------------------------------- */
    {
        ui::RateMeter m(0.0f);            /* sans lissage : valeur instantanee */
        /* The priming happens at a NON-ZERO clock and with a non-zero counter:
         * `last_us_` is only set when the counter MOVES, and it guards against a
         * division by zero. Priming at (0, 0) therefore leaves the meter
         * unprimed - which made this test fail before it made anyone doubt the
         * code. */
        int64_t t = 1000000;
        m.sample(1, t);
        t += 1000000; m.sample(121, t);
        proche(m.value(), 120.0f, 0.01f, "120 frames in one second = 120 /s");
        t += 500000;  m.sample(181, t);
        proche(m.value(), 120.0f, 0.01f, "60 frames in half a second = 120 /s");
    }

    /* --- Silence is a real zero, not a dip -------------------------------- */
    {
        ui::RateMeter m(0.0f);
        int64_t t = 1000000;
        m.sample(1, t);
        t += 1000000; m.sample(61, t);
        proche(m.value(), 60.0f, 0.01f, "a rate established before the silence");
        /* Two seconds with nothing: not a zero yet, that would be a sampling
         * dip. */
        t += 2000000; m.sample(61, t);
        check(m.value() > 0.0f, "2 s with no frame is not zero yet");
        /* Past the threshold, it is a real cut. */
        t += 2000000; m.sample(61, t);
        proche(m.value(), 0.0f, 0.001f, "4 s with no frame = a clean zero");
    }

    /* --- The smoothing keeps a trace of the old without freezing it -------- */
    {
        ui::RateMeter m(0.7f);
        int64_t t = 1000000;
        m.sample(1, t);
        t += 1000000; m.sample(101, t);
        proche(m.value(), 100.0f, 0.01f, "the first value is not smoothed");
        t += 1000000; m.sample(101 + 50, t);
        proche(m.value(), 0.7f * 100.0f + 0.3f * 50.0f, 0.01f,
               "the next one mixes 70 % old and 30 % new");
    }

    /* --- Two samples at the same instant do not divide by zero ------------ */
    {
        ui::RateMeter m(0.0f);
        m.sample(0, 1000);
        m.sample(500, 1000);          /* meme horloge */
        check(std::isfinite(m.value()),
                 "two samples at the same instant return neither inf nor NaN");
    }

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
