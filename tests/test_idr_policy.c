/* test_idr_policy.c - the two rules that decide when to ask for a key frame.
 *
 * Each check names its COUNTER-CASE: the exact input that produced a black
 * screen for real. A failure here therefore says what has just been undone.
 *
 * Campaign S34 (2026-08-25): over a run of twenty consecutive sessions,
 * eighteen displayed NO picture at all while the incoming bitrate was normal.
 * The two rules below were the cause.
 */
#include "../core/protocol/idr_policy.h"
#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static void rate_rule(void)
{
    /* The nominal case: the very first request always goes through. */
    idr_rate_t r = {0};
    CHECK(idr_rate_allow(&r, 0, 10),
            "the first request must go through, even at tick 0");
    idr_rate_mark(&r, 0);

    /* Inside the interval: no spamming. */
    CHECK(!idr_rate_allow(&r, 5, 10),
            "5 ticks after the last request: too early");
    CHECK(!idr_rate_allow(&r, 9, 10),
            "9 ticks: still too early (the interval is 10)");
    CHECK(idr_rate_allow(&r, 10, 10),
            "exactly 10 ticks: the interval is reached");

    /* COUNTER-CASE S34 - THE BLACK SCREEN BUG.
     * A 24 s session leaves the counter around 500. The next session restarts
     * at 0. The signed version computed 0 - 500 = -500, hence never >= 10: NO
     * key frame request was emitted any more, neither in that session nor in
     * any that followed. The server then never sent SPS/PPS/IDR again
     * (measured: NALs of type 1 exclusively), the decoder's gate did not open,
     * and the screen stayed black. */
    idr_rate_t after = {0};
    idr_rate_mark(&after, 500);            /* the end of the previous session */
    CHECK(idr_rate_allow(&after, 0, 10),
            "COUNTER-CASE: a new session (tick 0) after a session that ended "
            "at tick 500 - the request MUST go through");
    CHECK(idr_rate_allow(&after, 9, 10),
            "CONTRE-CAS : idem quelques ticks plus tard");

    /* The same trap when the 16-bit counter wraps (~55 min of session). */
    idr_rate_t wrapped = {0};
    idr_rate_mark(&wrapped, 65530);
    CHECK(!idr_rate_allow(&wrapped, 65535, 10),
            "wraparound: 5 ticks later, still too early");
    CHECK(idr_rate_allow(&wrapped, 4, 10),
            "COUNTER-CASE: the counter wrapped (65530 -> 4 = 10 ticks) - "
            "the request MUST go through");

    /* The flag tells "never requested" apart from "requested at tick 0". */
    idr_rate_t pristine = {0};
    CHECK(idr_rate_allow(&pristine, 3, 10),
            "never requested: goes through whatever the tick");
}

static void regle_calage(void)
{
    idr_stall_t s = {0};

    /* The decoder is producing: ask for nothing. */
    CHECK(!idr_stall_request(&s, 1, 15), "1 frame returned: nothing to ask for");
    CHECK(!idr_stall_request(&s, 2, 15), "2 frames returned: nothing to ask for");

    /* It goes quiet: ask once the threshold is reached, not before. */
    bool tire = false;
    for (int i = 0; i < 14; i++) tire |= idr_stall_request(&s, 2, 15);
    CHECK(!tire, "14 frames fed with no output: not the threshold yet");
    CHECK(idr_stall_request(&s, 2, 15),
            "15th frame fed with no output: we ask for a key frame");

    /* COUNTER-CASE S34 - THE ONE-SHOT REQUEST.
     * The old condition was `++stalled == 15`, a strict equality: past the
     * threshold the counter climbed to 16, 17, ... and the request was NEVER
     * emitted again. A single request is enough when the decoder has merely
     * lost its reference; it is not enough when it never started - if it is
     * lost, the screen stays black for the whole session. */
    int tirs = 0;
    for (int i = 0; i < 45; i++) if (idr_stall_request(&s, 2, 15)) tirs++;
    CHECK(tirs == 3,
            "COUNTER-CASE: past the threshold the request must REPEAT "
            "(45 feeds = 3 requests), not stop at the first");

    /* A displayed picture re-arms everything. */
    CHECK(!idr_stall_request(&s, 3, 15),
            "the decoder restarts: the stall counter starts over from zero");
    tire = false;
    for (int i = 0; i < 14; i++) tire |= idr_stall_request(&s, 3, 15);
    CHECK(!tire, "after re-arming, another 15 frames are needed to fire");

    /* A zero threshold must neither divide by zero nor fire in a loop. */
    idr_stall_t z = {0};
    CHECK(!idr_stall_request(&z, 0, 0), "a zero threshold: never a request");
}

int main(void)
{
    printf("== key frame request policy ==\n");
    rate_rule();
    regle_calage();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
