/* latency — what a session costs to deliver a picture, a sound, a click.
 *
 * === WHY THIS MODULE EXISTS (L5, 2026-08-29) ===
 *
 * A review of the whole path priced every item of latency, by READING the code
 * and building offline benches. It produced usable numbers, and one finding that
 * puts all of them in perspective: the most expensive link on the video path —
 * a finished picture waiting in `g_display_buf` for the NEXT one to arrive — is
 * also the only one nothing measures. Two instruments bracket it without
 * covering it: `[L1]` runs from "picture assembled" to "picture decoded",
 * `stat_latency` runs from "picture pushed" to "picture drawn". The link falls
 * exactly between them.
 *
 * Worse, `stat_latency` was only fed under `if (show_perf_stats &&
 * hud_.visible())`: a counter WRITTEN only while you are looking at it, leaving
 * no trace in the log. This repo has already paid for seven dead counters of
 * that family — displayed, never incremented. (VI5 2026-09-11: it is now fed
 * with the video/file-aff sample itself, one per new picture, panel open or
 * not.)
 *
 * No live session is possible from the command line (the desktop token comes and
 * goes), so whatever the application does not measure itself will not be
 * measured at all. Hence this module: the next real session must yield its
 * numbers without anyone driving it.
 *
 * === WHAT IT MEASURES, AND WHY PERCENTILES ===
 *
 * An average hides precisely what is felt. A path averaging 25 ms with a 99th
 * percentile at 200 ms plays WORSE than a tight path averaging 40 ms: it is the
 * spikes that make you miss a shot, not the mean. Every stage therefore reports
 * n / mean / p50 / p90 / p99 / worst.
 *
 * Percentiles come from a VARIABLE-STEP HISTOGRAM (fine below a millisecond,
 * coarse beyond 250 ms), not from sorting: nothing to allocate, nothing to sort,
 * and a sample costs one bucket increment. A percentile read from a histogram is
 * the UPPER BOUND of its bucket, so always slightly pessimistic — never
 * optimistic, which is the right direction for the error on a latency figure.
 *
 * === THE END-TO-END MEASURE, AND ITS TRAP ===
 *
 * The stamp a picture carries comes from the SERVER. We share no clock with it:
 * `t_local - t_server` contains an arbitrary clock offset plus transit time. THE
 * ABSOLUTE GAP THEREFORE MEANS NOTHING, and reporting it as a latency would be a
 * lie with a number attached.
 *
 * What IS measurable without a common clock is its VARIATION: the clock offset
 * is constant over a session, so everything that MOVES in that gap is time we
 * added. So we publish:
 *   - the window's BASE = the smallest gap observed. That is the fastest trip we
 *     have seen; by construction it contains almost no waiting, and serves as
 *     zero.
 *   - the distribution of `gap - base`, which is the ADDED latency, for real.
 *   - the DRIFT of that base from one window to the next. It mixes the drift of
 *     the two crystals (a few ppm, so ~0.1 ms over 10 s) with any delay that is
 *     settling in. A drift climbing by several ms per window is NOT clock drift:
 *     it is a queue filling up.
 *
 * A window's base is the zero for the NEXT one: you cannot subtract a minimum
 * you will only know at the end. Accepted consequence: the first window compares
 * against its own first sample and may produce negative values clamped to zero —
 * they are counted separately (`below_base`), because a large number of them
 * signals that the previous base was an outlier, not a fast trip.
 *
 * THE TRAP, DOCUMENTED RATHER THAN ASSUMED: the unit of the `VideoFrame` stamp
 * is not certain. The code divides it by 90 as if it were 90 kHz ticks
 * (vid_reasm.c), while the measured gaps between successive pictures (median
 * 41 727 units at ~24 fps) point to MICROSECONDS. So we assume microseconds —
 * and publish the measured RATIO `server units / local us` on every report, so
 * the assumption checks itself: ~1.000 confirms microseconds, ~0.090 would say
 * 90 kHz, ~0.001 would say milliseconds. One session settles it.
 *
 * === WHAT IT COSTS ===
 *
 * One `clock_gettime` per stage per picture — not per byte, not per chunk.
 * MEASURED on desktop (i7-9750H, -O2, 20 million calls): 15.2 ns for
 * `clock_gettime(CLOCK_MONOTONIC)` and 3.7 ns for `latency_add`, so 0.13 us per
 * picture for the video path's seven clocks and seven deposits.
 * ESTIMATED for Switch, and labelled as an estimate: a 1.02 GHz Cortex-A57
 * against a ~4 GHz i7 gives a factor of 8 to 10, so ~1.3 us on a 20 ms picture —
 * a few thousandths of a percent. On HOS `clock_gettime(CLOCK_MONOTONIC)`
 * resolves to `armGetSystemTick()`, a system register read, so the factor should
 * be no worse; that point is not measured.
 * No allocation, no lock, no I/O on the hot path: the periodic report is the
 * only writer, once every 10 s.
 *
 * `SHADOW_LATENCE=0` restores the UNINSTRUMENTED application — that is the
 * revert value in this repo's sense: it brings back the previous path, not a
 * variant. `SHADOW_LATENCE_MS` sets the report period (10000 by default).
 *
 * NOTE ON THE TOGGLE AND THE STAGE NAMES: neither is translated. `SHADOW_LATENCE`
 * appears in campaign scripts and in every log already captured, and the stage
 * names (`video/rafale`, `input/send`...) are parsed by
 * `tools/campaign_latency.sh` and by the offline analyses. They are a DATA
 * interface, like the keys in the settings file — renaming them would silently
 * break every comparison with past measurements.
 *
 * === CONCURRENCY: WHY THERE IS NO LOCK ===
 *
 * Each stage has EXACTLY ONE writer, and that is not a convenient assumption:
 * it is the shape of the path. Reception -> UDP thread; decode queue and
 * decoding -> decode thread; display queue, upload, draw cadence and end-to-end
 * -> UI thread; input -> drain thread; audio -> UDP thread. The reader (the
 * report) is a third thread, and it only reads integers.
 *
 * The report does NOT RESET: it photographs, then SUBTRACTS its photograph. A
 * sample arriving during the report is therefore kept for the next window
 * instead of being lost — a naive reset would silently drop samples, and a
 * counter that loses in silence is exactly what this repo no longer wants.
 *
 * One race remains and it is ACCEPTED, not overlooked: the report writes the
 * end-to-end base (`base = window minimum`) while the UI thread may be
 * depositing a picture. Worst case: ONE sample measured against the old base, in
 * a window that holds five hundred. A lock for that would cost more on the hot
 * path than the error it avoids — and this repo has already paid for a lock held
 * across an I/O.
 */
#ifndef SHADOW_LATENCY_H
#define SHADOW_LATENCY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The stages of the path. The order follows a picture's journey, then input,
 * then audio: it is also the order of the report, so that it reads as a budget.
 *
 * The enum names are English; the STRINGS they map to are not translated — they
 * are parsed by the campaign tooling and appear in every captured log. */
typedef enum {
    LAT_VID_BURST = 0,    /* first chunk -> last chunk of a picture (UDP) */
    LAT_VID_HOLD,         /* L7 — picture COMPLETE -> flushed from g_display_buf */
    LAT_VID_DEC_QUEUE,    /* assembled -> picked up by the decode thread */
    LAT_VID_DECODE,       /* h264_decoder_feed_annexb: submit + receive, and on the
                             synchronous default paths the frame callback too -
                             the view's plane copy included (VI2) */
    LAT_VID_DISP_QUEUE,   /* pushed -> popped by the draw. "Pushed" = the planes are
                             copied and the picture goes for the queue lock (VI2):
                             the copy is decode-thread work, in LAT_VID_DECODE only.
                             The panel's "display wait" row is this sample (VI5). */
    LAT_VID_UPLOAD,       /* CPU cost of the video render call, per draw; it uploads
                             the planes only when a new picture arrived (HO-1) */
    LAT_VID_CADENCE,      /* interval between two draws of the stream view */
    LAT_VID_E2E,          /* server stamp -> display (VARIATION only) */
    LAT_IN_SEND,          /* device read -> write on the socket */
    LAT_IN_PAD,           /* L9 — gamepad HID read -> write on the socket */
    LAT_IN_PAD_RATE,      /* L9 — interval between two gamepad HID reads */
    LAT_AUD_QUEUE,        /* depth of the audio output queue */
    /* ING-1 2026-09-11 - one pass of the session receive loop, BODY only: from
     * the top of the body to just before its poll wait (ctrl_session.c). The
     * console's ~300 ms Wi-Fi freezes are still unattributed; a long pass says
     * the LOOP held the sockets while the data waited. NOT a budget line: it
     * overlaps the video stages and must not be added to them. Two clock reads
     * per pass, on the receive thread (its only writer). Silent by design in
     * TCP video mode (K15i: no UDP video socket) and never fed there. Last in
     * the enum so the stages above keep their values and their report order. */
    LAT_RX_PASS,
    LAT_NB
} latency_stage_t;

/* Short stage name as it appears in the log. Never null. */
const char *latency_stage_name(latency_stage_t e);

/* Are we measuring? Read once (`SHADOW_LATENCE`, 1 by default). Callers must
 * test this BEFORE taking a clock: the point of setting it to 0 is that the path
 * becomes identical to what it was without this module. */
int latency_enabled(void);

/* Monotonic clock in microseconds. One definition for the whole path: two
 * stages measured against two different clocks do not add up. */
int64_t latency_now_us(void);

/* One sample. Negative values are dropped: a clock going backwards, or a
 * missing milestone, must not manufacture a percentile. */
void latency_add(latency_stage_t e, int64_t us);

/* --- Video path: carrying the server stamp ------------------------------- */

/* Published by `flush_display_buffer` JUST before it calls `on_video`, which
 * runs SYNCHRONOUSLY on the same stack: the glue therefore reads the value of
 * the very picture it is handed, with no key and no matching step. */
void     latency_video_assembled(uint32_t server_stamp, int64_t t_asm_us);
uint32_t latency_video_current_stamp(void);
int64_t  latency_video_asm_now_us(void);

/* The decoder carries exactly ONE field through to its output: `pts`. We use it
 * as a KEY — never as a value — to recover the server stamp on the other side.
 * An 8-entry ring, walked newest to oldest: two pictures sharing a `pts` (the
 * server does produce them, see G40) yield the newest, which is the one being
 * displayed. Returns 0 when nothing matches. */
void     latency_video_into_decoder(int64_t pts, uint32_t server_stamp);
uint32_t latency_video_stamp_for_pts(int64_t pts);

/* The picture reaches the screen. Computes the gap to the server stamp in
 * unsigned 32-bit arithmetic: the field's wrap (~71.6 min) and that of the
 * truncated local clock cancel out, as long as the real gap stays well below.
 * A no-op when the stamp is 0 (no carrier). */
void     latency_video_displayed(uint32_t server_stamp, int64_t t_disp_us);

/* --- Reporting ------------------------------------------------------------ */

/* Call from the session loop, every turn: the function limits ITSELF to one
 * write every `SHADOW_LATENCE_MS`. The first call arms the deadline without
 * writing anything. */
void latency_report_periodic(int64_t now_ms);

/* Full reset. One session = one set of measurements: keeping the previous
 * session's samples would blend two operating points, and this repo has already
 * drawn a false conclusion from an unsegmented log. Called when each session
 * starts. */
void latency_reset_session(void);

/* Final report, written even when the deadline has not been reached. Call at
 * the end of a session: without it an 8 s session leaves no measurement at
 * all. */
void latency_report_final(void);

/* === L19 2026-08-29 — READING THE STAGES FROM THE UI ===
 *
 * The percentiles only ever existed in the log. But it is WHILE PLAYING that
 * you want to know whether a spike just landed, and nobody reads a log while
 * playing. So the pause panel shows them, live.
 *
 * What is returned is the LAST COMPLETE REPORT, not the window in progress: a
 * partial window gives percentiles that move on every picture and an `n` that
 * climbs, which reads as an unstable path when it is only the instrument
 * filling up. The values therefore change every ten seconds, as a block, and
 * each one describes a whole window.
 *
 * `worst_session` is the only value that crosses windows: it is the one that
 * answers "has it dropped out at any point since I started playing". */
typedef struct {
    uint32_t n;               /* samples in the window; 0 = silent stage */
    uint32_t avg_us;
    uint32_t p50_us, p90_us, p99_us;
    uint32_t worst_us;        /* worst of the window */
    uint32_t worst_session_us;/* worst since the session started */
    uint32_t n_session;       /* VI4: samples since the session started */
    uint32_t silent_windows;  /* VI4: consecutive silent windows after a fed one */
} latency_report_t;

/* Returns 1 when the stage was fed in the last window, 0 when it was silent.
 * On 0 the per-window figures are ZERO - never an older window's (VI4) - while
 * worst_session_us, n_session and silent_windows still describe the session,
 * so a caller can tell "silent now" from "never fed". Safe from any thread. */
int latency_read(latency_stage_t e, latency_report_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_LATENCY_H */
