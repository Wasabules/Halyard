/* test_qt_rate_meter - the rate meter the metrics HUD derives its live rates
 * with (MET1). The one bug it exists to prevent is L21: an unsigned counter that
 * steps backwards at a session reset reading as a rate of ~1.8e19/s.
 */
#include <cstdio>
#include <cmath>

#include "../clients/qt/rate_meter.hpp"

using halyard::RateMeter;

static int checks = 0, failures = 0;

static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond) { failures++; std::printf("  FAIL %s\n", what); }
}

static void near(float got, float want, float tol, const char *what)
{
    checks++;
    if (std::fabs(got - want) > tol) {
        failures++;
        std::printf("  FAIL %-48s got %.3f, expected %.3f\n", what, got, want);
    }
}

int main()
{
    std::printf("== Qt rate meter (MET1 2026-10-03) ==\n");

    /* 60 units in one second -> 60/s. The first sample only sets the baseline
     * (there is no prior timestamp to measure against), so a real counter is
     * primed then measured - which is how the live poll feeds it. */
    {
        RateMeter m;
        m.sample(1000, 10000000);   /* baseline */
        m.sample(1060, 11000000);   /* +60 in 1 s */
        near(m.value(), 60.0f, 0.01f, "60 units / 1 s = 60/s");
    }

    /* Smoothing: a step from a steady 60 toward 120 keeps 70% of the old. */
    {
        RateMeter m(0.7f);
        m.sample(1000, 10000000);   /* baseline */
        m.sample(1060, 11000000);   /* value = 60 */
        m.sample(1180, 12000000);   /* inst = 120 -> 0.7*60 + 0.3*120 = 78 */
        near(m.value(), 78.0f, 0.01f, "smoothing keeps 70% of the old value");
    }

    /* L21: the counter restarts at a new session. The result must be 0, not a
     * gigantic number from the unsigned wrap. */
    {
        RateMeter m;
        m.sample(1000000, 10000000);
        m.sample(1000600, 11000000);       /* 600/s */
        ok(m.value() > 0.0f, "a rate is established before the reset");
        m.sample(0, 12000000);             /* the source restarted */
        near(m.value(), 0.0f, 0.001f, "a backwards step reads as 0, not ~1.8e19");
        m.sample(30, 13000000);
        near(m.value(), 30.0f, 0.01f, "it measures cleanly again after the reset");
    }

    /* Idle: no change for more than three seconds is a real zero. */
    {
        RateMeter m;
        m.sample(0, 10000000);
        m.sample(60, 11000000);            /* 60/s */
        m.sample(60, 15000000);            /* 4 s with no change -> 0 */
        near(m.value(), 0.0f, 0.001f, "no change for >3 s reads as 0");
    }

    /* An unchanged counter within the idle window keeps the last rate. */
    {
        RateMeter m;
        m.sample(1000, 10000000);          /* baseline */
        m.sample(1060, 11000000);
        m.sample(1060, 11500000);          /* 0.5 s, still no change */
        near(m.value(), 60.0f, 0.01f, "a brief gap does not zero the rate");
    }

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
