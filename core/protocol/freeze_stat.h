/* freeze_stat.h - the G44 micro-freeze detector's arithmetic, time as a
 * parameter.
 *
 * PURE module: no dependency, no global state, no clock, no getenv - testable
 * offline (tests/test_freeze_stat.c), same shape as idr_policy.h and
 * pad_mouse.h. The caller (ctrl_session_glue.c::on_frame) owns the clock and
 * the log strings; this file only decides WHAT is worth a line.
 *
 * What it measures: the interval between two pictures handed to the display by
 * the decoder. An interval far above the frame period is a picture that stayed
 * frozen on screen for that long.
 *
 * HO-2 (2026-09-11) - why this state is a struct and not function statics.
 * The G44 block kept last_ms / nfr / nfreeze / sum_ms / max_ms / logn in
 * function statics. The per-session memset of g_glue does not reach them, and
 * one process runs many sessions (autotest series, auto-reconnect, back to the
 * VM list). Measured consequences, all silent:
 *   - the 25-line budget was spent on one-frame gaps of 45-76 ms: in the
 *     2026-09-10 baseline the 25th line is 11.6 s after the first picture, and
 *     37 more freezes were counted and never printed - including the 76 and
 *     86 ms ones, larger than any printed gap. Every later session of the same
 *     process printed no line at all;
 *   - the first interval of session N+1 was the whole pause since session N's
 *     last picture: it became every later bilan's `max` and inflated `moy`;
 *   - the bilan cadence (every 500 intervals) ran on process-wide counts.
 * This is the family CLAUDE.md names first - session state in a function
 * `static` - and KB §3.28 listed G44's `logn` in it on 2026-08-25;
 * S34 moved the sibling counters, not these.
 *
 * Zero-initialised state is a valid fresh session (the glue's memset gives
 * exactly that); freeze_stat_reset() says so explicitly.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* A gap at or above this counts as a freeze (G44's historical threshold,
 * ~2 frame periods at 48 fps). Kept as is: the bilan's `gels(>=45ms)` must stay
 * comparable with every log already written. */
#define FREEZE_STAT_GAP_MS       45
/* Default per-LINE threshold. On an animated 36-47 fps desktop a 45-76 ms gap
 * is one skipped server frame (4-6 % of intervals): printing those spends the
 * budget in seconds. They stay counted, in the bilan and the histogram. */
#define FREEZE_STAT_LOG_MS      100
/* Per-freeze lines per SESSION. */
#define FREEZE_STAT_LOG_BUDGET   25
/* Intervals between two periodic bilans. */
#define FREEZE_STAT_BILAN_EVERY 500

/* Returned by freeze_stat_feed(). Flags: one picture can both close a
 * loggable gap and land on the bilan cadence (the old block could print both
 * lines for the same picture). */
enum {
    FREEZE_STAT_NONE    = 0,
    FREEZE_STAT_LOG_GAP = 1,   /* print one line for s->dt_ms */
    FREEZE_STAT_BILAN   = 2,   /* print the periodic bilan */
};

/* Histogram buckets, mirroring D4's (ctrl_session.c) so the two can be read
 * side by side: a gap the budget did not print is still counted here. */
enum {
    FREEZE_H_45_99 = 0,
    FREEZE_H_100_299,
    FREEZE_H_300_999,
    FREEZE_H_1000_UP,
    FREEZE_H_COUNT
};

/* Session state. Reset it ON EVERY SESSION. */
typedef struct {
    bool     started;   /* a picture has been seen THIS session */
    int64_t  last_ms;   /* time of that picture */
    int64_t  dt_ms;     /* the interval the last feed measured (0 on the first) */
    uint32_t nfr;       /* intervals measured this session */
    uint32_t nfreeze;   /* of which >= FREEZE_STAT_GAP_MS */
    int64_t  sum_ms;
    int64_t  max_ms;
    uint32_t logged;    /* per-freeze lines granted this session */
    uint32_t hist[FREEZE_H_COUNT];
} freeze_stat_t;

/* memset rather than a compound literal: the header then also compiles as
 * C++, should a display-side twin in stream_view.cpp ever include it. */
static inline void freeze_stat_reset(freeze_stat_t *s)
{
    memset(s, 0, sizeof *s);
}

static inline int freeze_stat_bucket(int64_t dt_ms)
{
    return dt_ms < 100  ? FREEZE_H_45_99
         : dt_ms < 300  ? FREEZE_H_100_299
         : dt_ms < 1000 ? FREEZE_H_300_999
                        : FREEZE_H_1000_UP;
}

/* Call once per picture handed to the display, with a monotonic time in ms.
 * `log_min_ms` is the per-line threshold (FREEZE_STAT_LOG_MS unless a toggle
 * says otherwise); below FREEZE_STAT_GAP_MS it is raised to it, since a gap
 * that is not a freeze is never worth a line.
 *
 * COUNTER-CASE (HO-2) - the first picture of a session measures NOTHING. With
 * process-wide state it measured the pause since the previous session (35 s in
 * the verifier's model) and made that the `max` of every later bilan. */
static inline unsigned freeze_stat_feed(freeze_stat_t *s, int64_t now_ms,
                                        int64_t log_min_ms)
{
    if (!s->started) {
        s->started = true;
        s->last_ms = now_ms;
        s->dt_ms   = 0;
        return FREEZE_STAT_NONE;
    }
    int64_t dt = now_ms - s->last_ms;
    /* A clock that goes backwards is not a freeze, and must not pull the mean
     * down either (the L21 family: rtt.h refuses the same input). Impossible
     * with CLOCK_MONOTONIC; this module takes time from its caller. */
    if (dt < 0) dt = 0;
    s->last_ms = now_ms;
    s->dt_ms   = dt;
    s->nfr++;
    s->sum_ms += dt;
    if (dt > s->max_ms) s->max_ms = dt;

    unsigned ev = FREEZE_STAT_NONE;
    if (dt >= FREEZE_STAT_GAP_MS) {
        s->nfreeze++;
        s->hist[freeze_stat_bucket(dt)]++;
        const int64_t thr = log_min_ms > FREEZE_STAT_GAP_MS ? log_min_ms
                                                            : FREEZE_STAT_GAP_MS;
        if (dt >= thr && s->logged < FREEZE_STAT_LOG_BUDGET) {
            s->logged++;
            ev |= FREEZE_STAT_LOG_GAP;
        }
    }
    if ((s->nfr % FREEZE_STAT_BILAN_EVERY) == 0) ev |= FREEZE_STAT_BILAN;
    return ev;
}

/* Mean interval of the session so far; 0 before the first interval (the final
 * per-session bilan can run on a session that showed one picture or none). */
static inline int64_t freeze_stat_mean_ms(const freeze_stat_t *s)
{
    return s->nfr ? s->sum_ms / (int64_t)s->nfr : 0;
}
