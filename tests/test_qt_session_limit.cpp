/* test_qt_session_limit - the end-of-session warnings (LIM1).
 *
 * Every edge here is "did this fire exactly once", which is not something to
 * check by sitting in front of a stream for five hours and forty-five
 * minutes. The suite walks a whole six-hour session second by second and
 * counts.
 */
#include <cstdio>

#include "../clients/qt/session_limit.hpp"

using namespace halyard;

static int checks = 0, failures = 0;

static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond) { failures++; std::printf("  FAIL %s\n", what); }
}

static void eqi(long got, long want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL %-46s got %ld, expected %ld\n", what, got, want);
    }
}

int main()
{
    std::printf("== session limit warnings (LIM1 2026-10-03) ==\n");

    const int kCeiling = 6 * 3600;   /* the measured account's 21600 s */

    /* --- a whole session, one second at a time --------------------------- */
    {
        int state = 0, fired = 0;
        int firedAt[8] = {0};
        for (int t = 0; t <= kCeiling + 120; t++) {
            const LimitWarning w = sessionLimitCheck(t, kCeiling, &state);
            if (w.fired) {
                if (fired < 8) firedAt[fired] = w.minutes;
                fired++;
            }
        }
        /* Four thresholds plus the end. */
        eqi(fired, 5, "five warnings over a whole session");
        eqi(firedAt[0], 30, "the first is 30 minutes");
        eqi(firedAt[1], 15, "then 15");
        eqi(firedAt[2], 5,  "then 5");
        eqi(firedAt[3], 1,  "then 1");
        eqi(firedAt[4], 0,  "then time is up");
    }

    /* --- nothing fires early -------------------------------------------- */
    {
        int state = 0;
        for (int t = 0; t < kCeiling - 30 * 60; t++) {
            const LimitWarning w = sessionLimitCheck(t, kCeiling, &state);
            if (w.fired) { failures++; checks++; std::printf("  FAIL fired at t=%d\n", t); break; }
        }
        checks++;
        ok(state == 0, "nothing is announced before the first threshold");
    }

    /* --- the level each one carries -------------------------------------- */
    {
        int state = 0;
        LimitWarning w = sessionLimitCheck(kCeiling - 30 * 60, kCeiling, &state);
        ok(w.fired && w.level == LimitLevel::Notice, "30 min is a notice");
        w = sessionLimitCheck(kCeiling - 15 * 60, kCeiling, &state);
        ok(w.fired && w.level == LimitLevel::Warn, "15 min is a warning");
        w = sessionLimitCheck(kCeiling - 5 * 60, kCeiling, &state);
        ok(w.fired && w.level == LimitLevel::Warn, "5 min is a warning");
        w = sessionLimitCheck(kCeiling - 60, kCeiling, &state);
        ok(w.fired && w.level == LimitLevel::Urgent, "1 min is urgent");
        w = sessionLimitCheck(kCeiling + 1, kCeiling, &state);
        ok(w.fired && w.level == LimitLevel::Urgent && w.minutes == 0,
           "past the end is urgent with no minutes");
    }

    /* --- the level PERSISTS between ticks -------------------------------- *
     *
     * A banner has to stay up, so a call that fires nothing must still
     * report where we are. Only `fired` separates "show this now" from
     * "keep showing this". */
    {
        int state = 0;
        LimitWarning w = sessionLimitCheck(kCeiling - 5 * 60, kCeiling, &state);
        ok(w.fired, "the 5-minute warning fires");
        w = sessionLimitCheck(kCeiling - 5 * 60 + 1, kCeiling, &state);
        ok(!w.fired, "and does not fire again a second later");
        ok(w.level == LimitLevel::Warn, "but the level is still reported");
        eqi(w.minutes, 5, "with the threshold it is at");
    }

    /* --- waking up late -------------------------------------------------- *
     *
     * A laptop lid, a suspended VM: the client can be away across several
     * thresholds. It must announce the MOST URGENT one reached, once - not
     * replay thirty, fifteen and five in a burst. */
    {
        int state = 0;
        const LimitWarning w = sessionLimitCheck(kCeiling - 4 * 60, kCeiling, &state);
        ok(w.fired, "coming back with four minutes left says something");
        eqi(w.minutes, 5, "and it is the 5-minute warning, not the 30");
        const LimitWarning w2 = sessionLimitCheck(kCeiling - 3 * 60, kCeiling, &state);
        ok(!w2.fired, "the ones it slept through are not replayed");
    }
    {
        /* Away past the end entirely. */
        int state = 0;
        const LimitWarning w = sessionLimitCheck(kCeiling + 600, kCeiling, &state);
        ok(w.fired && w.minutes == 0, "waking up after the end says time is up");
    }

    /* --- no ceiling, no warnings ----------------------------------------- *
     *
     * The server does not always say. A countdown invented from a default is
     * the exact mistake the console client refused to make. */
    {
        int state = 0;
        for (int t = 0; t < 100000; t += 997) {
            const LimitWarning w = sessionLimitCheck(t, 0, &state);
            if (w.fired || w.level != LimitLevel::None) {
                failures++; checks++;
                std::printf("  FAIL fired with no ceiling at t=%d\n", t);
                break;
            }
        }
        checks++;
        ok(state == 0, "no ceiling means nothing is ever announced");
        const LimitWarning w = sessionLimitCheck(10, -1, &state);
        ok(w.level == LimitLevel::None, "a negative ceiling is the same");
    }

    /* A null state must not crash. */
    {
        const LimitWarning w = sessionLimitCheck(10, kCeiling, nullptr);
        ok(w.level == LimitLevel::None, "a null state is refused, not written");
    }

    /* --- a reconnection starts over -------------------------------------- */
    {
        int state = 0;
        (void)sessionLimitCheck(kCeiling - 60, kCeiling, &state);
        ok(state > 0, "warnings were announced in the first session");
        state = 0;   /* what the caller does on a new session */
        const LimitWarning w = sessionLimitCheck(30, kCeiling, &state);
        ok(!w.fired && w.level == LimitLevel::None,
           "zeroing the state makes a new session quiet again");
    }

    /* --- a SHORT ceiling, where thresholds overlap the start ------------- *
     *
     * A 10-minute ceiling is past the 30- and 15-minute marks before the
     * session begins. It must not fire three warnings at t=0; it must say
     * the one that is true. */
    {
        int state = 0;
        const LimitWarning w = sessionLimitCheck(0, 10 * 60, &state);
        ok(w.fired, "a short session warns immediately");
        eqi(w.minutes, 15, "with the most urgent threshold already passed");
        int fired = 1;
        for (int t = 1; t <= 10 * 60 + 5; t++)
            if (sessionLimitCheck(t, 10 * 60, &state).fired) fired++;
        /* 15 at t=0, then 5, then 1, then the end: four, not three. The
         * first version of this expectation forgot the one-minute mark -
         * the test was wrong and the code was right, which is the outcome
         * worth having from a test like this. */
        eqi(fired, 4, "then 5, then 1, then the end - and nothing repeated");
    }

    /* --- fair use, against the SERVER's own threshold --------------------- */
    ok(!fairUseAlert(100, 1000, 0.8), "10 % of the allowance is not an alert");
    ok(fairUseAlert(800, 1000, 0.8),  "exactly the threshold IS an alert");
    ok(fairUseAlert(950, 1000, 0.8),  "past it too");
    ok(!fairUseAlert(950, 0, 0.8),    "no allowance, no alert");
    ok(!fairUseAlert(950, 1000, 0.0), "no threshold, no alert");
    ok(fairUseAlert(950, 1000, 0.9),  "the server's number is the one used");
    ok(!fairUseAlert(950, 1000, 0.99), "and a stricter one is respected");

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
