/* test_freeze_stat.c - the G44 micro-freeze detector (freeze_stat.h).
 *
 * Each COUNTER-CASE is an input the previous code (function statics in
 * ctrl_session_glue.c::on_frame) got wrong, silently, in a real process that
 * runs several sessions. During the HO-2 bench the same checks ran against a
 * verbatim copy of that code, and every COUNTER-CASE failed there - which is
 * what proves each one guards something.
 *
 * Campaign HO-2 (2026-09-11). Baseline log of 2026-09-10: 25 `[G44] micro-gel`
 * lines, the last 11.6 s after the first picture, all 45-72 ms; then 37 more
 * freezes counted and never printed, among them the 76 and 86 ms ones.
 */
#include "../core/protocol/freeze_stat.h"
#define PROCESS_START() ((void)0)   /* the bench's legacy arm restarted the process here */
#include <stdio.h>

static int checks = 0, failures = 0, cc_checks = 0, cc_failures = 0;
#define CHECK_(cc, cond, what) do {                                          \
    checks++; if (cc) cc_checks++;                                           \
    if (!(cond)) { failures++; if (cc) cc_failures++;                        \
        printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); }         \
} while (0)
#define CHECK(cond, what)        CHECK_(0, cond, what)
#define COUNTER_CASE(cond, what) CHECK_(1, cond, what)

#define T0 100000   /* a monotonic ms is never 0: keep the legacy sentinel honest */
#define LOG FREEZE_STAT_LOG_MS

/* Feed `n` pictures `step` ms apart; returns how many LOG_GAP came back. */
static int feed_run(freeze_stat_t *s, int64_t *t, int n, int step)
{
    int lines = 0;
    for (int i = 0; i < n; i++) {
        *t += step;
        if (freeze_stat_feed(s, *t, LOG) & FREEZE_STAT_LOG_GAP) lines++;
    }
    return lines;
}

/* Nominal behaviour: what must hold in a single fresh session. */
static void nominal(void)
{
    PROCESS_START();
    freeze_stat_t s; freeze_stat_reset(&s);
    int64_t t = T0;
    CHECK(freeze_stat_feed(&s, t, LOG) == FREEZE_STAT_NONE,
          "the first picture of a session returns nothing");
    CHECK(s.nfr == 0 && s.dt_ms == 0, "the first picture measures no interval");
    CHECK(freeze_stat_mean_ms(&s) == 0, "mean before any interval is 0, not a division by 0");

    t += 44; freeze_stat_feed(&s, t, LOG);
    CHECK(s.nfreeze == 0, "44 ms is not a freeze");
    t += 45; freeze_stat_feed(&s, t, LOG);
    CHECK(s.nfreeze == 1 && s.dt_ms == 45, "45 ms is a freeze, and dt_ms reports it");
    t += 100;
    CHECK(freeze_stat_feed(&s, t, LOG) & FREEZE_STAT_LOG_GAP,
          "a 100 ms gap is worth a line at the default threshold");
    CHECK(s.dt_ms == 100, "dt_ms carries the interval the caller prints");

    /* Bucket edges, D4's layout. */
    CHECK(freeze_stat_bucket(99)   == FREEZE_H_45_99,    "99 ms -> 45-99");
    CHECK(freeze_stat_bucket(100)  == FREEZE_H_100_299,  "100 ms -> 100-299");
    CHECK(freeze_stat_bucket(299)  == FREEZE_H_100_299,  "299 ms -> 100-299");
    CHECK(freeze_stat_bucket(300)  == FREEZE_H_300_999,  "300 ms -> 300-999");
    CHECK(freeze_stat_bucket(999)  == FREEZE_H_300_999,  "999 ms -> 300-999");
    CHECK(freeze_stat_bucket(1000) == FREEZE_H_1000_UP,  "1000 ms -> >=1s");

    /* A threshold below 45 ms is raised to 45: a normal interval never logs. */
    freeze_stat_t z; freeze_stat_reset(&z);
    PROCESS_START();
    int64_t tz = T0; freeze_stat_feed(&z, tz, 0);
    tz += 30;
    CHECK(!(freeze_stat_feed(&z, tz, 0) & FREEZE_STAT_LOG_GAP),
          "log_min_ms=0: a 30 ms interval is still not a freeze line");
}

/* The bilan bilan_cadence and a picture that is both a gap and a bilan. */
static void bilan_cadence(void)
{
    PROCESS_START();
    freeze_stat_t s; freeze_stat_reset(&s);
    int64_t t = T0; freeze_stat_feed(&s, t, LOG);
    int bilans = 0;
    for (int i = 1; i <= 499; i++) { t += 20; if (freeze_stat_feed(&s, t, LOG) & FREEZE_STAT_BILAN) bilans++; }
    CHECK(bilans == 0, "no bilan before 500 intervals");
    t += 150;
    unsigned ev = freeze_stat_feed(&s, t, LOG);
    CHECK((ev & FREEZE_STAT_BILAN) && (ev & FREEZE_STAT_LOG_GAP),
          "the 500th interval is a 150 ms gap: BOTH flags, the caller prints both lines");
    CHECK(s.nfr == 500 && s.max_ms == 150, "the bilan sees 500 intervals and the gap");
}

/* (a) COUNTER-CASE - two sessions 35 s apart. */
static void two_sessions_pause(void)
{
    PROCESS_START();
    freeze_stat_t s; freeze_stat_reset(&s);
    int64_t t = T0; freeze_stat_feed(&s, t, LOG);
    feed_run(&s, &t, 150, 20);
    t += 80; freeze_stat_feed(&s, t, LOG);
    feed_run(&s, &t, 49, 20);

    freeze_stat_reset(&s);          /* the glue's memset at session start */
    t += 35000;                     /* pause + reconnect */
    unsigned ev = freeze_stat_feed(&s, t, LOG);
    COUNTER_CASE(ev == FREEZE_STAT_NONE,
        "COUNTER-CASE (a): session 2's first picture must return nothing - the "
        "35 s pause is not a freeze line");
    COUNTER_CASE(s.nfr == 0 && s.dt_ms == 0,
        "COUNTER-CASE (a): session 2's first picture measures no interval");
    feed_run(&s, &t, 49, 20);
    t += 90; freeze_stat_feed(&s, t, LOG);
    feed_run(&s, &t, 50, 20);
    COUNTER_CASE(s.max_ms == 90,
        "COUNTER-CASE (a): session 2's max is its own largest gap (90 ms), not the pause");
    COUNTER_CASE(s.nfr == 100 && freeze_stat_mean_ms(&s) == 20,
        "COUNTER-CASE (a): session 2's mean is its own (99x20 + 90) / 100 = 20 ms");
    COUNTER_CASE(s.nfreeze == 1,
        "COUNTER-CASE (a): session 2 counts one freeze, not the pause as a second");
}

/* (b) COUNTER-CASE - thirty one-frame gaps, then a real freeze. */
static void budget_spent_by_small_gaps(void)
{
    PROCESS_START();
    freeze_stat_t s; freeze_stat_reset(&s);
    int64_t t = T0; freeze_stat_feed(&s, t, LOG);
    static const int small[30] = { 61, 68, 62, 46, 48, 52, 49, 48, 45, 49,
                                   48, 69, 46, 45, 52, 49, 51, 57, 72, 61,
                                   60, 68, 65, 51, 45, 76, 86, 50, 47, 55 };
    int lines_small = 0;
    for (int i = 0; i < 30; i++) {
        feed_run(&s, &t, 5, 20);
        t += small[i];
        if (freeze_stat_feed(&s, t, LOG) & FREEZE_STAT_LOG_GAP) lines_small++;
    }
    feed_run(&s, &t, 5, 20);
    t += 300;
    unsigned ev = freeze_stat_feed(&s, t, LOG);
    COUNTER_CASE(ev & FREEZE_STAT_LOG_GAP,
        "COUNTER-CASE (b): after thirty 45-86 ms gaps (the baseline's own values), "
        "a 300 ms freeze MUST get its line");
    CHECK(s.nfreeze == 31, "the thirty small gaps are still counted as freezes");
    COUNTER_CASE(s.hist[FREEZE_H_45_99] == 30 && s.hist[FREEZE_H_300_999] == 1,
        "COUNTER-CASE (b): the unprinted one-frame gaps stay visible in the "
        "45-99 bucket, the freeze in 300-999");
    /* Changed on purpose, not a bug of the old code: the old threshold printed
     * every 45 ms gap. Listed separately so a legacy run does not blur it with
     * the counter-cases. */
    CHECK(lines_small == 0,
          "DESIGN: one-frame gaps (45-86 ms) are counted, not printed");
}

/* (c) COUNTER-CASE - the histogram outlives the line budget. */
static void histogram_outlives_budget(void)
{
    PROCESS_START();
    freeze_stat_t s; freeze_stat_reset(&s);
    int64_t t = T0; freeze_stat_feed(&s, t, LOG);
    int lines = 0;
    for (int i = 0; i < 40; i++) {
        feed_run(&s, &t, 10, 20);
        t += 300 + 30 * i;                         /* 300 .. 1470 ms */
        if (freeze_stat_feed(&s, t, LOG) & FREEZE_STAT_LOG_GAP) lines++;
    }
    CHECK(lines == FREEZE_STAT_LOG_BUDGET, "exactly 25 lines per session, then quiet");
    COUNTER_CASE(s.hist[FREEZE_H_300_999] + s.hist[FREEZE_H_1000_UP] == 40,
        "COUNTER-CASE (c): all forty >=300 ms gaps are counted after the budget ran out");
    COUNTER_CASE(s.hist[FREEZE_H_1000_UP] == 16,
        "COUNTER-CASE (c): 1000-1470 ms gaps (16 of them) land in the >=1s bucket");
}

/* (d) COUNTER-CASE - the bilan bilan_cadence restarts per session. */
static void bilan_per_session(void)
{
    PROCESS_START();
    freeze_stat_t s; freeze_stat_reset(&s);
    int64_t t = T0; freeze_stat_feed(&s, t, LOG);
    feed_run(&s, &t, 300, 20);                     /* session 1: 300 intervals */

    freeze_stat_reset(&s);
    t += 35000;
    freeze_stat_feed(&s, t, LOG);
    int first_bilan_at = -1;
    uint32_t b_nfr = 0; int64_t b_moy = -1, b_max = -1;
    for (int i = 1; i <= 600; i++) {
        t += 25;
        if ((freeze_stat_feed(&s, t, LOG) & FREEZE_STAT_BILAN) && first_bilan_at < 0) {
            first_bilan_at = i;
            b_nfr = s.nfr; b_moy = freeze_stat_mean_ms(&s); b_max = s.max_ms;
        }
    }
    COUNTER_CASE(b_nfr == 500 && b_moy == 25 && b_max == 25,
        "COUNTER-CASE (d): session 2's first bilan reports ITS 500 intervals, "
        "avg=25 max=25");
    COUNTER_CASE(first_bilan_at == 500,
        "COUNTER-CASE (d): session 2's first bilan comes at its 500th interval, "
        "not at the 200th (process-wide count 300 + pause + 199)");
}

/* (e) COUNTER-CASE - the line budget is per session. */
static void budget_per_session(void)
{
    PROCESS_START();
    freeze_stat_t s; freeze_stat_reset(&s);
    int64_t t = T0; freeze_stat_feed(&s, t, LOG);
    for (int i = 0; i < 30; i++) { feed_run(&s, &t, 5, 20); t += 300; freeze_stat_feed(&s, t, LOG); }
    CHECK(s.logged == FREEZE_STAT_LOG_BUDGET, "session 1 spent its whole budget");

    freeze_stat_reset(&s);
    t += 35000; freeze_stat_feed(&s, t, LOG);
    feed_run(&s, &t, 100, 20);
    t += 300;
    COUNTER_CASE(freeze_stat_feed(&s, t, LOG) & FREEZE_STAT_LOG_GAP,
        "COUNTER-CASE (e): session 2's first 300 ms freeze gets its line although "
        "session 1 spent all 25");
}

/* Guard: a clock that goes backwards. */
static void clock_backwards(void)
{
    PROCESS_START();
    freeze_stat_t s; freeze_stat_reset(&s);
    int64_t t = T0; freeze_stat_feed(&s, t, LOG);
    t += 20; freeze_stat_feed(&s, t, LOG);
    int64_t sum_before = s.sum_ms;
    t -= 500; freeze_stat_feed(&s, t, LOG);
    CHECK(s.sum_ms >= sum_before && s.nfreeze == 0,
          "GUARD: a clock stepping back 500 ms neither lowers the sum nor makes a freeze");
    t += 20; freeze_stat_feed(&s, t, LOG);
    CHECK(s.max_ms == 20, "GUARD: and the next interval is measured from the new mark");
}

int main(void)
{
#ifdef FREEZE_STAT_LEGACY
    printf("== G44 micro-freeze detector - LEGACY arm (function statics) ==\n");
#else
    printf("== G44 micro-freeze detector (freeze_stat.h) ==\n");
#endif
    nominal();
    bilan_cadence();
    two_sessions_pause();
    budget_spent_by_small_gaps();
    histogram_outlives_budget();
    bilan_per_session();
    budget_per_session();
    clock_backwards();
    printf("%d checks, %d failure(s) | counter-cases: %d, %d failed\n",
           checks, failures, cc_checks, cc_failures);
    return failures ? 1 : 0;
}
