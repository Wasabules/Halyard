/* latency — accumulation and reporting. See latency.h for the WHY. */

#include "latency.h"

#include "../services/journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* VIDEO category: this is the path this module says the most about, and the
 * category mask must be able to isolate these lines in one go. */
#define latlog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)

/* === RELATIVE-STEP HISTOGRAM (octaves + mantissa) ===
 *
 * A FIXED-step histogram does not fit here: the measured stages span four
 * orders of magnitude (the texture upload counts in tens of microseconds, a
 * video stall in seconds). A step fine enough for the first would need
 * thousands of bins; a step wide enough for the second would make the first
 * unreadable.
 *
 * So we split by OCTAVE with a 4-bit mantissa: 16 sub-bins per doubling, i.e. a
 * bin width always below 6.25 % of the value. The error is therefore RELATIVE
 * and bounded everywhere — 1 ms reads as "at most 1.02 ms", 100 ms as "at most
 * 104 ms" — instead of being huge at the bottom and nil at the top.
 *
 *   values < 32 us : 16 linear bins of 2 us (below that bound, relative
 *                    precision stops meaning anything: it is the cost of the
 *                    clock call itself);
 *   values >= 32 us: bin = 16 + (e - 5) * 16 + mantissa, where `e` is the
 *                    exponent of the value and the mantissa its 4 top bits
 *                    below the implicit bit.
 *
 * A percentile read here is the UPPER BOUND of its bin: always pessimistic,
 * never optimistic. That is the right direction of error for a latency — a
 * measurement that is wrong must announce WORSE than reality, otherwise it
 * reassures you falsely and you stop looking.
 *
 * Memory cost: 289 bins of 4 bytes per stage, i.e. ~10 KB for the nine stages.
 * Irrelevant, even on console. */
#define LAT_MANT      4                    /* mantissa bits */
#define LAT_SUB       (1 << LAT_MANT)      /* 16 sub-bins per octave */
#define LAT_EXP_MIN   5                    /* first octave: 32 us */
#define LAT_EXP_MAX   21                   /* last octave: 2.097 s .. 4.194 s */
#define LAT_BINS      (LAT_SUB + (LAT_EXP_MAX - LAT_EXP_MIN + 1) * LAT_SUB + 1)

static int lat_bin(uint32_t us) {
    if (us < (1u << LAT_EXP_MIN)) return (int)(us >> (LAT_EXP_MIN - LAT_MANT));
    int e = 31 - __builtin_clz(us);
    if (e > LAT_EXP_MAX) return LAT_BINS - 1;           /* overflow */
    int m = (int)((us >> (e - LAT_MANT)) & (uint32_t)(LAT_SUB - 1));
    return LAT_SUB + (e - LAT_EXP_MIN) * LAT_SUB + m;
}

/* Upper bound of the bin, in microseconds. The overflow bin has none: we return
 * a sentinel that the formatting spells out in words, rather than a number that
 * would read as a measurement. */
static uint32_t lat_bin_upper(int c) {
    if (c < LAT_SUB) return (uint32_t)(c + 1) << (LAT_EXP_MIN - LAT_MANT);
    if (c >= LAT_BINS - 1) return 0xFFFFFFFFu;
    int c2 = c - LAT_SUB;
    int e  = LAT_EXP_MIN + c2 / LAT_SUB;
    int m  = c2 % LAT_SUB;
    return (uint32_t)(LAT_SUB + m + 1) << (e - LAT_MANT);
}

typedef struct {
    uint32_t bins[LAT_BINS];
    uint64_t n;
    uint64_t sum_us;
    uint32_t worst_us;      /* reset on every report: this is the worst OF THE WINDOW */
    uint32_t worst_session;  /* never reset: the worst of the whole session */
} lat_stage_t;

/* === MODULE STATE ===
 *
 * Gathered in ONE file-scope structure, zeroed by `latency_reset_session()`.
 * No function-level `static` carries session state here: that is this repo's
 * costliest defect family (black screen from the 3rd session on, sound audible
 * exactly once), and it would be particularly absurd to re-create it inside the
 * instrument meant to watch for it.
 *
 * The only two function-level `static`s tolerated in this file are environment
 * toggle caches (`SHADOW_LATENCE`), which carry no session state and do not
 * change from one session to the next. */
typedef struct {
    lat_stage_t stage[LAT_NB];

    /* Carrier for the server stamp, from reassembly to display. */
    uint32_t current_stamp;         /* published by flush_display_buffer */
    int64_t  asm_now_us;
    struct { int64_t pts; uint32_t stamp; } ring[8];
    unsigned ring_head;

    /* End to end: the raw gap `t_local - t_server` carries an arbitrary clock
     * offset. Only its variation means anything. */
    uint32_t e2e_base_us;       /* current zero = min(previous window, window so far) */
    int      e2e_base_valid;
    uint32_t e2e_win_min;       /* smallest gap seen in the window in progress */
    int      e2e_win_min_valid;
    uint32_t e2e_prev_min;      /* the previous window's, for the drift */
    int      e2e_prev_valid;
    uint32_t e2e_base_lowered;  /* how many times the base had to drop in the window */
    int64_t  e2e_drift_us;      /* variation of the minimum from one window to the next */

    /* Verification of the server stamp's unit, measured rather than assumed.
     * We accumulate both ends of the window and publish their ratio. */
    uint32_t clk_srv_first, clk_srv_last;
    int64_t  clk_loc_first, clk_loc_last;
    int      clk_primed;

    int64_t  next_report_ms;
    uint32_t reports;

    /* L19 — each stage's last COMPLETE report, for the UI. */
    latency_report_t last[LAT_NB];
} lat_state_t;

static lat_state_t g_lat;

/* --- Toggle -------------------------------------------------------------- */

int latency_enabled(void) {
    /* Toggle cache, not session state: see the comment above. `SHADOW_LATENCE=0`
     * restores the UNINSTRUMENTED application. */
    static int g_on = -1;
    if (g_on < 0) {
        const char *e = getenv("SHADOW_LATENCE");
        g_on = e ? atoi(e) : 1;
    }
    return g_on;
}

static int64_t lat_period_ms(void) {
    static int64_t g_period = -1;
    if (g_period < 0) {
        const char *e = getenv("SHADOW_LATENCE_MS");
        int64_t v = e ? (int64_t)atoi(e) : 10000;
        /* Lower bound: a report every 200 ms would drown the log and cost more
         * than what it measures. */
        g_period = v < 200 ? 200 : v;
    }
    return g_period;
}

int64_t latency_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* NOT TRANSLATED, DELIBERATELY: these strings are a data interface. They are
 * parsed by `tools/campaign_latency.sh` and appear in every log already
 * captured — renaming them would silently break every comparison against past
 * measurements. Same rule as the settings-file keys. */
static const char *k_names[] = {
    "video/rafale",
    "video/retention",
    "video/file-dec",
    "video/decode",
    "video/file-aff",
    "video/televerse",
    "video/cadence",
    "video/bout",
    "input/send",
    "input/gamepad",
    "input/sample",
    "audio/file",
    "reception/tour",   /* ING-1 2026-09-11 - LAT_RX_PASS, one receive-loop pass;
                           same data-interface rule as the names above */
};
/* VI4 - one name per stage, checked by the compiler: a stage added to the enum
 * without its name here would read past the array. */
_Static_assert(sizeof k_names / sizeof *k_names == LAT_NB, "k_names must name every latency stage");

const char *latency_stage_name(latency_stage_t e) {
    /* One unsigned comparison covers both ends. The `< 0` half is
     * unreachable on an unsigned enum -- and whether the enum IS unsigned
     * is the ABI's choice, which is why this warned on one console's
     * toolchain and not the other's. */
        return ((unsigned)e < (unsigned)LAT_NB) ? k_names[e] : "?";
}

void latency_add(latency_stage_t e, int64_t us) {
    if (!latency_enabled()) return;
    if ((unsigned)e >= (unsigned)LAT_NB) return;
    /* A negative value means one of the two milestones was missing, or that a
     * clock went backwards. We drop it rather than clamp it to zero: a
     * manufactured zero pulls the percentiles down and would make the path look
     * better than it is, which is exactly the mistake not to make here. */
    if (us < 0) return;
    /* Capped at 60 s: beyond that it is no longer a latency but a suspended
     * session (console sleep, a breakpoint). Such a sample would crush the
     * average of the whole window. */
    if (us > 60000000LL) us = 60000000LL;
    lat_stage_t *s = &g_lat.stage[e];
    s->bins[lat_bin((uint32_t)us)]++;
    s->n++;
    s->sum_us += (uint64_t)us;
    if ((uint32_t)us > s->worst_us)      s->worst_us = (uint32_t)us;
    if ((uint32_t)us > s->worst_session) s->worst_session = (uint32_t)us;
}

/* --- Stamp carrier -------------------------------------------------------- */

void latency_video_assembled(uint32_t server_stamp, int64_t t_asm_us) {
    g_lat.current_stamp = server_stamp;
    g_lat.asm_now_us    = t_asm_us;
}

uint32_t latency_video_current_stamp(void) { return g_lat.current_stamp; }
int64_t  latency_video_asm_now_us(void)    { return g_lat.asm_now_us; }

void latency_video_into_decoder(int64_t pts, uint32_t server_stamp) {
    if (!latency_enabled()) return;
    unsigned i = g_lat.ring_head & 7u;
    g_lat.ring[i].pts   = pts;
    g_lat.ring[i].stamp = server_stamp;
    g_lat.ring_head = (g_lat.ring_head + 1u) & 7u;
}

uint32_t latency_video_stamp_for_pts(int64_t pts) {
    /* Newest to oldest: two pictures sharing a `pts` do exist (the server
     * produces them, see G40), and it is the newest one we display. */
    for (unsigned k = 1; k <= 8; k++) {
        unsigned i = (g_lat.ring_head + 8u - k) & 7u;
        if (g_lat.ring[i].stamp != 0 && g_lat.ring[i].pts == pts)
            return g_lat.ring[i].stamp;
    }
    return 0;
}

void latency_video_displayed(uint32_t server_stamp, int64_t t_disp_us) {
    if (!latency_enabled()) return;
    if (server_stamp == 0) return;   /* no carrier: nothing to say */

    /* Unsigned 32-bit arithmetic: the server field wraps every ~71.6 min and so
     * does the truncated local clock, at the same bounds. The subtraction stays
     * correct as long as the real gap is far below that period — it is a few
     * tens of milliseconds. */
    uint32_t gap = (uint32_t)t_disp_us - server_stamp;

    if (!g_lat.e2e_win_min_valid || gap < g_lat.e2e_win_min) {
        g_lat.e2e_win_min       = gap;
        g_lat.e2e_win_min_valid = 1;
    }

    /* === THE BASE IS A RUNNING MINIMUM, NOT THE FIRST SAMPLE ===
     *
     * First version of this module: a window's base was the minimum of the
     * PREVIOUS one, and the very first window took its first sample as zero.
     * Counter-case found by running the report over realistic values: if that
     * first picture happens to be a spike, the base starts 40 ms too high, the
     * following 499 pictures ALL fall below the base and are discarded — the
     * report announces `n=1`. A whole window of measurement lost, and the only
     * thing betraying it was a counter at the end of the line.
     *
     * So the base drops as soon as a faster trip shows up. It is bounded by the
     * PREVIOUS window's minimum, never by the whole history: otherwise the
     * relative drift of the two crystals (a few ppm) would accumulate
     * indefinitely and inflate every measurement as the session goes on. A 10 s
     * forgetting window bounds that error to ~0.1 ms.
     *
     * Samples already filed against a higher base stay OVERESTIMATED; this is
     * confined to the start of the first window, where the minimum converges
     * within a few pictures, and it errs on the pessimistic side.
     * `base_abaissee` says how many times it happened: a number that stays large
     * on every window means the minimum is not converging, and therefore that
     * the base does not mean much. */
    if (!g_lat.e2e_base_valid) {
        g_lat.e2e_base_us    = gap;
        g_lat.e2e_base_valid = 1;
    } else if (gap < g_lat.e2e_base_us) {
        g_lat.e2e_base_us = gap;
        g_lat.e2e_base_lowered++;
    }

    /* Clock ratio: a measurement of the stamp's unit, not an assumption. */
    if (!g_lat.clk_primed) {
        g_lat.clk_srv_first = server_stamp;
        g_lat.clk_loc_first = t_disp_us;
        g_lat.clk_primed    = 1;
    }
    g_lat.clk_srv_last = server_stamp;
    g_lat.clk_loc_last = t_disp_us;

    /* By construction `gap >= base`: no sample is ever discarded, so this
     * stage's `n` really does count every picture displayed. */
    latency_add(LAT_VID_E2E, (int64_t)(gap - g_lat.e2e_base_us));
}

/* --- Reporting ------------------------------------------------------------ */

/* Percentile read from the histogram: upper bound of the bin the rank falls in. */
static uint32_t lat_percentile(const lat_stage_t *s, const uint32_t *snap, int pct) {
    if (s->n == 0) return 0;
    uint64_t threshold = (s->n * (uint64_t)pct + 99u) / 100u;   /* rounded up */
    uint64_t cum = 0;
    for (int c = 0; c < LAT_BINS; c++) {
        cum += snap[c];
        if (cum >= threshold) return lat_bin_upper(c);
    }
    return lat_bin_upper(LAT_BINS - 1);
}

/* Milliseconds, one decimal, into a caller-provided buffer: `%.1f` over a
 * `uint32_t` of microseconds, with the overflow sentinel spelled out rather
 * than rendered as 4294967 ms. */
static void lat_ms(char *dst, size_t n, uint32_t us) {
    if (us == 0xFFFFFFFFu) { snprintf(dst, n, ">4194"); return; }
    snprintf(dst, n, "%.1f", (double)us / 1000.0);
}

static void lat_write_report(void) {
    /* Snapshot then SUBTRACT: a sample landing during the report is carried
     * over to the next window instead of being lost. A naive reset would lose
     * in silence — precisely what this repo refuses from a counter. */
    char silent[256]; int so = 0; silent[0] = '\0';
    g_lat.reports++;

    /* SAY WHAT IS NOT MEASURED, ONCE. A budget that does not announce itself as
     * partial reads as complete, and someone will conclude the chain costs the
     * sum of these lines. It costs more. */
    /* The drift is computed BEFORE writing, otherwise a window's line carries
     * the PREVIOUS window's drift — a one-window offset that would pin a
     * settling delay on the wrong moment of the session. Counter-case found by
     * running the report: two windows 0.4 ms apart both announced
     * `derive=+0.0`. */
    if (g_lat.e2e_win_min_valid && g_lat.e2e_prev_valid)
        g_lat.e2e_drift_us = (int64_t)(int32_t)(g_lat.e2e_win_min
                                                - g_lat.e2e_prev_min);

    if (g_lat.reports == 1)
        latlog("[L5] budget PARTIAL by construction — not in it: server-side "
               "encoding and the network trip (no common clock, only their "
               "VARIATION is quantified below), and the panel scan-out after "
               "the buffer swap (at least one vblank, 16.7 ms at 60 Hz) which "
               "the application cannot observe from its own thread.");

    for (int e = 0; e < LAT_NB; e++) {
        lat_stage_t *s = &g_lat.stage[e];
        uint32_t snap[LAT_BINS];
        memcpy(snap, s->bins, sizeof snap);
        uint64_t n = s->n, sum = s->sum_us;
        uint32_t worst = s->worst_us, worst_s = s->worst_session;

        if (n == 0) {
            /* A SILENT STAGE SAYS SO. An instrument missing from the report
             * reads as "nothing to report", when it actually means "nobody
             * writes here" — that is the dead-counter failure, and it has
             * already covered seven counters in this repo. */
            if (so < (int)sizeof(silent) - 20)
                so += snprintf(silent + so, sizeof(silent) - (size_t)so,
                               "%s%s", so ? " " : "", latency_stage_name((latency_stage_t)e));
            /* VI4 2026-09-11 - and the panel says so too. The report named the
             * stage "never fed" while latency_read kept handing the panel the
             * last fed window's figures, for as long as the silence lasted (up
             * to 110 s of a 120 s outage in the bench). Field by field, not a
             * memset: the session figures must survive the silence. */
            {
                latency_report_t *L = &g_lat.last[e];
                L->n = 0;
                L->avg_us = L->p50_us = L->p90_us = L->p99_us = L->worst_us = 0;
                L->worst_session_us = worst_s;
                if (L->n_session) L->silent_windows++;
            }
            continue;
        }

        const uint32_t v_avg = (uint32_t)(sum / n);
        const uint32_t v_p50 = lat_percentile(s, snap, 50);
        const uint32_t v_p90 = lat_percentile(s, snap, 90);
        const uint32_t v_p99 = lat_percentile(s, snap, 99);

        /* L19 — published for the UI BEFORE formatting: the panel reads
         * numbers, not strings, and recomputing them elsewhere would make two
         * truths for one measurement. */
        g_lat.last[e].n                = (uint32_t)n;
        g_lat.last[e].avg_us           = v_avg;
        g_lat.last[e].p50_us           = v_p50;
        g_lat.last[e].p90_us           = v_p90;
        g_lat.last[e].p99_us           = v_p99;
        g_lat.last[e].worst_us         = worst;
        g_lat.last[e].worst_session_us = worst_s;
        g_lat.last[e].n_session       += (uint32_t)n;   /* VI4 */
        g_lat.last[e].silent_windows   = 0;

        char avg[16], p50[16], p90[16], p99[16], wo[16], wos[16];
        lat_ms(avg, sizeof avg, v_avg);
        lat_ms(p50, sizeof p50, v_p50);
        lat_ms(p90, sizeof p90, v_p90);
        lat_ms(p99, sizeof p99, v_p99);
        lat_ms(wo,  sizeof wo,  worst);
        lat_ms(wos, sizeof wos, worst_s);

        /* The `n= avg= p50= p90= p99=` keys below are NOT translated, for the
         * same reason as the stage names: `tools/campaign_latency.sh` matches
         * them, and so does every captured log. */
        if (e == LAT_VID_E2E) {
            /* The only stage whose values are GAPS to a base rather than
             * absolute durations: we say so in the line, otherwise someone will
             * one day add the base and the percentiles together. */
            double ratio = 0.0;
            int64_t dloc = g_lat.clk_loc_last - g_lat.clk_loc_first;
            uint32_t dsrv = g_lat.clk_srv_last - g_lat.clk_srv_first;
            if (dloc > 0) ratio = (double)dsrv / (double)dloc;
            char base[16]; lat_ms(base, sizeof base, g_lat.e2e_base_us);
            latlog("[L5] %-15s n=%-6llu base=%s ms +avg=%s +p50=%s +p90=%s "
                   "+p99=%s +worst=%s (session %s) drift=%+.1f ms base_lowered=%u "
                   "rapport=%.4f",
                   latency_stage_name((latency_stage_t)e),
                   (unsigned long long)n, base, avg, p50, p90, p99, wo, wos,
                   (double)g_lat.e2e_drift_us / 1000.0,
                   g_lat.e2e_base_lowered, ratio);
        } else {
            latlog("[L5] %-15s n=%-6llu avg=%s p50=%s p90=%s p99=%s worst=%s "
                   "(session %s) ms",
                   latency_stage_name((latency_stage_t)e),
                   (unsigned long long)n, avg, p50, p90, p99, wo, wos);
        }

        for (int c = 0; c < LAT_BINS; c++) s->bins[c] -= snap[c];
        s->n      -= n;
        s->sum_us -= sum;
        s->worst_us = 0;
    }

    if (so)
        latlog("[L5] never fed during this window: %s — a silent stage is a "
               "broken measurement, not a path without latency", silent);

    /* The next window's base is this one's smallest gap: the fastest trip we
     * have seen, hence the one containing the least waiting. Its VARIATION is
     * the drift — a mix of the two crystals' drift (a few ppm, ~0.1 ms over
     * 10 s) and of a delay settling in. Several ms per window is not clock
     * drift: it is a queue filling up. */
    if (g_lat.e2e_win_min_valid) {
        g_lat.e2e_prev_min      = g_lat.e2e_win_min;
        g_lat.e2e_prev_valid    = 1;
        g_lat.e2e_base_us       = g_lat.e2e_win_min;   /* forget the one before last */
        g_lat.e2e_base_valid    = 1;
        g_lat.e2e_win_min_valid = 0;
    }
    g_lat.e2e_base_lowered = 0;
    g_lat.clk_primed = 0;
}

void latency_report_periodic(int64_t now_ms) {
    if (!latency_enabled()) return;
    if (g_lat.next_report_ms == 0) {          /* first call: arm the deadline */
        g_lat.next_report_ms = now_ms + lat_period_ms();
        return;
    }
    if (now_ms < g_lat.next_report_ms) return;
    g_lat.next_report_ms = now_ms + lat_period_ms();
    lat_write_report();
}

void latency_report_final(void) {
    if (!latency_enabled()) return;
    /* Without it, a short session (an 8 s try, a disconnection) leaves NO
     * measurement at all: the 10 s window was never reached and everything
     * accumulated leaves with the process. */
    lat_write_report();
}

int latency_read(latency_stage_t e, latency_report_t *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if ((unsigned)e >= (unsigned)LAT_NB) return 0;
    *out = g_lat.last[e];
    return out->n > 0;
}

void latency_reset_session(void) {
    memset(&g_lat, 0, sizeof g_lat);
}
