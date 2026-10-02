// H.264 decoder — RTP reassembly + libavcodec decode. Cf. h264_decoder.h.

#include "h264_decoder.h"
#include "h264_decoder_vita.h"
#include "../services/config.h"
#include "../services/log.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* S81 - the category is DECLARED here, not inferred from the message text.
 * `wlog` stays at INFO: the existing calls do not disappear. `wdbg` is there for
 * the bulky lines, which move over to it one at a time. */
#define wlog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
#define wdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_VIDEO, __VA_ARGS__)

/* === COL1 2026-09-10 - THE COLORIMETRY THE STREAM DECLARES ===
 *
 * What the SPS's VUI says about the matrix and the range, as of the last
 * decoded picture, for the GL renderer to act on (it decides what "not
 * declared" means - see gl_video_renderer.cpp). Reset by `h264_decoder_create`,
 * i.e. per session: a stream that declares nothing must not inherit what the
 * previous one said. Atomics: written by the decoder thread, read by the UI
 * thread every frame. */
static int g_color_matrix = -1;   /* 601, 709, 2020; 0 not declared; -1 no picture yet */
static int g_color_full   = -1;   /* 1 full, 0 limited, -1 not declared */

static const char *color_matrix_name(int m)
{
    return m == 709 ? "BT.709" : m == 601 ? "BT.601" : m == 2020 ? "BT.2020"
                                                               : "non declaree";
}

static void publish_colorimetry(const AVFrame *f)
{
    int m = 0;
    switch (f->colorspace) {
    case AVCOL_SPC_BT709:                                 m = 709;  break;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M:     m = 601;  break;
    case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL:  m = 2020; break;
    default: break;   /* unspecified, or a matrix the shader does not model */
    }
    const int full = f->color_range == AVCOL_RANGE_JPEG ? 1
                   : f->color_range == AVCOL_RANGE_MPEG ? 0 : -1;
    if (m != __atomic_load_n(&g_color_matrix, __ATOMIC_RELAXED) ||
        full != __atomic_load_n(&g_color_full, __ATOMIC_RELAXED)) {
        wlog("[colour] the stream declares: matrix %s, range %s (colorspace=%d color_range=%d)",
             color_matrix_name(m),
             full == 1 ? "pleine" : full == 0 ? "limitee" : "non declaree",
             (int)f->colorspace, (int)f->color_range);
    }
    __atomic_store_n(&g_color_matrix, m, __ATOMIC_RELAXED);
    __atomic_store_n(&g_color_full, full, __ATOMIC_RELAXED);
}

void h264_decoder_colorimetry(int *matrix, int *full_range)
{
    if (matrix)     *matrix     = __atomic_load_n(&g_color_matrix, __ATOMIC_RELAXED);
    if (full_range) *full_range = __atomic_load_n(&g_color_full, __ATOMIC_RELAXED);
}
// Annex-B start code "\x00\x00\x00\x01" (4 bytes) - used to prefix every NAL
// unit before handing it to libavcodec.
static const uint8_t kAnnexBStartCode[4] = { 0, 0, 0, 1 };

/* A `kH264Extradata` array used to live here: a hand-built fallback SPS/PPS,
 * from the days of the RTP/WebRTC path when we believed Shadow sent none.
 * Removed on 2026-08-25: on the native path the server does send SPS (NAL 7) and
 * PPS (NAL 8) - logged at the start of every stream - and nobody read that array
 * any more. The real trap is elsewhere: the server only resends them ON THE
 * FIRST SESSION, so a key frame must be requested when reconnecting
 * (KB.md §3.28). */

// FU-A reassembly buffer - accumulates a NAL's fragments until the E flag.
// Access unit buffer - accumulates ALL the NALs of one picture (same PTS)
// before feeding them to libavcodec as ONE packet. Otherwise Shadow sends 2-3
// slices per frame, we fed them individually, and libavcodec output 2-3 frames
// per picture (each with only its own slice decoded and zeroes elsewhere) -> a
// scrolling horizontal band.
#define AU_BUF_MAX (1024 * 1024)  // 1 MB is enough for a 1080p high-profile IDR

// Async transfer worker queue: VIC detiling NVDEC->CPU costs 11 ms per frame.
// We move it onto a dedicated thread so it no longer blocks the receive loop
// (which has to serve video/audio/input at the same time).
#define VAW_QUEUE_CAP 4

/* Marker file for the sticky cross-session hwaccel disable (= consistent with
 * SHADOW_DATA_DIR in core/services/config.h). Matches the 3 platforms: Switch
 * SD, ephemeral Linux /tmp, Windows CWD-relative. */
#if defined(__SWITCH__)
#  define HW_MARKER_PATH "/switch/halyard/hwaccel.disabled"
#elif defined(_WIN32)
#  define HW_MARKER_PATH "./halyard-data/hwaccel.disabled"
#else
#  define HW_MARKER_PATH "/tmp/halyard/hwaccel.disabled"
#endif

struct video_async_worker {
    /* K18b - points at the decoder's flag: on console this thread is the ONLY
     * one that delivers hardware frames, hence the only place to set it. */
    bool           *hw_active;
    pthread_t       thread;
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    AVFrame        *queue[VAW_QUEUE_CAP];  // cloned hwframe refs (refcount bump)
    int             head, tail, count;
    bool            stop;
    bool            started;
    AVFrame        *sw_frame;              // worker-private CPU frame
    h264_frame_cb   on_frame;
    void           *user;
    uint32_t        frames_processed;
    uint32_t        frames_dropped;
};

struct h264_decoder {
#if defined(__vita__) || defined(__psp2__)
    /* PS Vita: the hardware decoder replaces libavcodec entirely - the SDK's
     * build has no H.264 at all (22 decoders, not that one). Everything below
     * stays declared so the file compiles unchanged; it is simply unused. */
    h264_vita      *vita;
#endif
    /* K15 - true when the expected stream is HEVC. The NAL parsing depends on
     * it: a two-byte header rather than one, and all types 0-31 are picture
     * slices. */
    int             hevc;
    AVCodec        *codec;
    AVCodecContext *ctx;
    AVPacket       *pkt;
    AVFrame        *frame;
    AVFrame        *sw_frame;         // for the hwframe -> CPU YUV420P transfer
    AVBufferRef    *hw_device_ctx;    // hardware device (envideo), NULL otherwise
    const AVCodecHWConfig *hw_cfg;    // S20: the decoder's chosen hardware config
    bool            hwaccel_active;

    // FU-A reassembly state

    // Access unit aggregation: a buffer of all of a picture's NALs, flushed on
    // a PTS change (= a new picture).
    uint8_t        *au_buf;
    size_t          au_len;
    int64_t         au_pts;
    bool            au_active;

    /* G39 2026-08-22 - cache of the last SPS/PPS (with start code), to prefix a
     * dumped offending AU (diagnostic) so it decodes standalone offline. */
    uint8_t         err_sps[128];
    size_t          err_sps_len;
    uint8_t         err_pps[128];
    size_t          err_pps_len;

    // Decoder gate: we only start feeding the decoder from an SPS (type 7) or
    // an IDR (type 5). Otherwise the first P-frames, with no SPS/PPS context,
    // put libavcodec into an error state and it refuses everything after.
    bool            ready_to_decode;

    // Hwaccel resilience: a fail-streak counter for the sticky auto-fallback.
    // More than HW_FAIL_THRESHOLD consecutive failures = we switch to the CPU
    // path for the rest of the session (= dead GPU, corrupted driver,
    // unsupported profile). Reset to 0 on every successful transfer.
    int             hw_fail_streak;
    bool            hw_fallback_done;  // log once on first fallback
    /* K18 - true as soon as a picture has REALLY been delivered by the
     * hardware. Set on delivery, not on open: the fallback is sticky (S28) and
     * intent proves nothing. This is what the panel must show. */
    bool            hw_active;
    /* S34 2026-08-25 - HEALTH CHECKS ON THE HARDWARE TRANSFER, PER DECODER.
     * They used to be `static`s inside hw_deliver_frame, hence capped once and
     * for all: from the 2nd session on, neither the EMPTY-surface detector (S18)
     * nor the SLOW-transfer one (S28) ran at all. A console whose hardware
     * degrades after a few sessions would therefore never have switched to
     * software decoding. Since the decoder is recreated for every session, these
     * fields start clean. */
    int             hw_check_seen;    /* pictures sampled (max 8) */
    int             hw_check_empty;   /* of which entirely black */
    int             hw_slow;          /* abnormally long transfers */

    // Callback + stats
    h264_frame_cb   on_frame;
    void           *user;
    h264_decoder_stats_t stats;

    // Async transfer worker (only used when hwaccel is active).
    struct video_async_worker async;
};

/* Sticky disable threshold: 5 consecutive hwframe_transfer failures = the GPU
 * is not in a usable state for this session. We disable the hwaccel path at
 * runtime and let the context fall back to its internal software path (or drop
 * the frame if the format stays GPU-side). Avoids spamming 200+ "transfer FAIL"
 * lines into the logs without helping the picture. */
#define HW_FAIL_THRESHOLD 5

// === Async transfer worker ===

/* === S14 2026-08-22 — IS HARDWARE ACCELERATION ALLOWED? ===
 *
 * This decision was written TWICE and inconsistently: the desktop path consulted
 * `SHADOW_HWACCEL` and the marker file, but the Switch block created the NVTEGRA
 * device **unconditionally, before** that logic. Result: on console, neither
 * `SHADOW_HWACCEL=0` nor the marker had the slightest effect - it was impossible
 * to test the software fallback, which was exactly what needed doing (the
 * `envideo` hardware transfer returns rc=0 but an EMPTY surface: luma measured
 * at 0 everywhere before the GPU). One function decides now, for both
 * platforms. */
static int hw_allowed(void)
{
    const char *e = getenv("SHADOW_HWACCEL");
    if (e) return atoi(e);            /* an explicit choice wins */
    FILE *mf = fopen(HW_MARKER_PATH, "rb");
    if (mf) {
        fclose(mf);
        wlog("h264: marker %s present -> SOFTWARE decoding "
             "(delete the file to go back to hardware)", HW_MARKER_PATH);
        return 0;
    }
    return 1;
}

static void *vaw_loop(void *arg) {
    struct video_async_worker *w = (struct video_async_worker *)arg;
    while (1) {
        pthread_mutex_lock(&w->mtx);
        while (!w->stop && w->count == 0) {
            pthread_cond_wait(&w->cond, &w->mtx);
        }
        if (w->stop && w->count == 0) {
            pthread_mutex_unlock(&w->mtx);
            break;
        }
        AVFrame *f = w->queue[w->head];
        w->head = (w->head + 1) % VAW_QUEUE_CAP;
        w->count--;
        pthread_mutex_unlock(&w->mtx);

        // The heavy work outside the mutex: NVDEC->CPU transfer (~11 ms VIC detiling).
        int trc = av_hwframe_transfer_data(w->sw_frame, f, 0);
        if (trc >= 0) {
            /* K18b - THE FLAG WAS SET ON THE WRONG PATH. It was only set in the
             * Linux/CUDA transfer branch; the Switch delivers its hardware
             * pictures through THIS asynchronous thread. The panel therefore
             * announced "software" while the log said "hwaccel materiel retenu :
             * nvtegra". A panel that lies about the decode mode is worse than no
             * panel at all. */
            if (w->hw_active) *w->hw_active = true;
            if (w->on_frame) {
                w->on_frame(w->sw_frame->width, w->sw_frame->height,
                            w->sw_frame->data[0], w->sw_frame->linesize[0],
                            w->sw_frame->data[1], w->sw_frame->linesize[1],
                            w->sw_frame->data[2], w->sw_frame->linesize[2],
                            w->sw_frame->format,
                            f->pts,
                            w->user);
            }
            av_frame_unref(w->sw_frame);
            w->frames_processed++;
        } else {
            char err[128] = {0};
            av_strerror(trc, err, sizeof(err));
            wlog("h264: async transfer FAIL rc=%d (%s)", trc, err);
        }
        av_frame_free(&f);
    }
    return NULL;
}

static bool vaw_init(struct video_async_worker *w,
                      h264_frame_cb cb, void *user) {
    memset(w, 0, sizeof(*w));
    w->on_frame = cb;
    w->user = user;
    /* Set AFTER vaw_init's memset, otherwise it would be wiped. */
    if (pthread_mutex_init(&w->mtx, NULL) != 0) return false;
    if (pthread_cond_init(&w->cond, NULL) != 0) {
        pthread_mutex_destroy(&w->mtx);
        return false;
    }
    w->sw_frame = av_frame_alloc();
    if (!w->sw_frame) {
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->mtx);
        return false;
    }
    if (pthread_create(&w->thread, NULL, vaw_loop, w) != 0) {
        av_frame_free(&w->sw_frame);
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->mtx);
        return false;
    }
    w->started = true;
    return true;
}

static void vaw_destroy(struct video_async_worker *w) {
    if (!w->started) return;
    pthread_mutex_lock(&w->mtx);
    w->stop = true;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->mtx);
    pthread_join(w->thread, NULL);
    while (w->count > 0) {
        AVFrame *f = w->queue[w->head];
        w->head = (w->head + 1) % VAW_QUEUE_CAP;
        w->count--;
        if (f) av_frame_free(&f);
    }
    if (w->sw_frame) av_frame_free(&w->sw_frame);
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->mtx);
    w->started = false;
}

#ifdef __SWITCH__   /* only hw_deliver_frame (Switch) feeds the worker */
// Push an hwframe to the worker. Dropped when the queue is full (low latency:
// we would rather skip a frame than block the receive loop).
static bool vaw_push(struct video_async_worker *w, AVFrame *src) {
    if (!w->started) return false;
    AVFrame *clone = av_frame_clone(src);  // refcount bump, no data copy
    if (!clone) return false;
    pthread_mutex_lock(&w->mtx);
    if (w->stop || w->count >= VAW_QUEUE_CAP) {
        pthread_mutex_unlock(&w->mtx);
        av_frame_free(&clone);
        w->frames_dropped++;
        return false;
    }
    w->queue[w->tail] = clone;
    w->tail = (w->tail + 1) % VAW_QUEUE_CAP;
    w->count++;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mtx);
    return true;
}
#endif /* __SWITCH__ */

// get_format callback: called by libavcodec after the first IDR to choose the
// output format. We want AV_PIX_FMT_NVTEGRA when listed (= hwaccel active),
// otherwise the first format by default (AV_PIX_FMT_YUV420P).
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                          const enum AVPixelFormat *pix_fmts) {
#ifndef __SWITCH__
    (void)ctx;   /* the surface pool is only created on the Switch path */
#endif
#ifdef __SWITCH__
    /* === S15 2026-08-22 — STOP COMPARING AGAINST THE AV_PIX_FMT_NVTEGRA CONSTANT ===
     *
     * The installed FFmpeg headers and the linked library are NOT from the same
     * build of the Averne fork: the header defines `AV_PIX_FMT_NVTEGRA = 228`
     * (the last format, NB=229), while the library exposes the hardware format
     * under number **243**, named "envideo" - a number that does not even exist
     * in our headers (verified: NV12=23 matches, so only the formats appended at
     * the end of the enum diverge).
     * Consequence: this comparison ALWAYS failed, we fell back to `pix_fmts[0]`
     * without confirming the hardware format, and the decoder then returned EMPTY
     * surfaces (transfer rc=0, luma measured at 0 everywhere).
     *
     * So we ask the library at runtime - `AV_PIX_FMT_FLAG_HWACCEL` says whether a
     * format is a hardware one, whatever its value and its name. Version-proof,
     * which is what it should have been from the start. */
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && (d->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            wlog("h264: hardware hwaccel chosen: %s (fmt=%d)",
                 av_get_pix_fmt_name(*p) ? av_get_pix_fmt_name(*p) : "?", (int)*p);
            /* === S21 2026-08-22 — CREATE THE SURFACE POOL IF THE DECODER DEMANDS IT ===
             * We supplied ONLY `hw_device_ctx`. But a hwaccel that does not
             * advertise HW_DEVICE_CTX expects the CLIENT to allocate the pool
             * (`hw_frames_ctx`) itself from `get_format`: otherwise the decoder
             * writes into surfaces that are not really its own - hence pictures
             * that "exist" (dimensions, I/P type) but stay EMPTY, exactly our
             * symptom. So we create it here when it is missing, asking the
             * decoder itself for its parameters. */
            if (!ctx->hw_frames_ctx && ctx->hw_device_ctx) {
                AVBufferRef *frames = NULL;
                int r = avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx,
                                                         *p, &frames);
                if (r >= 0 && frames) {
                    AVHWFramesContext *fc = (AVHWFramesContext *)frames->data;
                    wlog("h264: [S21] pool requested by the decoder: format=%d (%s) "
                         "sw_format=%d (%s) %dx%d pool=%d",
                         (int)fc->format,
                         av_get_pix_fmt_name(fc->format) ? av_get_pix_fmt_name(fc->format) : "?",
                         (int)fc->sw_format,
                         av_get_pix_fmt_name(fc->sw_format) ? av_get_pix_fmt_name(fc->sw_format) : "?",
                         fc->width, fc->height, fc->initial_pool_size);
                    int ir = av_hwframe_ctx_init(frames);
                    if (ir >= 0) {
                        ctx->hw_frames_ctx = frames;
                        wlog("h264: [S21] surface pool CREATED and initialised");
                    } else {
                        char e[128] = {0}; av_strerror(ir, e, sizeof(e));
                        wlog("h264: [S21] av_hwframe_ctx_init FAILED rc=%d (%s)", ir, e);
                        av_buffer_unref(&frames);
                    }
                } else {
                    char e[128] = {0}; if (r < 0) av_strerror(r, e, sizeof(e));
                    wlog("h264: [S21] avcodec_get_hw_frames_parameters rc=%d (%s) "
                         "- no pool created", r, e);
                }
            } else {
                wlog("h264: [S21] pool already present (hw_frames_ctx=%p) or no "
                     "peripherique (%p)", (void *)ctx->hw_frames_ctx,
                     (void *)ctx->hw_device_ctx);
            }
            return *p;
        }
    }
    wlog("h264: no HARDWARE format offered - falling back to %s (fmt=%d)",
         av_get_pix_fmt_name(pix_fmts[0]) ? av_get_pix_fmt_name(pix_fmts[0]) : "?",
         (int)pix_fmts[0]);
#else
    /* === HW2 2026-08-22 — THE PREFERENCE ORDER WAS NOT ONE ===
     *
     * The comment announced "CUDA, then VDPAU, then VAAPI", but all three were
     * tested in the SAME condition: the loop returned the first format offered
     * by libavcodec, not the first of our list. Since libavcodec announces VDPAU
     * before CUDA, we decoded in VDPAU - while the hardware context created just
     * before is a CUDA context.
     *
     * Observed log: "cuda hwdevice created" followed by "hwaccel vdpau
     * selected". That disagreement between device and format explains pictures
     * that are valid - libavcodec reports no error - but visually degraded, with
     * blocks on motion.
     *
     * One loop per format, in the intended order: that is the only way to
     * express a preference. */
    static const enum AVPixelFormat preferred[] = {
        AV_PIX_FMT_CUDA, AV_PIX_FMT_VDPAU, AV_PIX_FMT_VAAPI
    };
    for (size_t k = 0; k < sizeof(preferred) / sizeof(preferred[0]); k++) {
        for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == preferred[k]) {
                wlog("h264: hwaccel %s retenu (preference %zu/%zu)",
                     av_get_pix_fmt_name(*p), k + 1,
                     sizeof(preferred) / sizeof(preferred[0]));
                return *p;
            }
        }
    }
    wlog("h264: no hwaccel in pix_fmts list, falling back to %s",
         av_get_pix_fmt_name(pix_fmts[0]));
#endif
    return pix_fmts[0];
}

// av_log -> journal redirect (silent by default, debug only)
/* === G18 2026-08-22 — DETECTING DRIFT THROUGH LIBAVCODEC'S ERRORS ===
 *
 * Symptom seen directly on the pictures coming out of the decoder in session: on
 * animated content the picture degrades progressively (displaced macroblocks,
 * washed-out colours) then recovers all at once. That is REFERENCE DRIFT: a lost
 * slice breaks a reference picture, and every intermediate picture that leans on
 * it inherits the error until the next IDR.
 *
 * G13 does not catch it: it waits for 15 frames WITH NO OUTPUT, whereas during
 * the drift the decoder does produce pictures - wrong ones, but pictures. The
 * only reliable signal is libavcodec itself, which logs "error while decoding
 * MB", "Frame num gap", "concealing ... errors" at ERROR level. We count them,
 * and the session layer uses that to request a targeted IDR (glue,
 * rate-limited).
 *
 * One decoder in the application: a global counter is enough. */
static volatile uint32_t g_bitstream_errors = 0;
static uint32_t g_au_flush_index = 0;   /* G39: access unit (frame) number */

static void av_log_cb(void *avcl, int level, const char *fmt, va_list args) {
    (void)avcl;
    /* ERROR level (16) and worse = the decoder judges the stream corrupted.
     * That is exactly the drift event: we count it before any filtering. */
    if (level <= AV_LOG_ERROR) g_bitstream_errors++;
    /* libavcodec's detail (hwaccel init, surface choice) is only useful for
     * diagnosis: `SHADOW_FFMPEG_DEBUG=1` opens it, capped. */
    if (level > AV_LOG_WARNING) {
        static int dbg = -1, seen = 0;
        if (dbg < 0) { const char *e = getenv("SHADOW_FFMPEG_DEBUG"); dbg = e ? atoi(e) : 0; }
        if (!dbg || seen++ > 600) return;
    }
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    // strip trailing \n
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    wlog("ffmpeg: %s", buf);
}

/* Sets up HARDWARE decoding: choosing the device, checking the launch mode,
 * selecting the format the decoder demands. Both platforms are handled here,
 * each in its own branch.
 *
 * On the Switch side, three lessons are frozen into it. S16 looks the device up
 * by NAME at runtime ("envideo" then "nvtegra") rather than by a compiled
 * constant: that is what made it possible to switch from one FFmpeg generation
 * to the next WITHOUT touching the code, the day `envideo` turned out to be
 * broken. S17 tells applet mode from application mode, where the available
 * memory differs. S20 recognises the hardware format by its FLAG and not by a
 * constant that depends on the headers' version - that constant changed value
 * between versions, and every picture ended up silently dropped.
 *
 * Extracted from h264_decoder_create on 2026-08-25: 146 lines, five decoder
 * fields touched, no escape path. On failure, `hwaccel_active` stays false and
 * the caller carries on in software - that is the fallback, not an error. */
static void hw_setup_device(h264_decoder *d)
{
#ifdef __SWITCH__
    /* === S16 2026-08-22 — LOOK THE DEVICE UP BY NAME, NOT BY THE ENUM ===
     *
     * The Averne fork RENAMED its accelerator between two versions:
     * `AV_HWDEVICE_TYPE_NVTEGRA` / `AV_PIX_FMT_NVTEGRA` became `..._ENVIDEO`.
     * But the local install mixes the two: the libraries are the Averne c8ff0ba
     * build (which exposes "envideo"), while the portlibs headers come from the
     * devkitPro package (which still declares "nvtegra"). Hard-coding either
     * constant therefore makes the binary depend on the installation - and breaks
     * on the slightest update.
     * `av_hwdevice_find_type_by_name` queries the library that is ACTUALLY
     * linked: we try both names, the right one answers. */
    enum AVHWDeviceType hw_type = av_hwdevice_find_type_by_name("envideo");
    if (hw_type == AV_HWDEVICE_TYPE_NONE)
        hw_type = av_hwdevice_find_type_by_name("nvtegra");
    if (hw_type == AV_HWDEVICE_TYPE_NONE)
        wlog("h264: no 'envideo'/'nvtegra' accelerator in this libavutil");
    else
        wlog("h264: accelerator found by name -> type=%d (%s)", (int)hw_type,
             av_hwdevice_get_type_name(hw_type));

    /* === S17 2026-08-22 — APPLET vs APPLICATION MODE ===
     * The hardware decoder negotiates correctly (device created, `envideo`
     * format chosen, transfer rc=0) but returns EMPTY surfaces. The remaining
     * suspect is the launch mode: in APPLET mode (homebrew opened from the
     * Album), HOS restricts memory and access to the graphics services - NVDEC
     * is not reliably usable there. The crash reports did show
     * `Program ID 010029b0118e8000` (the Album). So we log the mode and stop
     * having to guess it. */
    {
        AppletType at = appletGetAppletType();
        bool is_app = (at == AppletType_Application || at == AppletType_SystemApplication);
        wlog("h264: mode de lancement = %s (AppletType=%d)%s",
             is_app ? "APPLICATION" : "APPLET",
             (int)at,
             is_app ? "" : " — NVDEC souvent indisponible en mode applet "
                          "(launch the homebrew while holding R on a game)");
    }

    /* === S20 2026-08-22 — WHAT THE DECODER REALLY DEMANDS ===
     * A hwaccel advertises the METHOD(S) it accepts. If `envideo` does not
     * support HW_DEVICE_CTX but only HW_FRAMES_CTX, it falls to the client to
     * create the surface pool: without it the decoder produces pictures... that
     * are empty, exactly our symptom. So we enumerate, we log, and we keep the
     * config that matches the device. */
    const AVCodecHWConfig *chosen_cfg = NULL;
    for (int i = 0;; i++) {
        const AVCodecHWConfig *c = avcodec_get_hw_config(d->codec, i);
        if (!c) break;
        wlog("h264: [S20] config materielle #%d : pix_fmt=%d (%s) methodes=0x%x "
             "(device_ctx=%d frames_ctx=%d ad_hoc=%d) device_type=%d (%s)",
             i, (int)c->pix_fmt,
             av_get_pix_fmt_name(c->pix_fmt) ? av_get_pix_fmt_name(c->pix_fmt) : "?",
             c->methods,
             !!(c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX),
             !!(c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX),
             !!(c->methods & AV_CODEC_HW_CONFIG_METHOD_AD_HOC),
             (int)c->device_type,
             av_hwdevice_get_type_name(c->device_type)
                 ? av_hwdevice_get_type_name(c->device_type) : "?");
        if (c->device_type == hw_type) chosen_cfg = c;
    }
    if (!chosen_cfg)
        wlog("h264: [S20] NO hardware config of the decoder matches the "
             "device type=%d - the hwaccel will not be engaged", (int)hw_type);
    d->hw_cfg = chosen_cfg;

    int hw_rc = (hw_allowed() && hw_type != AV_HWDEVICE_TYPE_NONE)
              ? av_hwdevice_ctx_create(&d->hw_device_ctx, hw_type, NULL, NULL, 0)
              : -1;   /* S14 : repli logiciel demande (env ou marqueur) */
    if (hw_rc >= 0 && d->hw_device_ctx) {
        d->ctx->hw_device_ctx = av_buffer_ref(d->hw_device_ctx);
        d->ctx->get_format = get_hw_format;
        d->hwaccel_active = true;
        wlog("h264: NVTEGRA hwdevice created");
    } else {
        char err[128] = {0};
        if (hw_allowed()) {
            av_strerror(hw_rc, err, sizeof(err));
            wlog("h264: hwdevice NVTEGRA FAIL rc=%d (%s) — fallback CPU decode", hw_rc, err);
        } else {
            wlog("h264: hardware acceleration disabled - SOFTWARE decoding");
        }
        d->hwaccel_active = false;
    }
#else
    /* N55 2026-05-14: testing NVDEC CUDA on Linux x86_64 before the Switch.
     * The Shadow documentation says "won't work with software decoding" - to
     * check that hardware decoding produces a 100 % complete picture
     * (= multi-slice handled by hardware reference-frame management), we enable
     * CUDA hwaccel on Linux. The GPU->CPU copy costs ~1 ms at 1080p but that does
     * not matter for a visual test.
     * SHADOW_HWACCEL=0 disables it (= CPU fallback, as before). */
    int hw_enable = hw_allowed();   /* S14: one decision (env + marker) */
    /* 2026-05-18: the fallback persists across sessions. If a previous session
     * reached HW_FAIL_THRESHOLD, we write a marker file. On the next init we read
     * that marker and skip hwaccel outright - the user does not have to set
     * SHADOW_HWACCEL=0 by hand after a crash.
     *
     * The user can force a hwaccel retry by deleting the file:
     *   rm /tmp/halyard/hwaccel.disabled
     *
     * Or by explicitly overriding: SHADOW_HWACCEL=1 (= overrides the marker). */
    if (hw_enable && !getenv("SHADOW_HWACCEL")) {
        FILE *mf = fopen(HW_MARKER_PATH, "rb");
        if (mf) {
            fclose(mf);
            hw_enable = 0;
            wlog("h264: marker %s present -> hwaccel skipped (= the previous session "
                 "crashed). Delete the file to retry, or set SHADOW_HWACCEL=1.",
                 HW_MARKER_PATH);
        }
    }
    if (hw_enable) {
        /* Try CUDA first (= NVIDIA's proprietary NVDEC SDK), VDPAU as a fallback
         * (= also NVDEC, through an older X11 API more tolerant of a busy GPU). */
        const enum AVHWDeviceType types[] = {
            AV_HWDEVICE_TYPE_CUDA,
            AV_HWDEVICE_TYPE_VDPAU,
            AV_HWDEVICE_TYPE_VAAPI,
            AV_HWDEVICE_TYPE_NONE,
        };
        d->hwaccel_active = false;
        for (int i = 0; types[i] != AV_HWDEVICE_TYPE_NONE; i++) {
            const char *type_name = av_hwdevice_get_type_name(types[i]);
            int hw_rc = av_hwdevice_ctx_create(&d->hw_device_ctx, types[i],
                                                 NULL, NULL, 0);
            if (hw_rc >= 0 && d->hw_device_ctx) {
                d->ctx->hw_device_ctx = av_buffer_ref(d->hw_device_ctx);
                d->ctx->get_format = get_hw_format;
                d->hwaccel_active = true;
                wlog("h264: %s hwdevice created (= NVDEC Linux)", type_name);
                break;
            }
            char err[128] = {0};
            av_strerror(hw_rc, err, sizeof(err));
            wlog("h264: %s hwdevice FAIL rc=%d (%s) - try next", type_name, hw_rc, err);
        }
        if (!d->hwaccel_active) {
            wlog("h264: ALL hwdevices failed — fallback CPU decode");
        }
    } else {
        d->hwaccel_active = false;
        wlog("h264: Linux SHADOW_HWACCEL=0 - CPU decode forced");
    }
#endif
}

const char *h264_decoder_codec_name(const h264_decoder *d)
{
    if (!d || !d->codec || !d->codec->name) return "?";
    /* libavcodec's name ("h264", "hevc", "av1") rendered in the user's terms -
     * they picked "H.265" from a menu. */
    if (strcmp(d->codec->name, "hevc") == 0) return "H.265";
    if (strcmp(d->codec->name, "h264") == 0) return "H.264";
    if (strcmp(d->codec->name, "av1")  == 0) return "AV1";
    return d->codec->name;
}

int h264_decoder_hw_active(const h264_decoder *d)
{
    return (d && d->hw_active) ? 1 : 0;
}

h264_decoder *h264_decoder_create(h264_frame_cb on_frame, void *user) {
    av_log_set_callback(av_log_cb);
    av_log_set_level(AV_LOG_DEBUG);   /* S19: full detail, filtered in av_log_cb */

    h264_decoder *d = calloc(1, sizeof(*d));
    /* COL1 - a new session knows nothing of the stream's colorimetry yet. */
    __atomic_store_n(&g_color_matrix, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_color_full, -1, __ATOMIC_RELAXED);
    if (!d) return NULL;
    d->on_frame = on_frame;
    d->user     = user;

    /* === K15 2026-08-27 — THE CODEC IS NO LONGER FROZEN ===
     * `SHADOW_CODEC` carries the WIRE's values, those of the channel
     * announcement: 0 = H.264 (default), 1 = H.265, 2 = AV1. The SAME variable
     * chooses what we ask the server for and what we prepare to decode - letting
     * them diverge would give a decoder that does not understand what it
     * receives, and on this protocol that does not announce itself: the picture
     * simply never comes out.
     *
     * We read an env var here rather than change a signature, because
     * `h264_decoder_create` has six callers, three of them on historical paths.
     * The fallback is H.264 for anything we do not recognise. */
#if defined(__vita__) || defined(__psp2__)
    /* === THE HARDWARE PATH, AND IT IS THE ONLY ONE HERE ====================
     *
     * Measured on console 2026-09-13: `avcodec_find_decoder FAIL`, because the
     * vitasdk's libavcodec ships 22 decoders and H.264 is not one of them. A
     * locally built ffmpeg would link, but a 444 MHz Cortex-A9 will not decode
     * 720p in software - the hardware is not an optimisation here.
     *
     * OPENED BY PROBING, and that is the correction of 2026-09-13. It used to
     * open at a fixed 1920x1088 ceiling on the reasoning that a bigger
     * reservation removes a class of "the stream was slightly larger than we
     * assumed" failures. On console the call answered
     * `SCE_VIDEODEC_ERROR_INVALID_PARAM` (0x80620802) - not OUT_OF_MEMORY, so
     * it is not a few MB, it is a hard geometry cap - and every session, every
     * retry, had no video at all. A ceiling nobody had measured was worse than
     * no ceiling.
     *
     * So we ASK. `sceVideodecInitLibrary` is a cheap yes/no taken before any
     * frame arrives, so the ladder below costs a handful of failed calls once
     * per session and MEASURES what this silicon grants instead of betting on
     * it. The granted configuration is logged: the first console run therefore
     * answers "what is the real cap?" permanently, which is the whole reason
     * for descending rather than jumping straight to the negotiated size.
     *
     * The reference count descends with the geometry because it is the same
     * constraint - the decoded-picture buffer - and the announcement grants us
     * a stream that needs far fewer than four.
     *
     * `SHADOW_VITA_DEC_MAX_W/H` still IMPOSE a geometry (a toggle must be able
     * to force a value, KB's env-var rule); setting either one skips the
     * ladder entirely and tries that one configuration, so a campaign can pin
     * a size the ladder would have walked past. */
    {
        const char *ew = getenv("SHADOW_VITA_DEC_MAX_W");
        const char *eh = getenv("SHADOW_VITA_DEC_MAX_H");
        const char *er = getenv("SHADOW_VITA_DEC_REFS");
        static const struct { int w, h, refs; } ladder[] = {
            { 1920, 1088, 3 },   /* if it is granted, we learn it here */
            { 1280,  720, 4 },   /* what the session negotiates in handheld */
            { 1280,  720, 3 },
            { 1280,  720, 2 },
            {  960,  544, 3 },   /* the panel's own resolution */
            {  640,  368, 2 },
        };

        if (ew || eh || er) {
            const int mw = ew ? atoi(ew) : 1280;
            const int mh = eh ? atoi(eh) : 720;
            const int mr = er ? atoi(er) : 3;
            wlog("h264: Vita decoder geometry IMPOSED by toggle: %dx%d, %d refs",
                 mw, mh, mr);
            d->vita = h264_vita_open(mw, mh, mr);
            if (!d->vita) {
                wlog("h264: the Vita hardware decoder refused the imposed "
                     "%dx%d/%d refs - no video", mw, mh, mr);
                free(d); return NULL;
            }
        } else {
            for (size_t i = 0; i < sizeof ladder / sizeof ladder[0]; i++) {
                d->vita = h264_vita_open(ladder[i].w, ladder[i].h,
                                         ladder[i].refs);
                if (d->vita) {
                    /* `%zu` printed the literal "zu" on console: this
                     * toolchain's newlib does not carry the length modifier.
                     * The journal is the evidence, so a silently dropped
                     * conversion is a defect, not a cosmetic one. */
                    wlog("h264: Vita hardware decoder open at %dx%d, %d refs "
                         "(step %u of the ladder)",
                         ladder[i].w, ladder[i].h, ladder[i].refs,
                         (unsigned)(i + 1));
                    break;
                }
            }
            if (!d->vita) {
                wlog("h264: the Vita hardware decoder refused every geometry "
                     "down to 640x368 - no video");
                free(d); return NULL;
            }
        }
    }
    return d;
#endif

    {
        const char *ec = getenv("SHADOW_CODEC");
        const int wanted = ec ? atoi(ec) : 0;
        enum AVCodecID id = AV_CODEC_ID_H264;
        if      (wanted == 1) { id = AV_CODEC_ID_HEVC; d->hevc = 1; }
        else if (wanted == 2) { id = AV_CODEC_ID_AV1; }
        d->codec = (AVCodec *)avcodec_find_decoder(id);
        if (!d->codec && id != AV_CODEC_ID_H264) {
            /* A missing decoder must SAY SO: without this message we would
             * hunt the defect in the network for hours. */
            wlog("h264: decoder %d absent from libavcodec - falling back to H.264",
                 (int)id);
            d->hevc = 0;
            d->codec = (AVCodec *)avcodec_find_decoder(AV_CODEC_ID_H264);
        }
    }
    if (!d->codec) { wlog("h264: avcodec_find_decoder FAIL"); free(d); return NULL; }
    wlog("h264: codec=%s%s", d->codec->name, d->hevc ? " (HEVC)" : "");

    d->ctx = avcodec_alloc_context3(d->codec);
    if (!d->ctx) { wlog("h264: avcodec_alloc_context3 FAIL"); free(d); return NULL; }

    // Clean single-threaded decode. Tested: FF_THREAD_FRAME (the Chromium
    // standard) gives frames=0 on this switch-ffmpeg -> we keep thread_type=0.
    d->ctx->thread_count = 1;
    d->ctx->thread_type  = 0;

    /* === G29 2026-08-22 — DECODER CONFIG MODELLED ON THE OFFICIAL CLIENT ===
     * RE of the official binary (FFMPEGVideoDecoder @0xbcb100) shows it opens
     * libavcodec with PURE DEFAULTS: no flag set between avcodec_alloc_context3
     * and avcodec_open2. In particular NO AV_CODEC_FLAG2_FAST.
     *
     * And FLAG2_FAST allows shortcuts that are NOT SPEC-COMPLIANT (approximate
     * IDCT / motion compensation / dequantisation): on complex pictures (scene
     * change, heavy motion) it produces SMALL BLOCKS all over the image -
     * exactly the reported defect. LOW_DELAY is a deviation too (the official
     * client does not set it). So we return to the defaults, to be bit-for-bit
     * like the official client.
     * SHADOW_DEC_FAST=1 / SHADOW_DEC_LOWDELAY=1 to re-enable them (debug). */
    {
        static int fast=-1, lowd=-1;
        if (fast<0){ const char*e=getenv("SHADOW_DEC_FAST"); fast=e?atoi(e):0; }
        if (lowd<0){ const char*e=getenv("SHADOW_DEC_LOWDELAY"); lowd=e?atoi(e):0; }
        if (lowd) d->ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
        if (fast) d->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    }
    d->ctx->has_b_frames  = 0;   /* Shadow encoder = no B-frames (no effect on quality) */

    // === Hardware decode through the Tegra X1's NVDEC ===
    // switch-ffmpeg (averne) exposes AV_HWDEVICE_TYPE_NVTEGRA, which uses the
    // NVDEC hardware engine. Decodes H.264 in ~1 ms against ~10 ms on pure CPU.
    // If init fails we fall back to software (CPU decode).
    hw_setup_device(d);   /* leaves hwaccel_active false if the hardware refuses */

    /* RE 2026-05-14: the flags that work on CPU decode - sharp top + clean green
     * bottom. AV_CODEC_FLAG2_CHUNKS and has_b_frames=0 crash -> do not set them.
     *
     * 2026-05-18 ROOT CAUSE OF THE NVDEC FAILURE: OUTPUT_CORRUPT + SHOW_ALL +
     * error_concealment are implemented ONLY in libavcodec's software path
     * (h264dec.c). In hwaccel mode (CUDA/VDPAU/VAAPI/NVTEGRA) the hardware
     * decoder produces partially decoded surfaces (= missing MBs left undefined
     * in GPU memory). Forcing those frames out through OUTPUT_CORRUPT then
     * attempting av_hwframe_transfer_data on them -> the driver detects an
     * invalid surface state -> `rc=-5 (Input/output error)` + `Failed to sync
     * surface 0xNN`.
     *
     * Fix: these software-only flags apply ONLY when hwaccel is inactive. In
     * hwaccel we let the HW decoder drop its own invalid frames
     * (decode_error_flags & 0x3 -> caught by our hard_err check below). */
    if (d->hwaccel_active) {
        d->ctx->error_concealment = 0;
        d->ctx->err_recognition   = 0;
        wlog("h264: hwaccel mode - software-only flags disabled "
             "(OUTPUT_CORRUPT/SHOW_ALL/EC) to avoid hwframe_transfer FAIL");
    } else {
        /* RE3 2026-05-18 - random UDP packet loss (~1-3 %) creates holes in the
         * H.264 bytestream -> libavcodec's error_concealment kicks in -> smear
         * and artefacts on moving video. Three toggles for interactive A/B
         * testing:
         *
         *   SHADOW_OUTPUT_CORRUPT=0 -> libavcodec DROPS the corrupted frames
         *                              (= the picture freezes on the last clean
         *                                 one, no smear BUT a visible time gap)
         *                              Default 1 = output everything (legacy)
         *   SHADOW_SHOW_ALL=0       -> outputs ONLY "complete" frames (= no
         *                              partially decoded ones before the first
         *                              keyframe). Default 1 = output everything
         *   SHADOW_ERR_CONCEAL=N    -> 0=no EC (garbage blocks), 1=guess_mvs,
         *                              3=guess_mvs+deblock (default),
         *                              259=+favor_inter (= the classic smear). */
        int output_corrupt = 1, show_all = 1;
        {
            const char *e;
            if ((e = getenv("SHADOW_OUTPUT_CORRUPT"))) output_corrupt = atoi(e);
            if ((e = getenv("SHADOW_SHOW_ALL")))      show_all = atoi(e);
        }
        if (output_corrupt) d->ctx->flags  |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
        if (show_all)       d->ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
        wlog("h264: CPU mode - OUTPUT_CORRUPT=%d SHOW_ALL=%d", output_corrupt, show_all);

        /* F2 WIN 2026-05-22: default ec=0 (= concealment disabled entirely).
         * An A/B test of 4 modes through test-cli + PPM dump proves it:
         * SHADOW_ERR_CONCEAL=0 ALONE gives a 100 % clean picture WITH the full
         * Windows taskbar visible (icons, search box). With ec=3, libavcodec
         * invents MBs to fill in the lost UDP chunks -> fake reconstructions
         * (= a bottom-centre smear of vertical stripes under the visible trunk).
         * With no concealment the missing MBs stay at 0 / the previous frame =
         * invisible, and the taskbar is legible.
         * Set SHADOW_ERR_CONCEAL=3 to return to the N40 behaviour. */
        int ec = 0;
        {
            const char *e = getenv("SHADOW_ERR_CONCEAL");
            if (e) ec = atoi(e);
        }
        d->ctx->error_concealment = ec;
        /* F17 2026-05-22 23h20: strict err_recognition (= AV_EF_BITSTREAM |
         * AV_EF_BUFFER | AV_EF_AGGRESSIVE | AV_EF_CRCCHECK) -> libavcodec flags
         * more partially corrupted frames as corrupt -> our drop check
         * (= frames_displayed != frames_decoded) catches and drops them -> no
         * green propagation. */
        /* G29: the official client leaves err_recognition at its DEFAULT. Strict
         * mode (F17: AGGRESSIVE|CRCCHECK) made VALID pictures be flagged as
         * corrupt (false positives) -> our drop check threw them away ->
         * stutter and artefacts. Default 0 (like the official client).
         * SHADOW_ERR_RECOGNITION=N to return to strict mode (=6151 for F17). */
        const char *e_recog = getenv("SHADOW_ERR_RECOGNITION");
        d->ctx->err_recognition = e_recog ? atoi(e_recog) : 0;
        wlog("h264: CPU mode - error_concealment=%d (1=guess_mvs 2=deblock 256=favor_inter)", ec);
    }

    int rc = avcodec_open2(d->ctx, d->codec, NULL);
    if (rc < 0) {
        char err[128] = {0};
        av_strerror(rc, err, sizeof(err));
        wlog("h264: avcodec_open2 FAIL rc=%d (%s)", rc, err);
        avcodec_free_context(&d->ctx); free(d); return NULL;
    }

    d->pkt      = av_packet_alloc();
    d->frame    = av_frame_alloc();
    d->sw_frame = av_frame_alloc();  // for the hwframe -> CPU transfer
    if (!d->pkt || !d->frame || !d->sw_frame) {
        wlog("h264: alloc packet/frame FAIL");
        if (d->pkt)      av_packet_free(&d->pkt);
        if (d->frame)    av_frame_free(&d->frame);
        if (d->sw_frame) av_frame_free(&d->sw_frame);
        avcodec_free_context(&d->ctx); free(d); return NULL;
    }

    d->au_buf = malloc(AU_BUF_MAX);
    if (!d->au_buf) { h264_decoder_destroy(d); return NULL; }
    d->au_len = 0;
    d->au_active = false;

    // Async transfer worker - only worth it when hwaccel is active, since
    // it is the NVDEC->CPU transfer_data that is expensive (11 ms VIC detiling).
    if (d->hwaccel_active) {
        if (!vaw_init(&d->async, on_frame, user)) {
            wlog("h264: vaw_init FAIL - fallback synchronous transfer");
        } else {
            /* K18b - AFTER the init, which memsets the structure. Set before,
             * the pointer would be wiped and the panel would announce "software"
             * while the hardware is working. */
            d->async.hw_active = &d->hw_active;
            wlog("h264: async transfer worker started");
        }
    }

    wlog("h264: decoder ready (libavcodec %s)", av_version_info());
    return d;
}

void h264_decoder_destroy(h264_decoder *d) {
#if defined(__vita__) || defined(__psp2__)
    if (d) { h264_vita_close(d->vita); free(d); }
    return;
#endif

    if (!d) return;
    vaw_destroy(&d->async);
    if (d->frame)    av_frame_free(&d->frame);
    if (d->sw_frame) av_frame_free(&d->sw_frame);
    if (d->pkt)      av_packet_free(&d->pkt);
    if (d->ctx)      avcodec_free_context(&d->ctx);
    if (d->hw_device_ctx) av_buffer_unref(&d->hw_device_ctx);
    free(d->au_buf);
    free(d);
}

// Flush the accumulated access unit: feed the AU buffer to libavcodec as ONE
// packet and pull the frames. Called on a PTS change (= a new picture detected).
#ifdef __SWITCH__
/* Brings a picture decoded IN HARDWARE back to main memory, then hands it to
 * the caller.
 *
 * This is the most expensive and most fragile passage of decoding on Switch:
 * S10 forces the transfer to be SYNCHRONOUS (the asynchronous worker stayed
 * blocked in its very first transfer and dropped everything else), S28 switches
 * automatically to software when a transfer exceeds one second - the exact
 * symptom of the broken `envideo` path, which returned rc=0 with an EMPTY
 * surface - and S30 takes the hardware format from its flag rather than from a
 * constant that depends on the headers' version.
 *
 * Extracted from flush_access_unit on 2026-08-25: 136 lines with no control-flow
 * escape, touching only six decoder fields. */
static void hw_deliver_frame(h264_decoder *d)
{
    /* === S10 2026-08-22 — SYNCHRONOUS TRANSFER BY DEFAULT ON SWITCH ===
     *
     * Console measurement: the asynchronous worker NEVER processed anything
     * (`async queue full, dropped frame (total dropped=N, processed=0)`) - it
     * stays blocked in its first `av_hwframe_transfer_data`, so no picture ever
     * reaches the screen. The transfer engine (VIC/envideo) seems tied to the
     * thread that owns the decoder: having another thread call it never returns.
     *
     * That worker dated from the WebRTC architecture, where decoding ran on
     * libjuice's thread which had to be freed at all costs. Since G25, decoding
     * ALREADY has its own thread: a synchronous transfer therefore no longer
     * blocks reception. The worker becomes useless and harmful.
     * `SHADOW_ASYNC_TRANSFER=1` restores it (debug). */
    static int g_async_ok = -1;
    if (g_async_ok < 0) {
        const char *e = getenv("SHADOW_ASYNC_TRANSFER");
        g_async_ok = e ? atoi(e) : 0;
    }
    if (g_async_ok && d->async.started) {
        if (!vaw_push(&d->async, d->frame)) {
            if (d->async.frames_dropped <= 5
                || d->async.frames_dropped % 60 == 0) {
                wlog("h264: async queue full, dropped frame "
                     "(total dropped=%u, processed=%u)",
                     d->async.frames_dropped,
                     d->async.frames_processed);
            }
        }
    } else {
        /* The normal path since S10: SYNCHRONOUS transfer on the decode thread
         * (the asynchronous worker is disabled by default). */
        /* === S33 2026-08-25 — DESTINATION ALIGNED ON 256 BYTES ===
         *
         * Measured over a run of ten sessions: ffmpeg reports "Frame
         * address/pitch not aligned to 256, falling back to cpu transfer" on
         * nearly every picture - 2785 times. The `nvtegra` hardware path was
         * therefore NOT taking its VIC transfer, but a CPU copy.
         *
         * The stride is fine (1280 = 5 x 256): it is the destination buffer's
         * ADDRESS that is not. `sw_frame` was allocated empty and it was
         * `av_hwframe_transfer_data` that gave it its buffer, with libavutil's
         * default alignment (32 bytes).
         *
         * So we pre-allocate it ourselves, aligned on 256. On failure we
         * release the buffer and let ffmpeg do as before: a CPU copy beats a lost
         * picture. */
        if (!d->sw_frame->data[0]) {
            d->sw_frame->format = AV_PIX_FMT_NV12;   /* the sw_format the pool announces */
            d->sw_frame->width  = d->frame->width;
            d->sw_frame->height = d->frame->height;
            if (av_frame_get_buffer(d->sw_frame, 256) < 0)
                av_frame_unref(d->sw_frame);         /* fallback: let ffmpeg allocate */
        }

        struct timespec t_av, t_ap;
        clock_gettime(CLOCK_MONOTONIC, &t_av);
        int trc = av_hwframe_transfer_data(d->sw_frame, d->frame, 0);
        clock_gettime(CLOCK_MONOTONIC, &t_ap);
        long transfer_ms = (long)((t_ap.tv_sec - t_av.tv_sec) * 1000
                            + (t_ap.tv_nsec - t_av.tv_nsec) / 1000000);
        {   /* S10: trace the first 3 transfers - this is the link that used to
             * block, and we want to see it succeed in black and white. */
            static int n = 0;
            if (n < 3) {
                char e2[128] = {0};
                if (trc < 0) av_strerror(trc, e2, sizeof(e2));
                wlog("h264: [S10] hardware transfer #%d rc=%d%s -> fmt=%d (%s)",
                     n, trc, trc < 0 ? e2 : "",
                     (int)d->sw_frame->format,
                     av_get_pix_fmt_name(d->sw_frame->format)
                         ? av_get_pix_fmt_name(d->sw_frame->format) : "?");
                n++;
            }
        }
        /* === S18 2026-08-22 — DETECTING A HARDWARE DECODER THAT RETURNS NOTHING ===
         *
         * On this Switch, `envideo` accepts everything (device created, format
         * negotiated, transfer rc=0) but NEVER writes into the surfaces: luma
         * measured at 0 everywhere, black screen. No error is reported, so
         * nothing triggered the existing fallback (which only counts FAILED
         * transfers). Verified in APPLICATION mode (AppletType=0), so unrelated
         * to applet mode.
         * So we sample the first transferred pictures: if they are entirely
         * black, we drop the marker that switches to SOFTWARE decoding - which
         * works (~51 fps at 720p). The user then only has to relaunch, instead of
         * sitting in front of a black screen with no explanation. */
        if (trc >= 0) {
            /* === K18c 2026-08-28 — THE FLAG WAS STILL IN THE WRONG PLACE ===
             * Set first on the desktop/CUDA branch, then on the ASYNCHRONOUS
             * thread - which has been DISABLED by default since S10. The path
             * actually taken on console is this one, the SYNCHRONOUS transfer.
             * The panel therefore announced "software" while the log said
             * "hardware hwaccel chosen: nvtegra".
             * Third attempt, and the lesson is clear: a state flag is set where
             * the work IS DONE, never where you believe it is done. I should have
             * followed the log rather than the code. */
            d->hw_active = true;
            /* === S28 2026-08-22 — THE HARDWARE TRANSFER IS PATHOLOGICALLY SLOW ===
             * Measured on console: `av_hwframe_transfer_data` takes **5.2 s**
             * per picture (timestamped log: cleanup at 35.719, transfer returned
             * at 40.913). One cause for three symptoms:
             *   - stopping the stream takes ~10 s (we wait on that transfer);
             *   - the asynchronous worker looked "blocked forever" (S10): it was
             *     simply running at 5 s per picture;
             *   - the decode queue overflows permanently.
             * At that rate the hardware is unusable, even if it did return
             * pixels. So we arm the fallback on the FIRST abnormally long
             * transfer, without waiting for the "black picture" criterion (which
             * required 5 pictures, i.e. 25 s at that rate). */
            if (transfer_ms > 1000 && ++d->hw_slow == 1) {
                FILE *mf = fopen(HW_MARKER_PATH, "wb");
                if (mf) {
                    fprintf(mf, "envideo hardware transfer pathologically slow "
                                "(%ld ms/picture). Software fallback armed.\n"
                                "Delete this file to try hardware again.\n",
                            transfer_ms);
                    fclose(mf);
                }
                wlog("h264: *** HARDWARE TRANSFER UNUSABLE *** %ld ms for "
                     "ONE picture -> marker written, RELAUNCH the application "
                     "(software decoding sustains ~51 fps)", transfer_ms);
            }
            if (d->hw_check_seen < 8) {
                d->hw_check_seen++;
                wlog("h264: [S28] transfert #%d en %ld ms", d->hw_check_seen, transfer_ms);
                unsigned long sum = 0;
                const uint8_t *yp = d->sw_frame->data[0];
                int ls = d->sw_frame->linesize[0];
                if (yp && ls > 0) {
                    for (int r = 0; r < d->sw_frame->height; r += 29)
                        for (int c = 0; c < d->sw_frame->width; c += 29)
                            sum += yp[(size_t)r * ls + c];
                }
                if (sum == 0) d->hw_check_empty++;
                if (d->hw_check_empty == 5) {
                    FILE *mf = fopen(HW_MARKER_PATH, "wb");
                    if (mf) {
                        fprintf(mf, "'envideo' hardware decoding: transfers rc=0 "
                                    "but EMPTY surfaces (luma=0) over 5 pictures.\n"
                                    "Software fallback armed. Delete this file to "
                                    "try hardware again.\n");
                        fclose(mf);
                    }
                    wlog("h264: *** HARDWARE DECODING INOPERATIVE *** 5 pictures "
                         "transferees entierement noires -> marqueur pose, "
                         "RELAUNCH the application to switch to software decoding");
                }
            }
        }
        if (trc < 0) {
            d->stats.decode_errors++;
        } else if (d->on_frame) {
            d->on_frame(d->sw_frame->width, d->sw_frame->height,
                        d->sw_frame->data[0], d->sw_frame->linesize[0],
                        d->sw_frame->data[1], d->sw_frame->linesize[1],
                        d->sw_frame->data[2], d->sw_frame->linesize[2],
                        d->sw_frame->format,
                        d->frame->pts,
                        d->user);
            av_frame_unref(d->sw_frame);
        }
    }
}
#endif /* __SWITCH__ */

/* Checks that a picture rendered on the GPU is still usable, and switches to
 * software if the hardware has failed too often.
 *
 * The fallback is STICKY: past HW_FAIL_THRESHOLD consecutive failures we set
 * `hw_fallback_done` and never retry for the rest of the session. That is what
 * saved the Switch when the `envideo` generation returned rc=0 with empty
 * surfaces - without it, every picture cost the nvgpu watchdog's timeout.
 *
 * Returns false when the current picture must be abandoned; the caller then
 * moves on to the next. Extracted from flush_access_unit on 2026-08-25. */
#if !defined(__SWITCH__)   /* called only by the non-Switch path */
static bool hw_frame_still_usable(h264_decoder *d, bool is_gpu_format)
{
    if (d->hwaccel_active && is_gpu_format) {
        /* One retry on a transient failure (= GPU busy with another process, an
         * intermittent driver). If the 2nd attempt passes we reset the streak; if
         * it fails again we bump it. */
        int trc = av_hwframe_transfer_data(d->sw_frame, d->frame, 0);
        if (trc < 0) {
            /* One immediate retry - a momentary GPU busy */
            av_frame_unref(d->sw_frame);
            trc = av_hwframe_transfer_data(d->sw_frame, d->frame, 0);
        }
        if (trc < 0) {
            d->stats.decode_errors++;
            d->hw_fail_streak++;
            /* Log the first 5 failures, then 1 in every 60 */
            if (d->hw_fail_streak <= 5 || d->hw_fail_streak % 60 == 0) {
                char err[128] = {0};
                av_strerror(trc, err, sizeof(err));
                wlog("h264: hwframe_transfer FAIL rc=%d (%s) streak=%d",
                     trc, err, d->hw_fail_streak);
            }
            /* Sticky fallback: past the threshold, disable the hwaccel path.
             * The decoder context stays configured for hwaccel; the following
             * frames will still arrive in GPU memory and be dropped by this
             * check, until libavcodec decides to fall back on its own (= rare) or
             * the session restarts.
             * In practice: the user sees a frozen picture plus one explicit log
             * line -> relaunch with SHADOW_HWACCEL=0 (= the stable CPU path). */
            if (d->hw_fail_streak >= HW_FAIL_THRESHOLD && !d->hw_fallback_done) {
                d->hw_fallback_done = true;
                d->hwaccel_active = false;
                /* Drop a marker for the next session - auto-skip hwaccel
                 * without asking the user to set SHADOW_HWACCEL=0 by hand. */
                FILE *mf = fopen(HW_MARKER_PATH, "wb");
                if (mf) {
                    fprintf(mf, "%d consecutive hwframe_transfer fails\n",
                            d->hw_fail_streak);
                    fclose(mf);
                    wlog("h264: %d hwframe_transfer fails — hwaccel disabled "
                         "session + marker %s written for the next session. Delete "
                         "the file to retry.",
                         d->hw_fail_streak, HW_MARKER_PATH);
                } else {
                    wlog("h264: %d hwframe_transfer fails — hwaccel disabled "
                         "session (marker write %s FAIL — set SHADOW_HWACCEL=0 manuellement).",
                         d->hw_fail_streak, HW_MARKER_PATH);
                }
            }
            av_frame_unref(d->frame);
            return false;
        }
        /* Transfer OK -> reset the streak (= healthy GPU) */
        d->hw_fail_streak = 0;
        d->hw_active = true;   /* K18: a hardware picture really did get through */
        if (d->on_frame) {
            d->on_frame(d->sw_frame->width, d->sw_frame->height,
                        d->sw_frame->data[0], d->sw_frame->linesize[0],
                        d->sw_frame->data[1], d->sw_frame->linesize[1],
                        d->sw_frame->data[2], d->sw_frame->linesize[2],
                        d->sw_frame->format,
                        d->frame->pts,
                        d->user);
        }
        av_frame_unref(d->sw_frame);
        av_frame_unref(d->frame);
        return false;
    }
    return true;
}
#endif /* !__SWITCH__ */

static void flush_access_unit(h264_decoder *d) {
    if (!d || !d->au_active || d->au_len == 0) return;

    av_packet_unref(d->pkt);
    d->pkt->data = d->au_buf;
    d->pkt->size = (int)d->au_len;
    d->pkt->pts  = d->au_pts;
    d->pkt->dts  = d->au_pts;

    /* G39 2026-08-22 - DIAGNOSTIC: captures the EXACT access unit that triggers a
     * erreur libavcodec (decode_slice_header / Frame num change), invisible hors
     * line (the same stream offline = 0 errors). g_bitstream_errors (a static in
     * this file) is incremented by av_log_cb during send_packet. We compare
     * before and after, and write the offending AU plus its frame number for
     * analysis.
     * S'ecrit UNIQUEMENT sur erreur, plafonne a 40 fichiers. SHADOW_DUMP_ERR_AU=0
     * desactive. */
    uint32_t g39_err_before = g_bitstream_errors;

    int rc = avcodec_send_packet(d->ctx, d->pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
        d->stats.decode_errors++;
        if (d->stats.decode_errors < 5) {
            char err[128] = {0};
            av_strerror(rc, err, sizeof(err));
            wlog("h264: send_packet rc=%d (%s) au_len=%zu", rc, err, d->au_len);
        }
        if (d->stats.decode_errors == 50) {
            wlog("h264: 50 errors, flushing decoder + re-gating");
            avcodec_flush_buffers(d->ctx);
            d->ready_to_decode = false;
        }
    }
    /* G39: did the current AU cause an error? If so, capture it. */
    if (g_bitstream_errors > g39_err_before) {
        static int g_dump_err_au = -1;
        static int g_err_au_count = 0;
        if (g_dump_err_au < 0) {
            const char *e = getenv("SHADOW_DUMP_ERR_AU");
            g_dump_err_au = e ? atoi(e) : 1;
        }
        if (g_dump_err_au && g_err_au_count < 40) {
            char path[512];
            snprintf(path, sizeof(path), "%serr_au_%03d_f%u.h264",
                     SHADOW_DATA_DIR, g_err_au_count, g_au_flush_index);
            FILE *ef = fopen(path, "wb");
            if (ef) {
                /* prefix the last known SPS/PPS so the AU is
                 * decodable standalone offline (P-frames carry none). */
                if (d->err_sps_len) fwrite(d->err_sps, 1, d->err_sps_len, ef);
                if (d->err_pps_len) fwrite(d->err_pps, 1, d->err_pps_len, ef);
                fwrite(d->au_buf, 1, d->au_len, ef);
                fclose(ef);
                wlog("h264: [G39] AU fautif #%d frame=%u len=%zu -> %s "
                     "(%u erreurs cumul)", g_err_au_count, g_au_flush_index,
                     d->au_len, path, g_bitstream_errors);
            }
            g_err_au_count++;
        }
    }
    g_au_flush_index++;

    d->pkt->data = NULL;
    d->pkt->size = 0;

    // Pull frames
    while (1) {
        rc = avcodec_receive_frame(d->ctx, d->frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            d->stats.decode_errors++;
            break;
        }
        d->stats.frames_decoded++;
        d->stats.last_width  = d->frame->width;
        d->stats.last_height = d->frame->height;
        /* COL1 - read on the decoded frame, before any hardware transfer:
         * `av_hwframe_transfer_data` copies the pixels, not these fields. */
        publish_colorimetry(d->frame);
        if (d->stats.frames_decoded <= 3) {
            wlog("h264: frame %ux%u linesize=[%d,%d,%d] pict_type=%d flags=0x%x fmt=%d (%s)%s",
                 d->frame->width, d->frame->height,
                 d->frame->linesize[0], d->frame->linesize[1], d->frame->linesize[2],
                 d->frame->pict_type, d->frame->flags,
                 (int)d->frame->format,
                 av_get_pix_fmt_name(d->frame->format) ? av_get_pix_fmt_name(d->frame->format) : "?",
                 d->frame->hw_frames_ctx ? " [HARDWARE]" : "");
        }
        int hard_err = d->frame->decode_error_flags & 0x3;
        bool flag_corrupt = (d->frame->flags & AV_FRAME_FLAG_CORRUPT) != 0;
        if (hard_err || flag_corrupt) {
            d->stats.decode_errors++;
            if (d->stats.decode_errors < 20) {
                wlog("h264: dropping hard-corrupt frame (err_flags=0x%x flags=0x%x)",
                     d->frame->decode_error_flags, d->frame->flags);
            }
            av_frame_unref(d->frame);
            continue;
        }

        // Hwaccel actif Linux/Windows (CUDA/VAAPI/VDPAU) : frame en GPU memory
        // -> av_hwframe_transfer_data into sw_frame (= CPU NV12) before
        // on_frame. Without it, on_frame receives device pointers and
        // stream_view drops them silently.
        //
        // 2026-05-18: we enter this path when the frame format is GPU-side, with
        // checker hwaccel_active. Pourquoi : post-sticky-disable (= hw_fail_streak
        // exceeded), hwaccel_active==false but the decoder context stays
        // configured for hwaccel -> the following frames still arrive in GPU
        // memory. If we skipped this block we would fall into the CPU path with
        // frame->data[] = device pointers -> stream_view crashes. With
        // hw_fallback_done=true we drop the frame cleanly (= the picture freezes
        // deliberately, the log message is already visible).
#if !defined(__SWITCH__)
        /* === K17 2026-08-28 - "UNKNOWN FORMAT" IS NOT "GPU PICTURE" ===
         *
         * This test said: anything that is neither YUV420P nor NV12 lives on the
         * GPU. That is false for any SOFTWARE format outside that list of two -
         * and 4:4:4 is one. Measured consequence: CUDA acceleration fails on
         * 4:4:4 (as it does in the official client), so `hw_fallback_done` becomes
         * true, so EVERY `yuv444p` picture fell into the "silent drop" below.
         * 2258 pictures decoded without a single error, and not one delivered.
         *
         * The SWITCH path had already learned the lesson (S16, a few lines
         * below): "`hw_frames_ctx` alone - the format constant depends on the
         * headers' version and must no longer serve as a test". The desktop path
         * had never applied it. We align: a picture lives on the GPU if and only
         * if it carries a hardware frames context. */
        bool is_gpu_format = (d->frame->hw_frames_ctx != NULL);
        if (is_gpu_format && d->hw_fallback_done) {
            /* Hwaccel disabled but the decoder still produces GPU frames - silent drop */
            av_frame_unref(d->frame);
            continue;
        }
        if (!hw_frame_still_usable(d, is_gpu_format)) continue;
#endif

        // Switch NVTEGRA : transfer NVDEC→CPU via VIC engine (~11ms 1080p).
        // Offloaded to the async worker to free the receive thread.
#ifdef __SWITCH__
        /* === S9 2026-08-22 - TRANSFER EVERY HARDWARE PICTURE, NOT ONLY NVTEGRA ===
         *
         * Console symptom: "Waiting for video" while decoding is running
         * (1280x720 pictures produced) and 6240 packets have arrived.
         * Cause: this block only acted when the format was EXACTLY
         * AV_PIX_FMT_NVTEGRA. But the negotiation had switched to another
         * hardware format (log: "NVTEGRA not in pix_fmts list, falling back to
         * ..."). The picture therefore went straight to the callback, with GPU
         * pointers and a format `pushYuvFrame` REFUSES (it only accepts YUV420P
         * and NV12): every picture was silently dropped -> an eternal waiting
         * screen.
         * We now rely on `hw_frames_ctx`, which reliably says "this picture
         * lives on the GPU", whatever format was chosen. */
        /* S16: `hw_frames_ctx` alone - the format constant depends on the
         * headers' version (see above) and must no longer serve as a test. */
        bool est_materielle = (d->frame->hw_frames_ctx != NULL);
        if (est_materielle) {
            hw_deliver_frame(d);
        } else
#endif /* __SWITCH__ */
        if (d->on_frame) {
            // Software decode path: the frame is already CPU YUV420P
            d->on_frame(d->frame->width, d->frame->height,
                        d->frame->data[0], d->frame->linesize[0],
                        d->frame->data[1], d->frame->linesize[1],
                        d->frame->data[2], d->frame->linesize[2],
                        d->frame->format,
                        d->frame->pts,
                        d->user);
        }
        av_frame_unref(d->frame);
    }

    d->au_len = 0;
    d->au_active = false;
}

// Append a NAL to the access-unit buffer. On a PTS change: flush the previous
// AU. `nal_with_header` must start with the NAL header (1 byte).
static void feed_nal_to_decoder(h264_decoder *d, const uint8_t *nal_with_header,
                                  size_t nal_len, int64_t pts) {
    if (!d || !nal_with_header || nal_len == 0) return;

    /* === K16 2026-08-28 - THE GATE WAS CLASSIFYING THE NALs AS H.264 ===
     *
     * `b0 & 0x1F` is the H.264 rule. Applied to HEVC, whose header is two bytes
     * and whose type lives on six bits from bit 1, it returns anything at all:
     *     VPS (0x40) -> 0    SPS (0x42) -> 2    PPS (0x44) -> 4
     *     IDR (0x26) -> 6                       TRAIL_R (0x02) -> 2
     * The parameter sets were therefore rejected (the gate waits for a type 5
     * that NEVER exists in HEVC), and only the IDR got through - because 6
     * happens to be "SEI" in H.264. The decoder received a slice WITHOUT its
     * parameter sets: "Invalid data found when processing input", and 85 bytes
     * missing from the access unit.
     *
     * The HEVC types that matter here: VPS 32, SPS 33, PPS 34, AUD 35, SEI 39
     * and 40; the instantaneous-refresh pictures run from 16 (BLA_W_LP) to 21
     * (CRA_NUT), including IDR_W_RADL 19 and IDR_N_LP 20. */
    const int hevc = d->hevc;
    const uint8_t nal_type = hevc ? (uint8_t)((nal_with_header[0] >> 1) & 0x3F)
                                  : (uint8_t)(nal_with_header[0] & 0x1F);
    /* G39: cache the SPS/PPS (with start code) to prefix a dumped offending AU. */
    const uint8_t T_SPS = hevc ? 33 : 7;
    const uint8_t T_PPS = hevc ? 34 : 8;
    if (nal_type == T_SPS && nal_len + 4 <= sizeof(d->err_sps)) {
        memcpy(d->err_sps, kAnnexBStartCode, 4);
        memcpy(d->err_sps + 4, nal_with_header, nal_len);
        d->err_sps_len = nal_len + 4;
    } else if (nal_type == T_PPS && nal_len + 4 <= sizeof(d->err_pps)) {
        memcpy(d->err_pps, kAnnexBStartCode, 4);
        memcpy(d->err_pps + 4, nal_with_header, nal_len);
        d->err_pps_len = nal_len + 4;
    }
    /* An instantaneous reference picture: type 5 in H.264, 16 to 21 in HEVC. */
    const bool is_idr = hevc ? (nal_type >= 16 && nal_type <= 21)
                              : (nal_type == 5);
    if (d->stats.nals_total < 30 || is_idr || nal_type == T_SPS) {
        wlog("h264: NAL type=%u len=%zu nals_total=%u",
             nal_type, nal_len, d->stats.nals_total);
    }
    if (nal_type == T_SPS || nal_type == T_PPS
            || (!hevc && nal_type == 6) || (hevc && nal_type == 32)) {
        char hex[1024];
        size_t print_len = nal_len < 256 ? nal_len : 256;
        for (size_t i = 0; i < print_len; i++) {
            snprintf(hex + i*2, sizeof(hex) - i*2, "%02x", nal_with_header[i]);
        }
        wlog("h264: %s NAL DUMP (%zu bytes): %s",
             nal_type == 7 ? "SPS" : (nal_type == 8 ? "PPS" : "SEI"),
             nal_len, hex);
    }
    /* Parameter sets and metadata ALWAYS get through, otherwise the decoder
     * never has anything to configure its context with. In HEVC one must count
     * the
     * VPS (32), absent from H.264 - forgetting it was enough to break everything. */
    const bool is_config = hevc
        ? (nal_type == 32 || nal_type == 33 || nal_type == 34
           || nal_type == 35 || nal_type == 39 || nal_type == 40)
        : (nal_type == 6 || nal_type == 7 || nal_type == 8 || nal_type == 9);
    if (!is_config) {
        if (!d->ready_to_decode) {
            if (is_idr) {
                d->ready_to_decode = true;
                wlog("h264: gate opened on an IDR (type %u)", nal_type);
            } else {
                return;  // skip non-IDR frames until the first IDR
            }
        }
    }

    // A PTS different from the current buffer? Flush the previous one (= a new picture).
    if (d->au_active && d->au_pts != pts) {
        flush_access_unit(d);
    }
    // Append start code + NAL au buffer
    if (d->au_len + 4 + nal_len > AU_BUF_MAX) {
        wlog("h264: AU buffer overflow au_len=%zu nal_len=%zu", d->au_len, nal_len);
        d->au_len = 0;
        d->au_active = false;
        return;
    }
    memcpy(d->au_buf + d->au_len, kAnnexBStartCode, 4);
    d->au_len += 4;
    memcpy(d->au_buf + d->au_len, nal_with_header, nal_len);
    d->au_len += nal_len;
    d->au_pts = pts;
    d->au_active = true;
}

// === Annex-B feed (Shadow native streaming) ============================
// Splits a `[start_code][NAL][start_code][NAL]...` buffer and calls
// feed_nal_to_decoder for each NAL. Supports 3-byte and 4-byte start codes.
void h264_decoder_feed_annexb(h264_decoder *d, const uint8_t *buf, size_t len,
                                uint32_t pts) {
    if (!d || !buf || len < 4) return;
    int64_t pts64 = (int64_t)pts;

#if defined(__vita__) || defined(__psp2__)
    /* The hardware takes the access unit WHERE IT IS - `vid_reasm` owns that
     * buffer and keeps it alive across this call, so there is no copy on this
     * path at all. One unit in, at most one picture out. */
    {
        h264_vita_picture pic;
        const int rc = h264_vita_decode(d->vita, buf, len, pts64, &pic);
        if (rc < 0) {
            d->stats.decode_errors++;
            if (d->stats.decode_errors < 5)
                wlog("h264/vita: decode FAIL (unit of %zu B)", len);
            return;
        }
        if (rc == 0) return;                 /* consumed, no picture yet */

        d->stats.frames_decoded++;
        d->stats.last_width  = pic.width;
        d->stats.last_height = pic.height;
        if (d->on_frame) {
            /* NV12: the chroma plane comes through `data_u`, `data_v` stays
             * NULL - the contract `h264_decoder.h` states, and what the view
             * already expects (`is_nv12`). 23 = AV_PIX_FMT_NV12. */
            d->on_frame(pic.width, pic.height,
                        pic.y,  pic.pitch,
                        pic.uv, pic.pitch,
                        NULL,   0,
                        23, pic.pts, d->user);
        }
        return;
    }
#endif

    /* H1 V10 dbg: dump the raw H.264 stream so it can be decoded offline with
     * the ffmpeg CLI, to confirm whether the bug is in the bytestream or in our
     * libavcodec wiring. Toggle SHADOW_DUMP_H264=1. */
    static int g_dump_h264 = -1;
    static FILE *g_dump_h264_f = NULL;
    if (g_dump_h264 < 0) {
        const char *e = getenv("SHADOW_DUMP_H264");
        g_dump_h264 = e ? atoi(e) : 0;
        if (g_dump_h264) {
            /* Absolute path: it used to be relative to the current directory, so
             * the file landed wherever the binary had been launched from - and
             * perdait selon d'ou on demarrait. */
            char path[512];
            snprintf(path, sizeof(path), "%sstream.h264", SHADOW_DATA_DIR);
            g_dump_h264_f = fopen(path, "wb");
            wlog("h264: dumping the stream to %s (%s)", path,
                 g_dump_h264_f ? "opened" : "ECHEC");
        }
    }
    if (g_dump_h264 && g_dump_h264_f) {
        fwrite(buf, 1, len, g_dump_h264_f);
        fflush(g_dump_h264_f);
    }

    size_t i = 0;
    size_t nal_start = 0;
    bool   have_nal = false;

    while (i + 3 <= len) {
        bool sc4 = (i + 4 <= len)
                    && buf[i] == 0 && buf[i+1] == 0
                    && buf[i+2] == 0 && buf[i+3] == 1;
        bool sc3 = !sc4
                    && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1;
        if (sc4 || sc3) {
            if (have_nal) {
                size_t nal_len = i - nal_start;
                if (nal_len > 0) {
                    feed_nal_to_decoder(d, buf + nal_start, nal_len, pts64);
                    d->stats.nals_total++;
                    d->stats.single_nal_count++;
                }
            }
            i += sc4 ? 4 : 3;
            nal_start = i;
            have_nal = true;
            continue;
        }
        i++;
    }
    if (have_nal && nal_start < len) {
        size_t nal_len = len - nal_start;
        feed_nal_to_decoder(d, buf + nal_start, nal_len, pts64);
        d->stats.nals_total++;
        d->stats.single_nal_count++;
    }

    /* The Annex-B buffer holds the complete access unit - flush immediately
     * (without waiting for a PTS change). */
    if (d->au_active) {
        flush_access_unit(d);
    }
}

void h264_decoder_get_stats(h264_decoder *d, h264_decoder_stats_t *out) {
    if (!d || !out) return;
    *out = d->stats;
    out->async_processed = d->async.frames_processed;
    out->async_dropped   = d->async.frames_dropped;
    out->bitstream_errors = g_bitstream_errors;
}
