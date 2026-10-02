/* ctrl_session_glue.c - see header. */

#include "ctrl_session_glue.h"
#include "ctrl_session.h"
#include "idr_policy.h"
#include "freeze_stat.h"    /* HO-2: G44 micro-freeze detector, per session */
#include "cursor_state.h"   /* CUR1 phase 2 2026-05-18 */
#include "../media/h264_decoder.h"
#include "../common/stats.h"
#include "../media/audio.h"          /* I3 2026-05-18 audio path natif */
#include "../input/shadow_input.h"   /* the UI input queue and its drain */
#include "../common/log.h"
#include "latency.h"   /* L5 : instrumentation du chemin video */

/* S81 - this module's log category. See shadow/journal.h: it is declared here,
 * never inferred from the text of the messages. */
#define gllog(...) JOURNAL_INFO_(JOURNAL_CAT_SESSION, __VA_ARGS__)
#define gldbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SESSION, __VA_ARGS__)

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>   /* BUG3 2026-05-18 — drain thread input mode natif */
#include <time.h>      /* nanosleep */

/* LIB1 2026-10-02 - where decoded pictures go, REGISTERED rather than linked.
 * See ctrl_session_glue.h for why this stopped being
 * `extern stream_view_push_yuv`. Written once before the session starts and
 * read by the decode thread, so no lock: a sink changed mid-session is not a
 * thing any client needs and would be the only reason to add one. */
static ctrl_session_frame_sink g_frame_sink;

void ctrl_session_glue_set_frame_sink(ctrl_session_frame_sink sink)
{
    g_frame_sink = sink;
}

/* G25 2026-08-22 - queue feeding the decode thread. Bounded depth: in steady
 * state decoding keeps up, so the queue sits at 0-1; the slack is there for
 * bursts.
 *
 * G38 2026-08-22 - depth raised from 16 to 64 (`SHADOW_DEC_QUEUE`). The
 * decisive measurement (260 s GUI session): lost=0/abandoned=0 (no network
 * loss at all) YET NAL top=12289 vs decoded=12062 = **227 assembled frames
 * never decoded** = overflows of THIS queue. Decoding keeps up on average
 * (49/s decoded > 43.8/s received): the overflows are transient BURSTS
 * (NVDEC/GL contention, a large IDR) where 16 frames are not enough to absorb
 * the spike. Every frame dropped here breaks a reference -> drift/smear, AND
 * used to trigger an IDR (G34) -> larger IDR -> more contention -> spiral. A
 * deeper queue absorbs the spike; in steady state it drains (49>43.8), so the
 * added latency is negligible outside a spike. The array is sized to the MAX;
 * the effective depth is `g_decq_cap` (tunable at runtime for A/B). */
#define DEC_QUEUE_CAP 256
/* L5 2026-08-29 - `server_stamp` (the server timestamp carried by the frame)
 * and `t_asm_us` (the instant it was assembled) travel WITH the frame inside
 * the queue rather than in a shared variable: between enqueue and dequeue
 * there is a thread and up to two frames of waiting, so a shared variable
 * would hand back the value of the NEXT frame. `t_recv_ms` stays in
 * milliseconds: it feeds the [L1] report, whose format is read by humans. */
typedef struct {
    uint8_t  *data; size_t len; uint64_t pts_90k; long long t_recv_ms;
    uint32_t  estampille_srv;   /* L5 : horodatage VideoFrame, tel quel */
    int64_t   t_asm_us;         /* L5 : instant de l'assemblage, us monotones */
    bool      key;              /* CONC-4: a key frame, reported when it is dropped */
} dec_item_t;

/* L1: monotonic clock in ms, to measure end-to-end latency. */
static long long l1_now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int g_decq_cap = 0;   /* G38 : profondeur effective, init depuis SHADOW_DEC_QUEUE */

/* Singleton state - h264_frame_cb is a C function pointer with no capture, and
 * we only ever have one session live at a time (Shadow = 1 client per account). */
typedef struct {
    h264_decoder  *h264;
    audio_decoder *audio;   /* I3 2026-05-18 — Opus decoder + ALSA playback */
    uint32_t       frames_displayed;
    /* S34: G13 stall detector - session state (rule lives in idr_policy.h). */
    idr_stall_t    dec_stall;
    /* HO-2: G44 micro-freeze detector - session state (rule in freeze_stat.h).
     * It lived in function statics of on_frame, which the per-session memset
     * never reached: every session after the first inherited a spent line
     * budget, the pause as its `max`, and a process-wide bilan cadence. */
    freeze_stat_t  freeze;
    int            last_w;
    int            last_h;
    /* === G25 2026-08-22 - DEDICATED DECODE THREAD (mirrors the official client) ===
     * Decouples decoding (slow: avcodec_send/receive + hwaccel transfer) from
     * reception (which must stay real-time, otherwise the reorder buffer
     * declares losses that did not happen). on_video ENQUEUES, dec_thread_fn
     * DEQUEUES and decodes. Teardown: the thread is joined BEFORE the decoder
     * is destroyed, so the decoder has a single producer and needs no lock of
     * its own (cf. BUG2: the race came from a worker that was never joined). */
    pthread_t         dec_thread;
    bool              dec_thread_started;
    volatile bool     dec_abort;
    pthread_mutex_t   dec_mtx;
    pthread_cond_t    dec_cv;
    dec_item_t        decq[DEC_QUEUE_CAP];
    int               decq_head, decq_tail;
    uint32_t          dec_dropped;   /* images jetees car file pleine (decodeur en retard) */
    /* CONC-4 2026-09-11 - session state that lived in function statics, the
     * family CLAUDE.md names first: the [G38] line budget, the enqueue and
     * dequeue marks the drop line reports, the stats publish cadence, the [L1]
     * accumulators and the [K17] witness. The memset in ctrl_session_glue_run
     * now resets them with everything else. */
    uint32_t          drop_log;
    long long         last_deq_ms, last_enq_ms;   /* -1 = none yet this session */
    uint32_t          pub_countdown;
    long long         l1_sum, l1_worst;
    unsigned          l1_n;
    unsigned          k17_seen;
    /* AUD-INS-3 2026-09-11 - the audio counters' publication, per session like
     * everything above: SHADOW_AUDIO_STATS_TICK resolved for THIS session
     * (before the decode thread starts, so both threads only read it), and the
     * time of the last AUDIO-group merge from on_audio. */
    int               aud_stats_tick;
    long long         aud_pub_ms;
    /* BUG3 2026-05-18 - input drain thread for the native path.
     * `shadow_input_drain_queue` was called only from the WebRTC/DC tick. On
     * the native path those code paths do not exist, so the queue filled up and
     * was never drained -> mouse and keyboard went silent. */
    pthread_t      input_drain_thread;
    volatile bool  input_drain_stop;
    bool           input_drain_started;
    /* F22 2026-05-23 00h45 - anti-green-MB cache: for every MB of the frame,
     * keeps the Y/U/V content of the last MB that was CLEAN (= not green).
     * When the current MB is green, it is replaced from the cache.
     * Layout: Y[1920*1088], U[960*544], V[960*544] = 3.1 MiB. */
    uint8_t       *clean_y;
    uint8_t       *clean_u;
    uint8_t       *clean_v;
    int            clean_w, clean_h;
    int            clean_ys, clean_us, clean_vs;
} glue_state_t;

static glue_state_t g_glue;

/* BUG3 2026-05-18 - drains the events posted by stream_view.
 *
 * === L16 2026-08-29 - WAIT ON THE EVENT, NOT ON THE CLOCK ===
 * This thread used to sleep a fixed 10 ms, and the original comment owned it:
 * "input latency ~5 ms on average (acceptable for gaming)". Five milliseconds
 * on average and ten at worst, added to every gesture before the byte even
 * leaves. The producer now signals, and the thread wakes up immediately.
 *
 * The 10 ms delay survives as a BOUND, not as a rate: on this console a
 * long-lived thread must re-check its abort flag at a coarse granularity, or
 * HOS leaks its handle and the console has to be rebooted. `SHADOW_INPUT_CV=0`
 * restores the fixed sleep. */
static void *input_drain_thread_fn(void *arg) {
    (void)arg;
    static int g_cv = -1;
    if (g_cv < 0) {
        const char *e = getenv("SHADOW_INPUT_CV");
        g_cv = e ? atoi(e) : 1;
    }
    while (!g_glue.input_drain_stop) {
        shadow_input_drain_queue();
        if (g_cv) {
            shadow_input_wait_cmd(10);
        } else {
            struct timespec ts = { 0, 10 * 1000 * 1000 };  /* 10 ms */
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* DEBUG: dump a YUV/NV12 frame as a .ppm for offline inspection.
 * Enabled by SHADOW_FRAME_DUMP=1. Dumps at frame #1, 10, 30, 60, 120, 300.
 * Handles YUV420P (3 planes) AND NV12 (= 2 planes: Y + interleaved UV). */
#include "../services/config.h"   /* SHADOW_DATA_DIR */
static void dump_frame_yuv_to_ppm(int width, int height,
        const uint8_t *data_y, int linesize_y,
        const uint8_t *data_u, int linesize_u,
        const uint8_t *data_v, int linesize_v,
        int format,
        uint32_t frame_num) {
    if (!data_y || width <= 0 || height <= 0) return;
    /* NV12 = data_u holds interleaved UV, data_v is usually NULL.
     * YUV420P = data_u holds U, data_v holds V. */
    bool is_nv12 = (data_v == NULL || linesize_v == 0);
    if (!is_nv12 && !data_u) return;
    if (is_nv12 && !data_u) return;

    char path[256];
    snprintf(path, sizeof(path), SHADOW_DATA_DIR "frame_%04u.ppm", frame_num);
    FILE *f = fopen(path, "wb");
    if (!f) {
        gllog("[glue] dump_frame: fopen FAIL path=%s", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        const uint8_t *yrow = data_y + (size_t)y * linesize_y;
        int uv_y = y / 2;
        for (int x = 0; x < width; x++) {
            int uv_x = x / 2;
            int U, V;
            if (is_nv12) {
                /* UV interleaved : [U0,V0,U1,V1,...] per row of (width/2)*2 bytes */
                size_t uv_off = (size_t)uv_y * linesize_u + (size_t)uv_x * 2;
                U = (int)data_u[uv_off] - 128;
                V = (int)data_u[uv_off + 1] - 128;
            } else {
                U = (int)data_u[(size_t)uv_y * linesize_u + uv_x] - 128;
                V = (int)data_v[(size_t)uv_y * linesize_v + uv_x] - 128;
            }
            int Y = yrow[x];
            int R = Y + (int)(1.402f * V);
            int G = Y - (int)(0.344f * U + 0.714f * V);
            int B = Y + (int)(1.772f * U);
            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;
            uint8_t rgb[3] = {(uint8_t)R, (uint8_t)G, (uint8_t)B};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    gllog("[glue] dumped frame %u to %s (%dx%d %s)",
               frame_num, path, width, height, is_nv12 ? "NV12" : "YUV420P");
    (void)format;
}

static void on_frame(int width, int height,
                       const uint8_t *data_y, int linesize_y,
                       const uint8_t *data_u, int linesize_u,
                       const uint8_t *data_v, int linesize_v,
                       int format, int64_t pts, void *user) {
    g_glue.frames_displayed++;
    /* K17 - INPUT witness for the glue. `pushYuvFrame` was never being called
     * while 2258 frames were being decoded: we need to know whether they even
     * reach this callback, and with which pixel format. Logged once per format
     * value, so a handful of lines in total. */
    {
        /* CONC-4 - per session (g_glue.k17_seen), was a function static. */
        if (format >= 0 && format < 32 && !(g_glue.k17_seen & (1u << format))) {
            g_glue.k17_seen |= (1u << format);
            gllog("[K17] passerelle on_frame : format=%d %dx%d ls=[%d,%d,%d]",
                       format, width, height, linesize_y, linesize_u, linesize_v);
        }
    }
    g_glue.last_w = width;
    g_glue.last_h = height;

    /* G44 2026-08-22 - MICRO-FREEZE DETECTOR: measures the interval between
     * frames ACTUALLY handed to the display. An interval >> normal means a
     * frozen picture (nothing new to show for that long). We log the big gaps
     * plus a periodic summary (p50 / p95 / max, number of freezes). Pure
     * diagnostics, no effect. SHADOW_DIAG_FREEZE=0 turns it off. */
    {
        static int g_diag_freeze = -1;
        if (g_diag_freeze < 0) { const char *e = getenv("SHADOW_DIAG_FREEZE"); g_diag_freeze = e ? atoi(e) : 1; }
        /* HO-2 2026-09-11 - per-LINE threshold, default 100 ms. At 45 ms every
         * one-frame gap of an animated desktop got a line and the 25-line
         * budget was gone 11.6 s after the first picture (baseline of
         * 2026-09-10); those gaps stay counted in the bilan and the histogram.
         * SHADOW_DIAG_FREEZE_LOG_MS=45 brings back a line per 45 ms gap (the
         * budget is per session either way). Env caches stay static on
         * purpose; the detector's STATE is g_glue.freeze. */
        static int g_freeze_log_ms = -1;
        if (g_freeze_log_ms < 0) {
            const char *e = getenv("SHADOW_DIAG_FREEZE_LOG_MS");
            g_freeze_log_ms = e ? atoi(e) : FREEZE_STAT_LOG_MS;
        }
        if (g_diag_freeze) {
            struct timespec ts_; clock_gettime(CLOCK_MONOTONIC, &ts_);
            const int64_t now = (int64_t)ts_.tv_sec * 1000 + ts_.tv_nsec / 1000000;
            freeze_stat_t *fs = &g_glue.freeze;
            const unsigned ev = freeze_stat_feed(fs, now, g_freeze_log_ms);
            if (ev & FREEZE_STAT_LOG_GAP)
                gllog("[G44] micro-freeze: %lld ms with no fresh picture", (long long)fs->dt_ms);
            if (ev & FREEZE_STAT_BILAN)
                gllog("[G44] summary: %u pictures, avg=%lld ms, max=%lld ms, freezes(>=45ms)=%u (%.2f%%)",
                           fs->nfr, (long long)freeze_stat_mean_ms(fs), (long long)fs->max_ms,
                           fs->nfreeze, 100.0 * fs->nfreeze / fs->nfr);
        }
    }

    /* D7 2026-08-21 - bisect of the GUI-only freeze (KB.md §3.17). Decoding runs
     * on the session thread in BOTH modes (the headless build decodes too:
     * decoded=13465 over 395 s); the only remaining GUI/headless delta is the
     * display work done here. SHADOW_GUI_NOVIDEO=1 short-circuits that handoff
     * while keeping everything else (decode, ctrl, channels, input) intact: if
     * the GUI then survives past 118 s, the cause is the display path; if it
     * dies anyway, the cause is elsewhere. Diagnostics only - the picture is
     * deliberately black under this flag. */
    static int novideo = -1;
    if (novideo < 0) {
        const char *e = getenv("SHADOW_GUI_NOVIDEO");
        novideo = e ? atoi(e) : 0;
    }
    if (novideo) return;

    /* RE 2026-05-14 N25 workaround: the server was only sending first_mb=0
     * slices (= the top 50% of the 1920x1080 picture). The bottom 50% stayed
     * green (= placeholder). We clamped the output to 540 (= height/2) to show
     * only the half that was real. SHADOW_NOCROP=1 disabled the crop.
     * N40 winner: EC=3 + Request_Flush 1Hz -> ~85% of the picture visible. The
     * 540 clamp is no longer needed (= the bottom is now filled, by
     * extrapolation). Default = no crop. SHADOW_CROP=1 -> re-enable the
     * height/2 clamp (= the old behaviour). */
    static int crop_checked = 0;
    static int crop_enabled = 0;
    if (!crop_checked) {
        const char *e = getenv("SHADOW_CROP");
        if (e && atoi(e) == 1) crop_enabled = 1;
        crop_checked = 1;
    }
    int eff_height = crop_enabled ? height / 2 : height;

    /* F19 2026-05-22 23h40: crop the bottom N pixels to hide the MB rows that
     * stay green/corrupt even with a complete bytestream. PPM analysis shows
     * rows 1056-1071 (= MB row 66) at 94% green pixels despite 0 incomplete
     * frames. Suspected server-side encoder bug (= last MB row not encoded, OR
     * the decoder emitting garbage for a partial MB).
     * Default is a 16px (= 1 MB row) crop. SHADOW_CROP_BOTTOM=0 disables it,
     * =32 crops 2 MB rows. */
    static int g_crop_bottom = -1;
    if (g_crop_bottom < 0) {
        const char *e = getenv("SHADOW_CROP_BOTTOM");
        /* Default 0 = no crop. SHADOW_CROP_BOTTOM=N hides N pixels at the bottom. */
        g_crop_bottom = e ? atoi(e) : 0;
    }
    if (g_crop_bottom > 0 && eff_height > g_crop_bottom) {
        eff_height -= g_crop_bottom;
    }

    /* F22 2026-05-23 00h50: ANTI-GREEN-MB cache + replace.
     * Detects pure-green 16x16 MB blocks (= Y mean < 8 inside the MB) and
     * replaces them with the remembered content of the last clean frame.
     * Target: the bottom areas where a libavcodec CABAC error left MBs
     * undecoded -> pure black/green default fill. Hides the pixel scatter
     * cleanly. Hot path: ~6 MFLOPS per 1080p frame = fine on desktop.
     * Default ON. SHADOW_ANTI_GREEN=0 disables it.
     *
     * === G23 2026-08-22 - DEFAULT BACK TO 0: F22 WAS CORRUPTING A CORRECT
     * PICTURE ===
     * F22 replaces every "green" OR DARK MB (Y<12) with content remembered from
     * an earlier frame. It was a band-aid over the old broken stream (green
     * blocks). Since G4/G21/G22 the bitstream is correct: there are no green
     * blocks left, but DARK scenes (an animated night-race wallpaper) have
     * legitimately dark MBs (Y<12) -> F22 repaints them with stale content ->
     * 16x16 artefact SQUARES, exactly the defect that was reported. This
     * post-processing only runs in the GUI (on_frame), so the ffmpeg/PNG
     * validation of the H.264 stream could not see it. Disabled.
     * SHADOW_ANTI_GREEN=1 restores it. */
    static int g_anti_green = -1;
    if (g_anti_green < 0) {
        const char *e = getenv("SHADOW_ANTI_GREEN");
        g_anti_green = e ? atoi(e) : 0;   /* G23 : defaut 0 (obsolete post-G21/G22) */
    }
    if (g_anti_green && format == 0 /* YUV420P */ && width > 0 && eff_height > 0) {
        /* (Re-)allocate the cache when the dimensions change */
        if (g_glue.clean_w != width || g_glue.clean_h != eff_height) {
            free(g_glue.clean_y); free(g_glue.clean_u); free(g_glue.clean_v);
            g_glue.clean_ys = linesize_y;
            g_glue.clean_us = linesize_u;
            g_glue.clean_vs = linesize_v;
            g_glue.clean_y = (uint8_t *)malloc((size_t)linesize_y * eff_height);
            g_glue.clean_u = (uint8_t *)malloc((size_t)linesize_u * (eff_height/2));
            g_glue.clean_v = (uint8_t *)malloc((size_t)linesize_v * (eff_height/2));
            g_glue.clean_w = width;
            g_glue.clean_h = eff_height;
            /* The three planes go together: if one is missing we free them all
             * and the anti-green pass disables itself cleanly for this
             * resolution. Only clean_y used to be tested, while the memset
             * below writes into clean_u and clean_v TOO - so a partial
             * allocation wrote to address zero. */
            if (!g_glue.clean_y || !g_glue.clean_u || !g_glue.clean_v) {
                free(g_glue.clean_y); free(g_glue.clean_u); free(g_glue.clean_v);
                g_glue.clean_y = g_glue.clean_u = g_glue.clean_v = NULL;
                g_glue.clean_w = g_glue.clean_h = 0;   /* retente au prochain passage */
            }
            if (g_glue.clean_y) {
                /* F22.3 2026-05-23 09h30: initialise to uniform BLACK (Y=0,
                 * U=128, V=128). Before: initialising from the current data_y
                 * copied the green of the first frame, so replacing green with
                 * green was a no-op. Black is less jarring than green for the
                 * areas that never get updated with a clean MB. */
                memset(g_glue.clean_y, 0,   (size_t)linesize_y * eff_height);
                memset(g_glue.clean_u, 128, (size_t)linesize_u * (eff_height/2));
                memset(g_glue.clean_v, 128, (size_t)linesize_v * (eff_height/2));
            }
        }
        if (g_glue.clean_y && g_glue.clean_u && g_glue.clean_v) {
            /* Mutable buffers (= we overwrite libavcodec's output in place) */
            uint8_t *my = (uint8_t *)data_y;
            uint8_t *mu = (uint8_t *)data_u;
            uint8_t *mv = (uint8_t *)data_v;
            int mb_w = width / 16;
            int mb_h = eff_height / 16;
            for (int my_idx = 0; my_idx < mb_h; my_idx++) {
                for (int mx_idx = 0; mx_idx < mb_w; mx_idx++) {
                    /* Mean Y sampled over the MB (= 16x16, corner-sampled) */
                    int y_off = (my_idx * 16) * linesize_y + (mx_idx * 16);
                    int sum_y = 0;
                    for (int dy = 0; dy < 16; dy += 4) {
                        for (int dx = 0; dx < 16; dx += 4) {
                            sum_y += my[y_off + dy * linesize_y + dx];
                        }
                    }
                    int y_mean = sum_y / 16;
                    /* F22.2 2026-05-23 01h00: the pure-green signature
                     * RGB(0,135,0) = YUV BT.601 ~= (Y=79, U=83, V=71). Window
                     * widened to cover the variations. */
                    int u_off_check = (my_idx * 8) * linesize_u + (mx_idx * 8);
                    int v_off_check = (my_idx * 8) * linesize_v + (mx_idx * 8);
                    int u_val = mu[u_off_check + 4 * linesize_u + 4];
                    int v_val = mv[v_off_check + 4 * linesize_v + 4];
                    bool is_green = (y_mean >= 50 && y_mean <= 110) &&
                                    (u_val >= 50 && u_val <= 110) &&
                                    (v_val >= 40 && v_val <= 100);
                    /* Also catches libavcodec's pure black/grey default fill */
                    bool is_dark  = (y_mean < 12);
                    /* F22.3 stats: count the replaced MBs, for debugging */
                    static int g_replace_count = 0;
                    static int g_clean_count = 0;
                    if (is_green || is_dark) {
                        g_replace_count++;
                        if ((g_replace_count % 5000) == 1) {
                            gllog("[F22] replaced=%d clean=%d ratio=%.1f%% (sample Y=%d U=%d V=%d)",
                                       g_replace_count, g_clean_count,
                                       g_replace_count * 100.0 / (g_replace_count + g_clean_count + 1),
                                       y_mean, u_val, v_val);
                        }
                        /* Replace this MB from the clean cache */
                        int u_off = (my_idx * 8) * linesize_u + (mx_idx * 8);
                        int v_off = (my_idx * 8) * linesize_v + (mx_idx * 8);
                        for (int dy = 0; dy < 16; dy++)
                            memcpy(my + y_off + dy * linesize_y,
                                   g_glue.clean_y + y_off + dy * linesize_y, 16);
                        for (int dy = 0; dy < 8; dy++) {
                            memcpy(mu + u_off + dy * linesize_u,
                                   g_glue.clean_u + u_off + dy * linesize_u, 8);
                            memcpy(mv + v_off + dy * linesize_v,
                                   g_glue.clean_v + v_off + dy * linesize_v, 8);
                        }
                    } else {
                        /* Clean MB -> update the cache */
                        g_clean_count++;
                        int u_off = (my_idx * 8) * linesize_u + (mx_idx * 8);
                        int v_off = (my_idx * 8) * linesize_v + (mx_idx * 8);
                        for (int dy = 0; dy < 16; dy++)
                            memcpy(g_glue.clean_y + y_off + dy * linesize_y,
                                   my + y_off + dy * linesize_y, 16);
                        for (int dy = 0; dy < 8; dy++) {
                            memcpy(g_glue.clean_u + u_off + dy * linesize_u,
                                   mu + u_off + dy * linesize_u, 8);
                            memcpy(g_glue.clean_v + v_off + dy * linesize_v,
                                   mv + v_off + dy * linesize_v, 8);
                        }
                    }
                }
            }
        }
    }

    /* Dump key frames for visual analysis */
    static const char *dump_env = NULL;
    static int dump_checked = 0;
    if (!dump_checked) { dump_env = getenv("SHADOW_FRAME_DUMP"); dump_checked = 1; }
    if (dump_env && atoi(dump_env) == 1) {
        uint32_t n = g_glue.frames_displayed;
        if (n==1||n==300||n==1000||n==2000||n==3000||n==4000||n==5000) {
            dump_frame_yuv_to_ppm(width, eff_height, data_y, linesize_y,
                                    data_u, linesize_u, data_v, linesize_v,
                                    format, n);
        }
    }
    /* Fingerprint of the decoded LUMA, one per frame. Compared against the
     * same stream decoded offline, the first divergence says exactly at which
     * frame the LIVE decode parts ways with the offline one - which is where
     * the user sees the corruption set in. SHADOW_FRAME_HASH=1. */
    {
        static int hchk=0, hon=0; if(!hchk){hchk=1; const char*e=getenv("SHADOW_FRAME_HASH"); hon=(e&&atoi(e)==1);}
        if (hon) {
            uint64_t h=1469598103934665603ULL;
            for (int r=0; r<eff_height; r++) {
                const uint8_t* row=data_y+(size_t)r*linesize_y;
                for (int c=0;c<width;c++){ h^=row[c]; h*=1099511628211ULL; }
            }
            gllog("[HASH] n=%u luma=%016llx w=%d h=%d",
                       g_glue.frames_displayed, (unsigned long long)h, width, eff_height);
        }
    }

    if (g_frame_sink) {
        g_frame_sink(width, eff_height, data_y, linesize_y,
                     data_u, linesize_u, data_v, linesize_v,
                     format, pts, user);
    } else {
        /* SAID, once. A client that forgot to register a sink gets a session
         * that decodes correctly and displays nothing - the single most
         * confusing outcome available, and indistinguishable from a decoder
         * problem without this line. */
        static int said = 0;
        if (!said) {
            said = 1;
            gllog("[LIB1] no frame sink registered: %dx%d decoded and DROPPED. "
                  "Call ctrl_session_glue_set_frame_sink() before "
                  "ctrl_session_glue_run()", width, eff_height);
        }
    }
}

/* === AUD-INS-3 2026-09-11 - SHADOW_AUDIO_STATS_TICK ===
 * 1 (default): the audio counters reach the panel from on_audio, at most every
 * 250 ms, on the thread that decodes them - they keep moving through a video
 * outage. 0: from glue_decode_frame again, once every 30 decoded VIDEO frames:
 * frozen through an outage, the behaviour measured before this change (and
 * the decode thread copies a->stats while the session thread writes it, as it
 * did). Either way through a group merge. The variable is an env cache, static
 * on purpose; the session's copy is g_glue.aud_stats_tick. */
static int glue_audio_stats_tick(void)
{
    static int g_aud_stats_tick = -1;
    if (g_aud_stats_tick < 0) {
        const char *e = getenv("SHADOW_AUDIO_STATS_TICK");
        g_aud_stats_tick = e ? atoi(e) : 1;
    }
    return g_aud_stats_tick;
}

/* AUD-INS-3 - the AUDIO group of the session stats, as the decoder counts it. Fills
 * only that group's fields; the caller merges SESSION_STATS_AUDIO. */
static void glue_fill_audio_group(session_stats_t *pub)
{
    audio_stats_t as;
    audio_decoder_get_stats(g_glue.audio, &as);
    pub->opus_packets   = as.packets_received;
    pub->opus_decoded   = as.frames_decoded;
    pub->opus_pushed    = as.buffers_pushed;
    pub->opus_errors    = as.decode_errors;
    pub->opus_invalid   = as.invalid_packets;
    pub->opus_ring_full = as.buffers_dropped;
}

/* === G25 - DECODE + POST-PROCESSING (runs on the decode thread) ===
 * Split out of on_video to decouple decoding (slow) from reception (real
 * time). Holds the libavcodec feed, the stall/drift detection (G13/G18) and
 * the stats publication. */
static void glue_decode_frame(const uint8_t *nal_data, size_t len, uint32_t pts_90k,
                              uint32_t estampille_srv) {
    if (!g_glue.h264 || !nal_data || len == 0) return;
    /* === L5 2026-08-29 - `pts` IS A KEY, NOT A VALUE ===
     * The decoder carries exactly one field from input to output: `pts`. So we
     * record the pair (pts, server stamp) here and the renderer looks the
     * stamp up on the other side. We never READ `pts` as a duration: its value
     * went through `pts_ms = ts / 90`, a division whose unit is doubtful (the
     * stamp looks like microseconds, not 90 kHz ticks). Using it as a key
     * makes the measurement immune to that division, and to any future
     * correction of it. */
    if (latency_enabled())
        latency_video_into_decoder((int64_t)pts_90k, estampille_srv);
    const int64_t t_dec0 = latency_enabled() ? latency_now_us() : 0;
    h264_decoder_feed_annexb(g_glue.h264, nal_data, len, pts_90k);
    /* Duration of `send_packet` + `receive_frame` + the hardware transfer, as
     * the decode thread experiences it. This is WORK, not waiting: measuring
     * it tells us which part of the budget is incompressible, and lets us
     * compare hardware and software objectively on the same scene. */
    if (t_dec0)
        latency_add(LAT_VID_DECODE, latency_now_us() - t_dec0);

    /* === G13 2026-08-22 - GETTING OUT OF A FREEZE ===
     *
     * Reported symptom, confirmed on the panel: "decoded" drops to zero while
     * "received" keeps climbing. That is how libavcodec behaves once it has
     * lost its reference - it SILENTLY discards every frame that depends on
     * it, with no error and no log, until the next key frame. So the freeze
     * lasts as long as the server takes to send one on its own, which is
     * several seconds.
     *
     * G12 removed the pointless key-frame requests - the ones triggered by a
     * queue running late - and in doing so removed what used to break the
     * freeze by accident. The right trigger is neither loss nor queue depth:
     * it is the decoder producing nothing while we keep feeding it. So we only
     * ask then, which is rare and justified. */
    {
        h264_decoder_stats_t hs;
        h264_decoder_get_stats(g_glue.h264, &hs);

        /* === G18 - REFERENCE DRIFT: ask for one targeted IDR ===
         *
         * When libavcodec reports bitstream errors (corrupt macroblock, frame
         * num gap, concealment), the reference picture is compromised and the
         * degradation propagates until the next IDR. We ask for one, but rate
         * limited to one every ~1.2 s: a burst of errors spread over a whole
         * GOP then triggers a SINGLE IDR, and we give it time to take effect.
         * Precise (only fires on real corruption), so without the quality cost
         * of G12, which fired on every incomplete frame. */
        /* === G37 2026-08-22 - G18 (IDR on libavcodec errors) OFF by default ===
         *
         * Decisive measurement (real GUI session, webrtc_prev.log, 185 s):
         * lost=0/467k, abandoned=0, skip=0, 100% ok - NO real loss at all - and
         * yet idr_t climbs to ~1.2 IDR/s (228 IDRs in 185 s). Cause:
         * `g_bitstream_errors` is CUMULATIVE (never reset) and the NVDEC (CUDA)
         * path logs "decode_slice_header error" / "Frame num change" at ERROR
         * level on a stream that is nonetheless VALID - the SAME stream decoded
         * in software (ffmpeg, offline) = 0 errors. G18 counted those PHANTOM
         * errors and asked for an IDR every 1.2 s -> an ever-increasing IFR
         * counter (`69 50 00 02 [ctr++]`) = exactly the autofocus cycle G5 had
         * removed (the server does not dedup an increasing counter -> a coarse
         * keyframe every time). Worse: an IDR resets frame_num to 0 -> "Frame
         * num change" -> another error -> a SELF-SUSTAINING LOOP. Recovery from
         * REAL loss is already covered by G26 (loss/abandoned) and G36 (subchan
         * gap), which stay silent when lost=0. So we keep the COUNTING
         * (diagnostics) but no longer trigger an IDR from it by default.
         * `SHADOW_IDR_ON_BITSTREAM_ERR=1` restores the old behaviour (debug
         * only, or a software decoder under heavy loss). */
        static int g_idr_on_bserr = -1;
        if (g_idr_on_bserr < 0) {
            const char *e = getenv("SHADOW_IDR_ON_BITSTREAM_ERR");
            g_idr_on_bserr = e ? atoi(e) : 0;
        }
        static uint32_t derniere_err = 0;
        static int64_t  dernier_idr_ms = 0;
        if (g_idr_on_bserr && hs.bitstream_errors != derniere_err) {
            struct timespec dts; clock_gettime(CLOCK_MONOTONIC, &dts);
            int64_t now = (int64_t)dts.tv_sec * 1000 + dts.tv_nsec / 1000000;
            if (now - dernier_idr_ms >= 1200) {
                gllog("[G18] drift detected (%u bitstream errors) - requesting an IDR",
                           hs.bitstream_errors);
                ctrl_session_request_idr();
                dernier_idr_ms = now;
            }
            derniere_err = hs.bitstream_errors;
        }

        if (idr_stall_request(&g_glue.dec_stall, hs.frames_decoded, 15)) {
            /* Fifteen frames fed without a single one coming out: at 60 per
             * second that is a quarter of a second frozen, far beyond a mere
             * queueing delay.
             *
             * S34 2026-08-25: we REPEAT the request every 15 feeds for as long
             * as nothing comes out, instead of firing only on the strict
             * equality `== 15`. One request is enough when the decoder has
             * merely lost its reference, but not when it has NEVER started: on
             * joining, the server carries on with its GOP and sends neither SPS
             * nor a keyframe until we ask for one. If that single request is
             * lost or arrives too early, the screen stays black for the whole
             * session. Measured: 18 sessions out of 20 without a single
             * picture, at a normal inbound bitrate. */
            gllog("[G13] decoder stalled (%u pictures fed with no output) - "
                       "key-frame request", g_glue.dec_stall.stalled);
            ctrl_session_request_idr();
        }
    }

    /* Decode counters for the metrics panel: the decoder keeps them, but on
     * the native path nobody was republishing them - they stayed at zero, just
     * like the network ones. Once per second of video is enough; publishing on
     * every frame would take the lock 30 times more often for a display that
     * only refreshes twice a second. */
    /* CONC-4 - the cadence is session state (g_glue), it was a function static. */
    /* === AUD-INS-3 2026-09-11 - THIS THREAD WRITES THE VIDEO GROUP, AND ONLY IT ===
     * This block was a whole-struct get / modify / publish, and the ONLY writer
     * of the audio counters. The session loop published its own whole-struct
     * copy at 4 Hz, so each thread could write back the other's STALE fields:
     * a counter stepping back, which RateMeter reads as a restart (L21). And
     * since it runs once every 30 VIDEO frames, a video outage froze the audio
     * counters while the sound played - the gauge read 0 from ~2.8 s into the
     * outage, on 81 % of its draws (real-time replay with the real libopus and
     * RateMeter), where KB S41 measured 200 packets/s of real audio through a
     * 143.9 s outage. The audio group now leaves from on_audio, on the thread
     * that decodes it. SHADOW_AUDIO_STATS_TICK=0 brings it back here, on the
     * video cadence - still through the merge, so the lost update stays fixed. */
    if (g_glue.pub_countdown++ % 30 == 0) {
        h264_decoder_stats_t hs;
        h264_decoder_get_stats(g_glue.h264, &hs);
        session_stats_t pub;
        memset(&pub, 0, sizeof pub);
        unsigned groups = SESSION_STATS_VIDEO;
        pub.h264_frames_decoded = hs.frames_decoded;
        pub.h264_decode_errors  = hs.decode_errors;
        /* CONC-4 - the decode-queue drops reach the panel. The row existed and
         * read a field nothing wrote; the counter existed and only its own log
         * line read it. */
        pub.dec_queue_dropped   = __atomic_load_n(&g_glue.dec_dropped, __ATOMIC_RELAXED);
        if (!g_glue.aud_stats_tick && g_glue.audio) {
            glue_fill_audio_group(&pub);   /* the cadence before AUD-INS-3 */
            groups |= SESSION_STATS_AUDIO;
        }
        session_stats_merge(&pub, groups);
    }
}

/* === G25 - DECODE THREAD: dequeues and decodes, off the receive thread. ===
 * pthread_cond_timedwait at 100 ms = the abort granularity (Switch constraint:
 * every long-lived thread must poll its abort flag at <=100 ms). The decoder is
 * only destroyed after this thread is joined (see teardown), so g_glue.h264 is
 * valid for its whole lifetime. */
static void *dec_thread_fn(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_glue.dec_mtx);
        while (g_glue.decq_head == g_glue.decq_tail && !g_glue.dec_abort) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100 * 1000 * 1000;   /* 100 ms */
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_glue.dec_cv, &g_glue.dec_mtx, &ts);
        }
        if (g_glue.dec_abort) { pthread_mutex_unlock(&g_glue.dec_mtx); break; }
        uint8_t *data = g_glue.decq[g_glue.decq_head].data;
        size_t   len  = g_glue.decq[g_glue.decq_head].len;
        uint64_t pts  = g_glue.decq[g_glue.decq_head].pts_90k;
        long long t_recv = g_glue.decq[g_glue.decq_head].t_recv_ms;   /* L1 */
        uint32_t est_srv = g_glue.decq[g_glue.decq_head].estampille_srv;  /* L5 */
        int64_t  t_asm   = g_glue.decq[g_glue.decq_head].t_asm_us;        /* L5 */
        g_glue.decq[g_glue.decq_head].data = NULL;
        g_glue.decq_head = (g_glue.decq_head + 1) % g_decq_cap;
        g_glue.last_deq_ms = l1_now_ms();   /* CONC-4: reported by the [G38] line */
        pthread_mutex_unlock(&g_glue.dec_mtx);
        if (data) {
            /* L5 - time spent queued: frame assembled -> frame picked up. The
             * thread is woken by a condition variable, so in steady state this
             * stage costs one wake-up; it only grows when the decoder falls
             * behind, which is exactly what we want to see apart from the
             * decoding itself. */
            if (t_asm > 0)
                latency_add(LAT_VID_DEC_QUEUE, latency_now_us() - t_asm);
            glue_decode_frame(data, len, (uint32_t)pts, est_srv);
            free(data);
            /* === L1 - LATENCY "FRAME ASSEMBLED -> FRAME DECODED" ===
             * What the player actually feels is made of two waits: the time
             * spent queued plus the decode, measured here; then the wait to be
             * displayed, measured on the view side. We log them separately so
             * we know which one to cut, and so the render paths (software /
             * nvtegra+GL / deko3d) can be compared on measurements rather than
             * argued about. */
            /* CONC-4 - accumulators per session (g_glue), were function statics. */
            long long dt = l1_now_ms() - t_recv;
            g_glue.l1_sum += dt; if (dt > g_glue.l1_worst) g_glue.l1_worst = dt; g_glue.l1_n++;
            if ((g_glue.l1_n % 300) == 0) {
                gllog("[L1] latence decodage : avg=%lld ms worst=%lld ms "
                           "(over %u pictures, queue=%d)",
                           g_glue.l1_sum / g_glue.l1_n, g_glue.l1_worst, g_glue.l1_n, g_decq_cap);
                g_glue.l1_sum = 0; g_glue.l1_worst = 0; g_glue.l1_n = 0;
            }
        }
    }
    return NULL;
}

/* on_video (native-path callback): ENQUEUES for the decode thread, or decodes
 * synchronously when that thread is disabled (SHADOW_DECODE_THREAD=0). */
static void on_video(const uint8_t *nal_data, size_t len, uint64_t pts_ms,
                     bool is_keyframe, void *user) {
    (void)user;
    if (!nal_data || len == 0) return;
    uint32_t pts_90k = (uint32_t)(pts_ms * 90);
    /* L5 - the server timestamp of THIS frame. `flush_display_buffer` has just
     * published it and calls us synchronously, so the value belongs to the
     * frame we are being handed, not to some other one. */
    const uint32_t est_srv = latency_enabled() ? latency_video_current_stamp() : 0;
    const int64_t  t_asm   = latency_enabled() ? latency_video_asm_now_us() : 0;
    if (!g_glue.dec_thread_started) {           /* repli synchrone */
        glue_decode_frame(nal_data, len, pts_90k, est_srv);
        return;
    }
    /* === CONC-4 2026-09-11 - NOTHING SLOW UNDER THE QUEUE LOCK ===
     * The copy is made BEFORE taking dec_mtx, and an evicted buffer is freed
     * and logged AFTER releasing it: an allocation, a 30-100 KB memcpy, a free
     * and a log line (which takes the journal's own lock) all ran under the
     * lock the decode thread needs to pick up its next picture. If the copy
     * cannot be made, nothing is enqueued and nothing is evicted (it used to
     * evict first and then fail to enqueue). */
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return;
    memcpy(buf, nal_data, len);
    const long long now = l1_now_ms();
    uint8_t  *evicted = NULL;
    bool      ev_key = false, should_log = false;
    long long ev_age = 0, since_deq = -1, since_enq = -1;
    uint32_t  dropped = 0;

    pthread_mutex_lock(&g_glue.dec_mtx);
    int next = (g_glue.decq_tail + 1) % g_decq_cap;
    if (next == g_glue.decq_head) {
        /* Queue full: we drop the OLDEST frame to bound the latency. It fills
         * on ARRIVAL bursts - ten pictures before the first dequeue at start,
         * a jitter episode mid-session - not because decoding falls behind:
         * [L5] video/file-dec p50 and p90 are 0.0 in steady state (CONC-4). */
        dec_item_t *old = &g_glue.decq[g_glue.decq_head];
        evicted   = old->data;
        ev_key    = old->key;
        ev_age    = now - old->t_recv_ms;
        since_deq = g_glue.last_deq_ms < 0 ? -1 : now - g_glue.last_deq_ms;
        since_enq = g_glue.last_enq_ms < 0 ? -1 : now - g_glue.last_enq_ms;
        old->data = NULL;
        g_glue.decq_head = (g_glue.decq_head + 1) % g_decq_cap;
        dropped = __atomic_add_fetch(&g_glue.dec_dropped, 1, __ATOMIC_RELAXED);
        /* G38: trace the overflows (diagnosis of the residual smear). CONC-4:
         * the budget is per SESSION - a process-wide static spent its 8 lines
         * by 57 s of the first session - and the line reports FACTS (key frame
         * or not, age of the dropped picture, time since the decode thread
         * last dequeued, time since the previous arrival) instead of a cause
         * it could not know: "decode/render contention, not network loss"
         * was wrong for the startup burst. */
        if (g_glue.drop_log < 8 || (dropped % 16) == 0) {
            should_log = true;
            g_glue.drop_log++;
        }
        /* G34 2026-08-22 - a frame DROPPED here breaks the decoder's reference
         * chain (the following P-frames reference it) -> drift/smear that never
         * corrects itself. So we ask for a clean IDR to resynchronise. This is
         * the GUI case (decode+render not holding ~50 fps), which the headless
         * build does not reproduce. With the deep queue (G38) it became rare ->
         * no more IDR spiral. */
        ctrl_session_request_idr();
    }
    g_glue.decq[g_glue.decq_tail].data    = buf;
    g_glue.decq[g_glue.decq_tail].len     = len;
    g_glue.decq[g_glue.decq_tail].pts_90k = pts_90k;
    g_glue.decq[g_glue.decq_tail].t_recv_ms = now;             /* L1 */
    g_glue.decq[g_glue.decq_tail].estampille_srv = est_srv;  /* L5 */
    g_glue.decq[g_glue.decq_tail].t_asm_us       = t_asm;    /* L5 */
    g_glue.decq[g_glue.decq_tail].key            = is_keyframe;  /* CONC-4 */
    g_glue.decq_tail = next;
    g_glue.last_enq_ms = now;
    pthread_cond_signal(&g_glue.dec_cv);
    pthread_mutex_unlock(&g_glue.dec_mtx);

    free(evicted);
    if (should_log)
        gllog("[G38] decode queue FULL (cap=%d) - %u pictures dropped "
              "cle=%d age=%lld ms depuis_depile=%lld ms ecart_entree=%lld ms",
              g_decq_cap, dropped, ev_key ? 1 : 0, ev_age, since_deq, since_enq);
}

/* I3 2026-05-18 - hands every audio frame the session accepted to the
 * audio_decoder (Opus or FLAC, K14): decode, then ALSA / audout playback.
 *
 * AUD-INS-3 2026-09-11 - this comment used to say that ctrl_audio_dtls's
 * receive thread calls it. That socket is the INPUT channel (:base+12, KB
 * §3.37) and is off by default (SHADOW_AUDIO_DTLS=0, S11): the audio arrives
 * on :base+30, and this runs on the SESSION thread, from its receive loop - the
 * thread that writes a->stats, which the publication below relies on. Forcing
 * SHADOW_AUDIO_DTLS=1 feeds input echoes to the decoder from another thread;
 * that mode already races inside audio_decoder_feed itself. */
static void on_audio(const uint8_t *payload, size_t len, uint32_t rtp_ts,
                       void *user) {
    (void)user;
    if (!g_glue.audio || !payload || len == 0) return;
    audio_decoder_feed(g_glue.audio, payload, len, rtp_ts);
    /* === AUD-INS-3 2026-09-11 - THE AUDIO COUNTERS LEAVE FROM HERE, AT 4 Hz ===
     * On the thread that just wrote them, so the copy reads nothing another
     * thread is writing (the decode thread used to copy a->stats while this
     * one incremented it). 250 ms is the session loop's NET cadence; RateMeter
     * needs a change within 3 s not to read a real zero. aud_pub_ms starts at
     * 0, so the session's first frame publishes at once - wanted here: this is
     * a copy under a lock, nothing is sent (the "interval counter at 0 fires
     * immediately" trap of CLAUDE.md is about emissions). The last < 250 ms of
     * a channel that dies stay unpublished. No glue lock is taken here, so
     * g_glue_pub_mtx is never nested inside g_stats_mtx. */
    if (g_glue.aud_stats_tick) {
        const long long now = l1_now_ms();
        if (now - g_glue.aud_pub_ms >= 250) {
            session_stats_t pub;
            memset(&pub, 0, sizeof pub);
            glue_fill_audio_group(&pub);
            session_stats_merge(&pub, SESSION_STATS_AUDIO);
            g_glue.aud_pub_ms = now;
        }
    }
}

/* CUR1 2026-05-18 - cursor callback: count + parse + dump, to identify the
 * format. The format was unknown at the time. Heuristics tried:
 *   - PNG: starts with `89 50 4e 47`
 *   - JPEG: starts with `ff d8 ff`
 *   - Raw bitmap: `[width u16][height u16][hotspot_x u16][hotspot_y u16][BGRA pixels]`
 *   - Position update: `[type u8=...][x u16][y u16][shape_id u32]`
 *
 * SHADOW_DUMP_CURSOR=1 writes /tmp/cursor_NNN.bin (the first 10 frames).
 * Always: log the first 32 bytes in hex for the first 5 frames. */
static void on_cursor(const uint8_t *payload, size_t len, void *user) {
    (void)user;
    static uint32_t g_count = 0;
    static int g_dump_check = 0;
    static int g_dump_on = 0;
    if (!g_dump_check) {
        const char *e = getenv("SHADOW_DUMP_CURSOR");
        g_dump_on = (e && atoi(e) == 1) ? 1 : 0;
        g_dump_check = 1;
    }
    g_count++;

    /* Log the first 5 frames in hex (= 32-byte head, to identify the format) */
    if (g_count <= 5 && payload && len > 0) {
        char hex[200]; int ho = 0;
        size_t hl = len < 32 ? len : 32;
        for (size_t i = 0; i < hl && ho < (int)sizeof(hex) - 4; i++) {
            ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x ", payload[i]);
        }
        const char *fmt = "raw";
        if (len >= 4 && payload[0] == 0x89 && payload[1] == 0x50
            && payload[2] == 0x4e && payload[3] == 0x47) fmt = "PNG";
        else if (len >= 3 && payload[0] == 0xff && payload[1] == 0xd8
                 && payload[2] == 0xff) fmt = "JPEG";
        else if (len >= 8) {
            /* Try to read it as a bitmap header */
            uint16_t w = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            uint16_t h = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
            if (w > 0 && w <= 256 && h > 0 && h <= 256) {
                size_t bgra = (size_t)w * h * 4;
                if (len == 8 + bgra) fmt = "BMP-BGRA-8B-hdr";
                else if (len == 4 + bgra) fmt = "BMP-BGRA-4B-hdr";
            }
        }
        gllog("[cursor] frame #%u fmt=%s len=%zu hex32=%s",
                   g_count, fmt, len, hex);
    }

    /* Dump the first 10 frames raw when enabled */
    if (g_dump_on && g_count <= 10 && payload && len > 0) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/cursor_%03u.bin", g_count);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(payload, 1, len, f); fclose(f); }
    }

    /* CUR1 phase 2: feed the cursor state, for the Borealis overlay renderer */
    cursor_state_feed(payload, len);
}

static void on_progress(const char *step, const char *detail, void *user) {
    (void)user;
    /* CONC-4 - the periodic stats line carries the decode-queue drops: they
     * were counted where no reader of the log could see them. */
    /* AUD-INS-3 2026-09-11 - ' aud_pub=' is the audio frame count AS THE PANEL
     * SEES IT (the session stats, which StreamView samples), beside the session's
     * own 'audio='. A frozen aud_pub while audio= climbs is the defect, readable
     * in a log with nobody opening the panel. Appended last: no key moves. */
    if (step && strcmp(step, "stats") == 0) {
        session_stats_t ws;
        session_stats_get(&ws);
        gllog("[glue] %s — %s jetees=%u aud_pub=%u", step, detail ? detail : "",
              __atomic_load_n(&g_glue.dec_dropped, __ATOMIC_RELAXED), ws.opus_decoded);
    } else
        gllog("[glue] %s — %s", step ? step : "?", detail ? detail : "");
}

void ctrl_session_glue_set_udp_register_input(int on)
{
    ctrl_session_set_udp_register_input(on);
}

/* === AF8 2026-09-10 - THE HUD READS THE DECODERS UNDER A LOCK ===
 *
 * These three are called from the UI thread (the stream's info panel) while
 * the session thread may be tearing the decoders down. They tested the pointer
 * and then dereferenced it, with nothing to stop the teardown destroying the
 * object in between. A few instructions wide and never observed - but it is
 * exactly the shape rule 2 exists for. The teardown now unpublishes under this
 * lock and destroys OUTSIDE it: a destroy joins threads and closes devices, and
 * no I/O may run under a lock (rule 3). The names returned are static strings,
 * so they outlive the lock. */
static pthread_mutex_t g_glue_pub_mtx = PTHREAD_MUTEX_INITIALIZER;

const char *ctrl_session_glue_codec_audio(void)
{
    pthread_mutex_lock(&g_glue_pub_mtx);
    const char *s = g_glue.audio ? audio_decoder_codec_name(g_glue.audio) : "—";
    pthread_mutex_unlock(&g_glue_pub_mtx);
    return s;
}

const char *ctrl_session_glue_codec(void)
{
    pthread_mutex_lock(&g_glue_pub_mtx);
    const char *s = g_glue.h264 ? h264_decoder_codec_name(g_glue.h264) : "—";
    pthread_mutex_unlock(&g_glue_pub_mtx);
    return s;
}

int ctrl_session_glue_hw(void)
{
    pthread_mutex_lock(&g_glue_pub_mtx);
    const int hw = g_glue.h264 ? h264_decoder_hw_active(g_glue.h264) : 0;
    pthread_mutex_unlock(&g_glue_pub_mtx);
    return hw;
}

bool ctrl_session_glue_run(const ctrl_session_glue_params *p,
                              ctrl_session_glue_stats *out_stats) {
    if (!p || !p->vm_host) return false;

    memset(&g_glue, 0, sizeof(g_glue));
    g_glue.last_deq_ms = g_glue.last_enq_ms = -1;   /* CONC-4: 0 would read as "since boot" */
    g_glue.aud_stats_tick = glue_audio_stats_tick(); /* AUD-INS-3: before any thread starts */
    g_glue.h264 = h264_decoder_create(on_frame, NULL);
    if (!g_glue.h264) {
        gllog("[glue] h264_decoder_create FAIL");
        return false;
    }

    /* === G25 2026-08-22 - start the dedicated DECODE THREAD === */
    g_glue.decq_head = g_glue.decq_tail = 0;
    g_glue.dec_dropped = 0;
    /* G38: effective queue depth, bounded by the array. Was 64 (~1.3 s at
     * 50 fps in the WORST case, though the queue drains in steady state).
     * The ring is full when `next == head`: cap=N holds N-1 queued frames plus
     * the one being decoded. */
    if (g_decq_cap == 0) {
        const char *e = getenv("SHADOW_DEC_QUEUE");
        /* === L1 2026-08-22 - SHALLOWER QUEUE: LATENCY WINS ===
         * G38's 64 frames were compensating for a decoder at 5 s per frame (the
         * broken envideo path). With nvtegra hardware decoding (1-2 ms per
         * frame) the decoder never falls behind any more: a deep queue only
         * ACCUMULATES delay - up to ~1.3 s at 48 frames/s. For gaming it is
         * better to drop a stale frame than to show it late. 3 frames = one
         * spike absorbed, ~60 ms of slack at worst.
         *
         * === CONC-1 2026-09-11 - 16: THE QUEUE ONLY FILLS ON ARRIVAL BURSTS ===
         * L1's premise holds - the decoder keeps up - but its conclusion did
         * not follow: a queue the decoder keeps empty adds NO delay, and "3"
         * held 2 frames, not 3. What fills it is ARRIVAL: at every session
         * start the video socket collects ~0.17 s of pictures while the side
         * channels open, and they reach the queue back to back (`ecart_entree=0`
         * on the [G38] line); mid-session, a 50-70 ms decoder stall. Each drop
         * breaks the reference chain until a requested IDR - and the evicted
         * oldest frame can be the key frame itself.
         * Live A/B on desktop, 5 sessions of 130 s per depth: depth 3 -> 24
         * drops (16 at startup in 3 sessions, 8 mid-session in 2, one KEY
         * frame); depth 8 -> 59, all at startup (one 1.4 s backlog when a side
         * channel took 1.2 s to open, then 2); depth 16 -> 0. L1's own guard did
         * not move: [L5] video/file-dec p50 0.10 ms at every depth, p90 2.8 /
         * 2.7 / 2.4 ms, video/bout +p99 30.7 ms at every depth. Desktop only:
         * the console (Wi-Fi bursts, nvtegra) has yet to confirm it.
         * SHADOW_DEC_QUEUE=3 restores L1. */
        g_decq_cap = e ? atoi(e) : 16;
        /* L2: floor lowered from 8 to 2. G38's floor dated from the slow
         * decoder and SILENTLY OVERRODE the requested value (measured:
         * "queue=8" while the default was 3). A guard rail that contradicts the
         * setting without saying so is worse than no guard rail. */
        if (g_decq_cap < 2) g_decq_cap = 2;
        if (g_decq_cap > DEC_QUEUE_CAP - 1) g_decq_cap = DEC_QUEUE_CAP - 1;
    }
    g_glue.dec_abort = false;
    g_glue.dec_thread_started = false;
    {
        static int dec_thread_on = -1;
        if (dec_thread_on < 0) {
            const char *e = getenv("SHADOW_DECODE_THREAD");
            dec_thread_on = e ? atoi(e) : 1;   /* G25 : defaut 1 */
        }
        if (dec_thread_on) {
            pthread_mutex_init(&g_glue.dec_mtx, NULL);
            pthread_cond_init(&g_glue.dec_cv, NULL);
            if (pthread_create(&g_glue.dec_thread, NULL, dec_thread_fn, NULL) == 0) {
                g_glue.dec_thread_started = true;
                gllog("[glue] fil de decodage demarre (G25)");
            } else {
                pthread_mutex_destroy(&g_glue.dec_mtx);
                pthread_cond_destroy(&g_glue.dec_cv);
                gllog("[glue] pthread_create fil decodage FAIL — repli synchrone");
            }
        }
    }
    /* EQV2 2026-09-11 - the equaliser's filter memory is process-global, and
     * the previous session's would otherwise play out in this one's first
     * buffer (a thump up to -8 dBFS in handheld). HERE rather than inside
     * audio_decoder_create: every native session comes through this line, the
     * decoder - hence the ALSA play thread - does not exist yet, and the
     * decoder's output-ownership code stays untouched. */
    audio_eq_session_start();

    /* I3 2026-05-18 - create the audio_decoder (Opus + ALSA/AudioOut).
     * Non-fatal on failure (= carry on with a silent video stream). */
    g_glue.audio = audio_decoder_create();
    if (!g_glue.audio) {
        gllog("[glue] audio_decoder_create FAIL — continuing without audio");
    } else {
        gllog("[glue] audio_decoder created OK");
    }

    /* I1 - tell the UI there is an input session: stream_view only posts mouse
     * and keyboard events while this is true. (It used to be a mock SCTP
     * pointer set to 0x1, from the days the WebRTC path decided the question.) */
    shadow_input_set_session_active(true);

    /* BUG3 2026-05-18 - spawn the input drain thread. Without it the GLFW
     * events posted by stream_view through shadow_input_post_* pile up in the
     * queue and never come out -> mouse and keyboard stay silent in native
     * mode. */
    g_glue.input_drain_stop = false;
    if (pthread_create(&g_glue.input_drain_thread, NULL, input_drain_thread_fn, NULL) == 0) {
        g_glue.input_drain_started = true;
        gllog("[glue] input drain thread started (100 Hz)");
    } else {
        gllog("[glue] input drain thread CREATE FAIL — mouse/keyboard silents");
    }

    /* CUR1 phase 2: initialise the cursor state singleton + set display dims */
    cursor_state_init();
    cursor_state_set_display(p->display_width  > 0 ? p->display_width  : 1920,
                              p->display_height > 0 ? p->display_height : 1080);

    ctrl_session_params params;
    memset(&params, 0, sizeof(params));
    params.vm_host         = p->vm_host;
    params.streaming_token = p->streaming_token;
    params.client_id       = p->client_id;
    params.bearer_jwt      = p->bearer_jwt;
    params.display_width   = p->display_width  > 0 ? p->display_width  : 1920;
    params.display_height  = p->display_height > 0 ? p->display_height : 1080;
    params.port_base       = p->port_base;
    params.max_bitrate_mbps = p->max_bitrate_mbps;  /* Q1 2026-05-18 */
    params.target_fps       = p->target_fps;
    params.video_tcp        = p->video_tcp;   /* B4 */
    params.on_video        = on_video;
    params.on_audio        = on_audio;   /* I3 2026-05-18 — DTLS → audio_decoder */
    params.on_cursor       = on_cursor;  /* CUR1 2026-05-18 — parse + dump */
    params.on_progress     = on_progress;
    params.user            = NULL;
    params.abort_flag      = p->abort_flag;

    ctrl_session_stats stats;
    memset(&stats, 0, sizeof(stats));
    bool ok = ctrl_session_run(&params, &stats);

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
        out_stats->bootstrap_ok    = stats.bootstrap_ok;
        out_stats->udp_video_pkts  = stats.udp_video_pkts;
        out_stats->udp_cursor_pkts = stats.udp_cursor_pkts;
        out_stats->udp_audio_bytes = stats.udp_audio_bytes;
        out_stats->udp_video_bytes = stats.udp_video_bytes;
        out_stats->frames_decoded  = stats.frames_decoded;
        out_stats->frames_displayed = g_glue.frames_displayed;
        out_stats->dec_dropped     = g_glue.dec_dropped;   /* CONC-4 */
        out_stats->decrypt_ok      = stats.decrypt_ok;
        out_stats->decrypt_fail    = stats.decrypt_fail;
        out_stats->nal_top         = stats.nal_top;
        out_stats->nal_bottom      = stats.nal_bottom;
        out_stats->nal_idr_top     = stats.nal_idr_top;
        out_stats->nal_idr_bottom  = stats.nal_idr_bottom;
        out_stats->parity_skip     = stats.parity_skip;
        out_stats->parity_decrypt_ok   = stats.parity_decrypt_ok;
        out_stats->parity_decrypt_fail = stats.parity_decrypt_fail;
        out_stats->reasm_abandoned = stats.reasm_abandoned;
        out_stats->chunks_missing         = stats.chunks_missing;
        out_stats->chunks_expected        = stats.chunks_expected;
        out_stats->frames_miss1           = stats.frames_miss1;
        out_stats->chunks_orphan          = stats.chunks_orphan;
        out_stats->chunks_orphan_dup      = stats.chunks_orphan_dup;
        out_stats->chunks_orphan_lost     = stats.chunks_orphan_lost;
        out_stats->chunks_orphan_stale    = stats.chunks_orphan_stale;
        out_stats->chunks_redundant       = stats.chunks_redundant;
        out_stats->frames_dropped_trunc   = stats.frames_dropped_trunc;
        out_stats->incomplete_at_flush    = stats.incomplete_at_flush;
        out_stats->nack_sent              = stats.nack_sent;
        out_stats->session_seconds = stats.session_seconds;
        out_stats->exit_reason     = stats.exit_reason;
    }

    /* BUG3 2026-05-18 - stop the drain thread BEFORE the session is marked
     * inactive (otherwise one last drain iteration can pop a command after the
     * input channel has already been freed). */
    if (g_glue.input_drain_started) {
        g_glue.input_drain_stop = true;
        pthread_join(g_glue.input_drain_thread, NULL);
        g_glue.input_drain_started = false;
    }
    /* === G25 - stop the decode thread BEFORE destroying the decoder ===
     * The order is critical (cf. BUG2): that thread is the only one feeding the
     * decoder; we join it before destroying, so there is no race on h264. */
    if (g_glue.dec_thread_started) {
        pthread_mutex_lock(&g_glue.dec_mtx);
        g_glue.dec_abort = true;
        pthread_cond_signal(&g_glue.dec_cv);
        pthread_mutex_unlock(&g_glue.dec_mtx);
        pthread_join(g_glue.dec_thread, NULL);
        g_glue.dec_thread_started = false;
        /* drain whatever is left in the queue */
        while (g_glue.decq_head != g_glue.decq_tail) {
            free(g_glue.decq[g_glue.decq_head].data);
            g_glue.decq[g_glue.decq_head].data = NULL;
            g_glue.decq_head = (g_glue.decq_head + 1) % g_decq_cap;
        }
        pthread_mutex_destroy(&g_glue.dec_mtx);
        pthread_cond_destroy(&g_glue.dec_cv);
    }
    /* AF8 - unpublished under the lock the HUD accessors take, destroyed
     * outside it (see the accessors above). */
    pthread_mutex_lock(&g_glue_pub_mtx);
    h264_decoder  *dead_h264  = g_glue.h264;
    audio_decoder *dead_audio = g_glue.audio;
    g_glue.h264  = NULL;
    g_glue.audio = NULL;
    pthread_mutex_unlock(&g_glue_pub_mtx);
    h264_decoder_destroy(dead_h264);
    /* HO-2 - the session's own G44 bilan, histogram included. Emitted only
     * here, after h264_decoder_destroy: that call joins the
     * SHADOW_ASYNC_TRANSFER worker, the other thread that can run on_frame, so
     * nothing writes g_glue.freeze any more. The periodic bilan fires every
     * 500 pictures, so without this line a session's tail - or a whole
     * session under 500 pictures - reported nothing. New line, new string:
     * the two existing [G44] strings are unchanged. */
    if (g_glue.freeze.nfr > 0)
        gllog("[G44] session summary: %u pictures, avg=%lld ms, max=%lld ms, "
              "gels(>=45ms)=%u | 45-99ms=%u 100-299ms=%u 300-999ms=%u >=1s=%u | "
              "%u ligne(s) micro-gel",
              g_glue.freeze.nfr, (long long)freeze_stat_mean_ms(&g_glue.freeze),
              (long long)g_glue.freeze.max_ms, g_glue.freeze.nfreeze,
              g_glue.freeze.hist[FREEZE_H_45_99], g_glue.freeze.hist[FREEZE_H_100_299],
              g_glue.freeze.hist[FREEZE_H_300_999], g_glue.freeze.hist[FREEZE_H_1000_UP],
              g_glue.freeze.logged);
    /* I3 2026-05-18 - clean up the audio_decoder (= libopus + ALSA close) */
    if (dead_audio) audio_decoder_destroy(dead_audio);
    /* I1 - the input session is over: stream_view stops posting. */
    shadow_input_set_session_active(false);
    return ok;
}
