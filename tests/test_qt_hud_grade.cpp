/* test_qt_hud_grade - the HUD's reading-to-colour rule (OV5/HUD1).
 *
 * The thresholds are Borealis's, value for value, so that the same reading
 * carries the same colour in both clients. What is pinned here is the part
 * that cannot be seen by reading the code twice: which side of each boundary
 * is which, and that the "lower is better" direction is not accidentally the
 * "higher is better" one. A reading EXACTLY on a threshold is the case that
 * was wrong when this was written inline.
 */
#include <cstdio>

#include "../clients/qt/hud_grade.hpp"

using halyard::Grade;
using halyard::gradeHi;
using halyard::gradeLo;
using halyard::badIfAny;
using halyard::warnIfAny;
namespace grade = halyard::grade;

static int checks = 0, failures = 0;

static const char *name(Grade g)
{
    switch (g) {
    case Grade::Neutral: return "neutral";
    case Grade::Good:    return "good";
    case Grade::Warn:    return "warn";
    case Grade::Bad:     return "bad";
    }
    return "?";
}

static void is(Grade got, Grade want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL %-52s got %s, expected %s\n", what, name(got),
                    name(want));
    }
}

int main()
{
    std::printf("== Qt HUD grading (OV5/HUD1 2026-10-03) ==\n");

    /* --- higher is better ------------------------------------------------ */
    is(gradeHi(60, 30, 55), Grade::Good, "60 fps, good at 55");
    is(gradeHi(40, 30, 55), Grade::Warn, "40 fps, between the two");
    is(gradeHi(10, 30, 55), Grade::Bad,  "10 fps, under the warn line");
    /* The boundaries. Both inclusive: exactly good is good, exactly warn is
     * warn - the one thing two clients must agree on. */
    is(gradeHi(55, 30, 55), Grade::Good, "exactly on good is good");
    is(gradeHi(30, 30, 55), Grade::Warn, "exactly on warn is warn");
    is(gradeHi(29.999, 30, 55), Grade::Bad, "just under warn is bad");
    is(gradeHi(0, 30, 55), Grade::Bad, "zero is bad, not neutral");

    /* --- lower is better ------------------------------------------------- */
    is(gradeLo(5, 80, 30), Grade::Good, "5 ms rtt is good");
    is(gradeLo(50, 80, 30), Grade::Warn, "50 ms rtt is between the two");
    is(gradeLo(200, 80, 30), Grade::Bad, "200 ms rtt is bad");
    is(gradeLo(30, 80, 30), Grade::Good, "exactly on good is good");
    is(gradeLo(80, 80, 30), Grade::Warn, "exactly on warn is warn");
    is(gradeLo(80.001, 80, 30), Grade::Bad, "just over warn is bad");
    is(gradeLo(0, 80, 30), Grade::Good, "zero of a bad thing is good");

    /* The two directions must not agree, or one of them is the other written
     * twice - which is exactly how an inverted threshold survives review. */
    checks++;
    if (gradeHi(10, 30, 55) == gradeLo(10, 30, 55)) {
        failures++;
        std::printf("  FAIL the two directions grade 10 the same way\n");
    }

    /* --- counters that should stay at zero ------------------------------- */
    is(badIfAny(0), Grade::Neutral, "no errors is neutral, not green");
    is(badIfAny(1), Grade::Bad, "one error is bad");
    is(badIfAny(9999), Grade::Bad, "many errors is bad");
    is(warnIfAny(0), Grade::Neutral, "nothing to warn about is neutral");
    is(warnIfAny(1), Grade::Warn, "one is a warning");

    /* --- the named thresholds, by value ---------------------------------- */
    /* 16 ms is one frame at 60 Hz and 33 ms is two: the pacing these come
     * from, asserted so a tidy-up cannot quietly round them. */
    checks++;
    if (!(grade::kDispWaitGoodMs == 16.0 && grade::kDispWaitWarnMs == 33.0)) {
        failures++;
        std::printf("  FAIL display wait thresholds are not 16/33 ms\n");
    }
    checks++;
    if (!(grade::kLossGoodPct == 0.3 && grade::kLossWarnPct == 1.0)) {
        failures++;
        std::printf("  FAIL loss thresholds are not 0.3/1.0 %%\n");
    }
    checks++;
    if (!(grade::kRttGoodMs == 30.0 && grade::kRttWarnMs == 80.0)) {
        failures++;
        std::printf("  FAIL rtt thresholds are not 30/80 ms\n");
    }

    /* Each pair must have good STRICTER than warn, in the right direction for
     * a "lower is better" reading. Swapped, every value grades good. */
    checks++;
    if (!(grade::kDispWaitGoodMs < grade::kDispWaitWarnMs &&
          grade::kLossGoodPct    < grade::kLossWarnPct &&
          grade::kRttGoodMs      < grade::kRttWarnMs)) {
        failures++;
        std::printf("  FAIL a threshold pair is the wrong way round\n");
    }

    /* And used as intended: one frame of wait is good, three is bad. */
    is(gradeLo(16, grade::kDispWaitWarnMs, grade::kDispWaitGoodMs), Grade::Good,
       "one frame of display wait is good");
    is(gradeLo(50, grade::kDispWaitWarnMs, grade::kDispWaitGoodMs), Grade::Bad,
       "three frames of display wait is bad");
    is(gradeLo(0.2, grade::kLossWarnPct, grade::kLossGoodPct), Grade::Good,
       "0.2 % loss is good");
    is(gradeLo(2.0, grade::kLossWarnPct, grade::kLossGoodPct), Grade::Bad,
       "2 % loss is bad");

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
