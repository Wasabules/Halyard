/* test_latency — the instrument that measures latency, tested offline.
 *
 * WHY THIS TEST EXISTS. A wrong instrument is WORSE than no instrument: it
 * produces numbers you believe. The three mistakes that would make this module
 * dangerous are all silent — none raises an error, a crash, or an odd-looking
 * line in the log:
 *
 *   1. an OPTIMISTIC percentile (lower bound of the bin instead of the upper)
 *      makes the path look better than it is, and that is exactly the direction
 *      of error you must not have on a latency;
 *   2. a naive reset of the report LOSES the samples arriving while it writes —
 *      a counter that loses in silence, the defect family this repo documents
 *      everywhere;
 *   3. the gap to the SERVER stamp taken for a latency: it contains an
 *      arbitrary clock offset, so it would announce "5,000 ms" with as much
 *      confidence as "20 ms".
 *
 * Every check therefore carries its COUNTER-CASE: the input that, if the rule
 * were wrong, would give a plausible result.
 *
 * The test includes the .c to reach the internal state and the bucketing
 * functions: those are the logic, not the API.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* --- Journal stub: we CAPTURE the lines so they can be checked. The report is
 * not only a computation, it is a TEXT: a missing line is a stage you will not
 * know is dead. */
#define JOURNAL_LINES_MAX 64
static char g_lines[JOURNAL_LINES_MAX][512];
static int  g_nlines = 0;

#include "../core/services/journal.h"

void journal_write(journal_severity_t sev, journal_category_t cat,
                    const char *fmt, ...) {
    (void)sev; (void)cat;
    if (g_nlines >= JOURNAL_LINES_MAX) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_lines[g_nlines], sizeof(g_lines[0]), fmt, ap);
    va_end(ap);
    g_nlines++;
}

#include "../core/protocol/latency.c"

static int g_ko = 0, g_n = 0;
#define CHECK(cond, ...) do { g_n++; if (!(cond)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); g_ko = 1; } } while (0)

static void lines_clear(void) { g_nlines = 0; }

static const char *line_containing(const char *needle) {
    for (int i = 0; i < g_nlines; i++)
        if (strstr(g_lines[i], needle)) return g_lines[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 1. The bucketing: monotonic, covering, and PESSIMISTIC.              */
/* ------------------------------------------------------------------ */
static void test_bins(void)
{

    /* Monotonicity. COUNTER-CASE: the five junctions of the piecewise formula
     * (1 ms, 16 ms, 64 ms, 256 ms, 1.024 s) are exactly where an off-by-one
     * files a BIG value into a SMALL bin — which would make the spikes
     * disappear, i.e. the whole point. */
    int prev = -1;
    const uint32_t junctions[] = { 0, 1, 2, 31, 32, 33, 63, 64, 65,
                                   999, 1000, 1023, 1024, 1025,
                                   65535, 65536, 65537,
                                   2097151, 2097152, 4194303, 4194304,
                                   60000000u, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof(junctions)/sizeof(junctions[0]); i++) {
        int c = lat_bin(junctions[i]);
        CHECK(c >= 0 && c < LAT_BINS, "bin out of range for %u us: %d",
              junctions[i], c);
        CHECK(c >= prev, "bin NOT monotonic at %u us: %d after %d",
              junctions[i], c, prev);
        prev = c;
    }

    /* Coverage AND direction of the error. COUNTER-CASE: an upper bound that
     * actually held the LOWER bound would make `p99` smaller than the real
     * value — a latency announced better than it is, the error never to make
     * here. */
    for (uint32_t us = 0; us < 5000000u; us += 37u) {
        int c = lat_bin(us);
        uint32_t up = lat_bin_upper(c);
        CHECK(up >= us, "bin %d: upper bound %u < value %u (OPTIMISTIC percentile)",
              c, up, us);
    }
    /* The last bin has no bound: it must SAY SO, not return a number.
     * COUNTER-CASE: returning 4294967 ms would read as a measurement. */
    CHECK(lat_bin_upper(LAT_BINS - 1) == 0xFFFFFFFFu,
          "the overflow bin must carry the sentinel");
    char txt[16]; lat_ms(txt, sizeof txt, 0xFFFFFFFFu);
    CHECK(strcmp(txt, ">4194") == 0, "the sentinel must print as \">4194\", not \"%s\"", txt);

    /* THE ERROR IS RELATIVE AND BOUNDED. That is the property that makes a
     * percentile usable over four orders of magnitude. COUNTER-CASE: a fixed
     * step wide enough to cover a one-second stall would put "1 ms" and "20 ms"
     * in the same bin, and the whole input stage would become one number. */
    for (uint32_t us = 32; us < 4000000u; us = us + (us >> 5) + 1) {
        uint32_t up = lat_bin_upper(lat_bin(us));
        CHECK(up >= us && (double)(up - us) <= 0.0626 * (double)us,
              "relative error too large at %u us: bound %u", us, up);
    }
}

/* ------------------------------------------------------------------ */
/* 2. The percentiles.                                                  */
/* ------------------------------------------------------------------ */
static void test_percentiles(void)
{
    latency_reset_session();

    /* 99 samples at 1 ms, 1 at 300 ms. An AVERAGE would say 4 ms and hide the
     * spike: precisely the case this module exists to show. */
    for (int i = 0; i < 99; i++) latency_add(LAT_VID_DECODE, 1000);
    latency_add(LAT_VID_DECODE, 300000);

    lat_stage_t *s = &g_lat.stage[LAT_VID_DECODE];
    uint32_t snap[LAT_BINS];
    memcpy(snap, s->bins, sizeof snap);

    CHECK(s->n == 100, "n=%llu expected 100", (unsigned long long)s->n);
    CHECK(s->worst_us == 300000, "worst=%u expected 300000", s->worst_us);

    uint32_t p50 = lat_percentile(s, snap, 50);
    uint32_t p99 = lat_percentile(s, snap, 99);
    /* p50 falls in the 1 ms bin, whose upper bound is 1.1 ms. */
    CHECK(p50 >= 1000 && p50 <= 1064,
          "p50=%u us: the bound of a 1 ms bin must stay within 6.25 %%", p50);
    /* p99 must stay on the 1 ms values: the 100th sample alone is the spike.
     * COUNTER-CASE: a threshold rounded down would tip p99 onto 300 ms and the
     * instrument would cry spike on every isolated value. */
    CHECK(p99 <= 1064, "p99=%u us: a single sample in a hundred must not tip "
                       "the 99th percentile", p99);
    /* And the spike MUST stay readable elsewhere: that is `worst`'s job. */
    CHECK(lat_percentile(s, snap, 100) >= 300000,
          "the 100th percentile must reach the spike");
}

/* ------------------------------------------------------------------ */
/* 3. The report SUBTRACTS its snapshot, it does not reset.             */
/* ------------------------------------------------------------------ */
static void test_subtraction(void)
{
    latency_reset_session();
    lines_clear();

    for (int i = 0; i < 10; i++) latency_add(LAT_IN_SEND, 5000);
    latency_report_final();
    CHECK(g_lat.stage[LAT_IN_SEND].n == 0, "the window must be settled");

    /* COUNTER-CASE for the naive reset: a sample deposited BETWEEN the snapshot
     * and the reset would vanish. We simulate it by depositing after the
     * report: the next window must count exactly 3, not 13. */
    for (int i = 0; i < 3; i++) latency_add(LAT_IN_SEND, 5000);
    CHECK(g_lat.stage[LAT_IN_SEND].n == 3,
          "n=%llu after the report: new samples must start again from zero",
          (unsigned long long)g_lat.stage[LAT_IN_SEND].n);
    CHECK(g_lat.stage[LAT_IN_SEND].sum_us == 15000,
          "sum=%llu: it must track n, otherwise the average drifts",
          (unsigned long long)g_lat.stage[LAT_IN_SEND].sum_us);

    /* The SESSION worst survives the report; the WINDOW one does not. */
    CHECK(g_lat.stage[LAT_IN_SEND].worst_session == 5000,
          "the session worst must never be erased");
}

/* ------------------------------------------------------------------ */
/* 4. A silent stage SAYS SO.                                           */
/* ------------------------------------------------------------------ */
static void test_silent_stage(void)
{
    latency_reset_session();
    lines_clear();
    latency_add(LAT_VID_DECODE, 2000);
    latency_report_final();

    /* COUNTER-CASE: a report that merely omits the empty stages reads as
     * "nothing to report" when it means "nobody writes here". This repo has
     * paid for seven dead counters of that family. */
    const char *l = line_containing("never fed");
    CHECK(l != NULL, "a stage with no sample must be NAMED in the report");
    if (l) {
        CHECK(strstr(l, "audio/file") != NULL,
              "the silent audio stage must be named: %s", l);
        CHECK(strstr(l, "video/decode") == NULL,
              "a FED stage must not appear among the silent ones: %s", l);
    }
    CHECK(line_containing("video/decode") != NULL,
          "the fed stage must have its line");
}

/* ------------------------------------------------------------------ */
/* 5. End to end: it is the VARIATION, not the absolute gap.            */
/* ------------------------------------------------------------------ */
#define VI4_EXT 1
/* ------------------------------------------------------------------ */
/* 4b. VI4 - the panel reads SILENT when the log does.                  */
/* ------------------------------------------------------------------ */
/* VI4 (2026-09-11). The VI4_EXT checks read the two fields VI4 added; the
 * bench ran the same function against the unpatched module, where the
 * counter-cases failed. */
static void test_silent_panel(void)
{
    latency_report_t r, never;
    int rd;

    latency_reset_session();
    lines_clear();

    /* Window 1: decode fed (3.0..3.6 ms), retention fed with 0 us ONLY (a path
     * where no picture ever waits), audio never fed. */
    for (int i = 0; i < 400; i++) latency_add(LAT_VID_DECODE, 3000 + (i % 7) * 100);
    for (int i = 0; i < 400; i++) latency_add(LAT_VID_HOLD, 0);
    latency_report_final();
    rd = latency_read(LAT_VID_DECODE, &r);
    CHECK(rd == 1 && r.n == 400, "a fed window must read as fed: read=%d n=%u", rd, r.n);

    /* Window 2: nobody writes - a 3.34 video outage. COUNTER-CASE: the report
     * names the stage "never fed" while latency_read kept handing the panel
     * window 1's figures, for as long as the outage lasted. The panel exists
     * for exactly that moment (L19). */
    lines_clear();
    latency_report_final();
    const char *l = line_containing("never fed");
    CHECK(l && strstr(l, "video/decode"), "the log must name the silent stage");
    rd = latency_read(LAT_VID_DECODE, &r);
    CHECK(rd == 0, "a stage silent for a whole window must read as silent: "
                   "read=%d n=%u p50=%u us", rd, r.n, r.p50_us);
    CHECK(r.n == 0 && r.avg_us == 0 && r.p50_us == 0 && r.p99_us == 0 && r.worst_us == 0,
          "no figure of the previous window may survive: n=%u p50=%u p99=%u",
          r.n, r.p50_us, r.p99_us);
    CHECK(r.worst_session_us == 3600,
          "the session worst must survive the silence: %u", r.worst_session_us);

    /* COUNTER-CASE for "ever fed" keyed on a VALUE: a stage fed only with 0 us
     * has worst_session == 0, so a rule "worst_session > 0 means it was fed"
     * hides it as if nobody had ever written there. What latency_read returns
     * must tell it apart from a never-fed stage. */
    int rz = latency_read(LAT_VID_HOLD, &r);
    int rn = latency_read(LAT_AUD_QUEUE, &never);
    CHECK(rz == 0, "the zero-fed stage went silent too and must read so: read=%d", rz);
    CHECK(rn == 0 && never.n == 0 && never.worst_session_us == 0,
          "a never-fed stage must read as empty");
    CHECK(memcmp(&r, &never, sizeof r) != 0,
          "a stage fed with 0 us must stay distinguishable from a never-fed one");
#ifdef VI4_EXT
    CHECK(never.n_session == 0, "never fed: n_session=%u", never.n_session);
    CHECK(r.n_session == 400 && r.silent_windows == 1,
          "zero-fed stage: n_session=%u silent_windows=%u", r.n_session, r.silent_windows);
    lines_clear();
    latency_report_final();
    latency_read(LAT_VID_DECODE, &r);
    CHECK(r.silent_windows == 2 && r.n_session == 400 && r.worst_session_us == 3600,
          "second silent window: silent_windows=%u n_session=%u ws=%u",
          r.silent_windows, r.n_session, r.worst_session_us);
    latency_read(LAT_AUD_QUEUE, &never);
    CHECK(never.silent_windows == 0, "a never-fed stage must not count silent windows");
#endif

    /* It comes back at another operating point: the figures must come from the
     * NEW window only. */
    for (int i = 0; i < 100; i++) latency_add(LAT_VID_DECODE, 8000);
    latency_report_final();
    rd = latency_read(LAT_VID_DECODE, &r);
    CHECK(rd == 1 && r.n == 100 && r.p50_us >= 8000 && r.p50_us <= 8500,
          "the returning window must read alone: read=%d n=%u p50=%u", rd, r.n, r.p50_us);
#ifdef VI4_EXT
    CHECK(r.silent_windows == 0 && r.n_session == 500,
          "back: silent_windows=%u n_session=%u", r.silent_windows, r.n_session);
#endif

    /* A new session starts clean: nothing crosses latency_reset_session. */
    latency_reset_session();
    rd = latency_read(LAT_VID_DECODE, &r);
    CHECK(rd == 0 && r.n == 0 && r.worst_session_us == 0, "a new session must read empty");
#ifdef VI4_EXT
    CHECK(r.n_session == 0 && r.silent_windows == 0, "a new session must forget n_session");
#endif
}

static void test_end_to_end(void)
{
    latency_reset_session();
    lines_clear();

    /* A 5 s clock offset between us and the server, constant. The added latency
     * is nil. COUNTER-CASE: publishing the raw gap would announce "5,000 ms of
     * latency" — a plausible number, wrong, and one nobody could contradict
     * without a common clock. */
    const int64_t base_loc = 1000000000LL;   /* 1000 s of local clock */
    const uint32_t offset = 5000000u;        /* 5 s */
    for (int i = 0; i < 50; i++) {
        int64_t t = base_loc + i * 20000LL;              /* one picture every 20 ms */
        latency_video_displayed((uint32_t)t - offset, t);
    }
    CHECK(g_lat.stage[LAT_VID_E2E].n >= 49,
          "n=%llu: the samples must be counted",
          (unsigned long long)g_lat.stage[LAT_VID_E2E].n);
    CHECK(g_lat.stage[LAT_VID_E2E].worst_us < 1000,
          "worst=%u us: at a CONSTANT offset the added latency must be nil, "
          "not equal to the clock offset", g_lat.stage[LAT_VID_E2E].worst_us);

    /* One picture taking 30 ms longer must stand out, and only that one. */
    int64_t t = base_loc + 50 * 20000LL;
    latency_video_displayed((uint32_t)t - offset - 30000u, t);
    CHECK(g_lat.stage[LAT_VID_E2E].worst_us >= 29000
       && g_lat.stage[LAT_VID_E2E].worst_us <= 31000,
          "worst=%u us expected ~30000: this is the ADDED latency",
          g_lat.stage[LAT_VID_E2E].worst_us);

    /* COUNTER-CASE FOUND BY RUNNING THE REPORT (not by re-reading it): if the
     * base is frozen on the FIRST sample and that one happens to be a spike,
     * all the rest of the window falls below it and disappears — the report
     * announces `n=1` over 500 pictures. So the base must DROP as soon as a
     * faster trip shows up. */
    const char *l;
    latency_reset_session();
    latency_video_displayed((uint32_t)base_loc - offset - 40000u, base_loc); /* spike first */
    for (int i = 1; i < 100; i++) {
        int64_t tt = base_loc + i * 20000LL;
        latency_video_displayed((uint32_t)tt - offset, tt);
    }
    CHECK(g_lat.stage[LAT_VID_E2E].n == 100,
          "n=%llu expected 100: no sample must be lost because the first "
          "picture happened to be a spike",
          (unsigned long long)g_lat.stage[LAT_VID_E2E].n);
    CHECK(g_lat.e2e_base_lowered >= 1,
          "lowering the base must be COUNTED, otherwise you cannot know the "
          "base moved under samples already filed");

    /* The DRIFT must read on the window being written, not on the previous one.
     * COUNTER-CASE: computed after the write, it shifts by a whole window and
     * pins a settling delay on the wrong moment of the session. */
    latency_reset_session();
    lines_clear();
    for (int i = 0; i < 20; i++) {
        int64_t tt = base_loc + i * 20000LL;
        latency_video_displayed((uint32_t)tt - offset, tt);
    }
    latency_report_final();                       /* window 1: base 5 s */
    lines_clear();
    for (int i = 0; i < 20; i++) {                /* window 2: 7 ms later */
        int64_t tt = base_loc + 1000000LL + i * 20000LL;
        latency_video_displayed((uint32_t)tt - offset - 7000u, tt);
    }
    latency_report_final();
    l = line_containing("video/bout");
    CHECK(l && strstr(l, "drift=+7.0") != NULL,
          "the drift of the WRITTEN window must be +7.0 ms: %s",
          l ? l : "(line missing)");

    latency_reset_session();
    lines_clear();
    for (int i = 0; i < 50; i++) {
        int64_t tt = base_loc + i * 20000LL;
        latency_video_displayed((uint32_t)tt - offset, tt);
    }
    latency_report_final();
    l = line_containing("video/bout");
    CHECK(l != NULL, "the report must carry the end-to-end line");
    if (l) CHECK(strstr(l, "base=") != NULL,
                 "the line must announce its BASE, otherwise someone will read "
                 "the percentiles as absolute latencies: %s", l);
}

/* ------------------------------------------------------------------ */
/* 6. The 32-bit wrap of the stamp.                                     */
/* ------------------------------------------------------------------ */
static void test_wrap(void)
{
    latency_reset_session();

    /* The server field is a u32 of microseconds: it wraps every ~71.6 min.
     * COUNTER-CASE: a signed 64-bit subtraction would return ~4,295 s once per
     * wrap, i.e. ONE absurd measurement per hour — rare enough to pass for an
     * accident, big enough to crush its window's average. */
    const int64_t t = 0x100000000LL + 4096;      /* local clock AFTER the wrap */
    const uint32_t stamp = 0xFFFFF000u;          /* stamp BEFORE the wrap */
    /* expected gap: (uint32_t)4096 - 0xFFFFF000 = 8192 us */
    latency_video_displayed(stamp, t);            /* sets the base */
    CHECK(g_lat.e2e_base_us == 8192,
          "base=%u us expected 8192: the wrap must cancel out between the two "
          "truncated clocks", g_lat.e2e_base_us);
}

/* ------------------------------------------------------------------ */
/* 7. A negative value is DROPPED, not clamped to zero.                 */
/* ------------------------------------------------------------------ */
static void test_negative(void)
{
    latency_reset_session();
    latency_add(LAT_VID_BURST, -1);
    latency_add(LAT_VID_BURST, -100000);
    /* COUNTER-CASE: clamping to zero would add two perfect samples and pull
     * every percentile down — the path would look better than it is, because of
     * a missing milestone. */
    CHECK(g_lat.stage[LAT_VID_BURST].n == 0,
          "n=%llu: a negative value must produce NO sample at all",
          (unsigned long long)g_lat.stage[LAT_VID_BURST].n);

    /* Cap: a suspended session (console sleep) must not crush the window with a
     * value of several minutes. */
    latency_add(LAT_VID_BURST, 3600LL * 1000000LL);
    CHECK(g_lat.stage[LAT_VID_BURST].worst_us == 60000000u,
          "worst=%u: outliers must be capped at 60 s",
          g_lat.stage[LAT_VID_BURST].worst_us);
}

/* ------------------------------------------------------------------ */
/* 8. The pts -> stamp ring.                                            */
/* ------------------------------------------------------------------ */
static void test_pts_ring(void)
{
    latency_reset_session();

    for (int i = 1; i <= 8; i++)
        latency_video_into_decoder(i * 90, (uint32_t)(i * 1000));
    CHECK(latency_video_stamp_for_pts(8 * 90) == 8000, "last entry lost");
    CHECK(latency_video_stamp_for_pts(1 * 90) == 1000, "the ring must hold 8 entries");

    /* A 9th entry evicts the oldest: that is intended — a picture that has not
     * come out of the decoder within eight pictures never will. */
    latency_video_into_decoder(9 * 90, 9000);
    CHECK(latency_video_stamp_for_pts(1 * 90) == 0,
          "the oldest must have been evicted");

    /* COUNTER-CASE for the matching: the server produces TWO pictures with the
     * same timestamp (G40). The ring must return the NEWEST, which is the one
     * we display — returning the old one would overestimate the latency by a
     * picture. */
    latency_video_into_decoder(9 * 90, 12345);
    CHECK(latency_video_stamp_for_pts(9 * 90) == 12345,
          "at equal pts, the ring must return the most recent entry");

    /* An unknown pts returns 0, and the caller drops the sample rather than
     * manufacturing one from a null stamp. */
    CHECK(latency_video_stamp_for_pts(777777) == 0, "an unknown pts must return 0");
    latency_video_displayed(0, 123456789);
    CHECK(g_lat.stage[LAT_VID_E2E].n == 0,
          "a missing stamp must produce no sample");
}

/* ------------------------------------------------------------------ */
/* 9. SHADOW_LATENCE=0 restores the uninstrumented path.                */
/*    In a child process: the toggle is cached on the first call, like   */
/*    every toggle in this repo.                                        */
/* ------------------------------------------------------------------ */
static int test_toggle_off(void)
{
    setenv("SHADOW_LATENCE", "0", 1);
    if (latency_enabled() != 0) { printf("  FAIL: SHADOW_LATENCE=0 must switch it off\n"); return 1; }
    latency_reset_session();
    latency_add(LAT_VID_DECODE, 5000);
    latency_video_into_decoder(90, 4242);
    latency_video_displayed(1000, 2000);
    lines_clear();
    latency_report_final();
    if (g_lat.stage[LAT_VID_DECODE].n != 0) {
        printf("  FAIL: with the toggle at 0 nothing must be accumulated\n"); return 1;
    }
    if (latency_video_stamp_for_pts(90) != 0) {
        printf("  FAIL: with the toggle at 0 nothing must land in the ring\n"); return 1;
    }
    if (g_nlines != 0) {
        printf("  FAIL: with the toggle at 0 nothing must be written to the log\n"); return 1;
    }
    return 0;
}

int main(void)
{
    printf("== latency: the path's measuring instrument (stages, percentiles) ==\n");
    fflush(stdout);   /* otherwise the header is duplicated by the forked child */

    /* The toggle is cached on the first call: we exercise it in a child, before
     * the parent has read it. */
    pid_t pid = fork();
    if (pid == 0) { int r = test_toggle_off(); fflush(stdout); _exit(r); }
    if (pid > 0) {
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) g_ko = 1;
    } else {
        printf("  (fork unavailable: toggle not exercised)\n");
    }

    unsetenv("SHADOW_LATENCE");
    test_bins();
    test_percentiles();
    test_subtraction();
    test_silent_stage();
    test_silent_panel();
    test_end_to_end();
    test_wrap();
    test_negative();
    test_pts_ring();

    printf("%d checks, %d failure(s)\n", g_n, g_ko);
    return g_ko;
}
