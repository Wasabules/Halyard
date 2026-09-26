/* ctrl_session_glue - convenience wrapper around ctrl_session_run() that
 * sets up the H.264 decoder + stream_view_push_yuv pipeline.
 *
 * The caller only passes the connection params (vm_host, jwt, etc.) and the
 * abort flag. The glue takes care of:
 *   - h264_decoder_create + on_frame -> stream_view_push_yuv
 *   - ctrl_session video callback -> h264_decoder_feed_annexb
 *   - automatic cleanup on the way out
 *
 * Meant to be used from connecting_activity.cpp.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *vm_host;          /* conn.ip */
    const char *streaming_token;  /* msess.streaming_token */
    const char *client_id;        /* msess.id */
    const char *bearer_jwt;       /* creds.main_jwt */
    int display_width;            /* default 1920 */
    int display_height;           /* default 1080 */
    int port_base;                /* = atoi(vm.port) + 7000 ; obligatoire */
    /* Q1 2026-05-18 — Quality params (forwarded to ctrl_session_params). */
    uint32_t max_bitrate_mbps;    /* default 0 = use the hardcoded 20 Mbps */
    float    target_fps;          /* default 0 = use the hardcoded 143.85 */
    int      video_tcp;           /* B4 - 0 = UDP, 1 = TCP (reliability) */
    volatile int *abort_flag;
} ctrl_session_glue_params;

typedef struct {
    bool     bootstrap_ok;
    uint32_t udp_video_pkts;
    /* Channel `:base+30` - cursor AND sound (KB.md §3.26). Surfaced this far up for
     * the test loop: this is the counter that tells whether the channel ever
     * started, and characterising its intermittence means reading it session
     * after session. */
    uint32_t udp_cursor_pkts;
    uint64_t udp_audio_bytes;
    uint64_t udp_video_bytes;
    uint32_t frames_decoded;       /* depuis ctrl_session (= frames assembled) */
    uint32_t frames_displayed;     /* depuis h264_decoder (= YUV emitted) */
    uint32_t dec_dropped;          /* CONC-4: pictures dropped by the full decode queue ([G38]) */
    uint32_t decrypt_ok;
    uint32_t decrypt_fail;
    /* A1 2026-05-18: NAL stats for automation (health-check thresholds).
     * Lets us measure the "bottom slice ratio" without grepping the log:
     *   bottom_ratio = nal_bottom / (nal_top + nal_bottom)
     * Desktop ref ~= 50% (= V11 PARITY_RAW), image bug = ratio << 50% */
    uint32_t nal_top;              /* slices first_mb=0 */
    uint32_t nal_bottom;           /* slices first_mb!=0 */
    uint32_t nal_idr_top;
    uint32_t nal_idr_bottom;
    uint32_t parity_skip;
    uint32_t parity_decrypt_ok;    /* RE8 : parity chunks dont chacha20 decrypt OK */
    uint32_t parity_decrypt_fail;  /* RE8 : parity chunks dont tag poly1305 fail */
    uint32_t reasm_abandoned;
    /* V10 2026-08-28 - the LOSS accounting, invisible from the headless binary
     * until now: bench_runner.py was comparing sessions on numbers that never
     * moved. */
    uint32_t chunks_missing;
    uint32_t chunks_expected;
    uint32_t frames_miss1;
    uint32_t chunks_orphan;
    uint32_t chunks_orphan_dup;
    uint32_t chunks_orphan_lost;
    uint32_t chunks_orphan_stale;
    uint32_t frames_dropped_trunc;
    uint32_t incomplete_at_flush;
    uint32_t nack_sent;
    uint32_t chunks_redundant;   /* V10b: chunks received for a slot already filled
                                  * = network duplicate OR a reply to our `rG`. The only
                                  * direct proof that a retransmission does arrive. */
    int      session_seconds;
    int      exit_reason;
} ctrl_session_glue_stats;

/* Blocking. Returns true if bootstrap+stream succeeded. */
/* Gamepad experiment: also register the input channel `:base+13`, the way video
 * and cursor already do. Must be set BEFORE ctrl_session_glue_run(). Goes
 * through the glue rather than the internal header: the UI must only ever know
 * this boundary. */
void ctrl_session_glue_set_udp_register_input(int on);

/* K18 - what the decoder actually DOES, for the diagnostics panel: "H.264" /
 * "H.265" / "AV1", and 1 if a hardware-decoded frame was really delivered.
 * Returns the em-dash placeholder and 0 when there is no session. */
const char *ctrl_session_glue_codec(void);
/* Audio codec ACTUALLY received: "Opus" or "FLAC". Negotiated, hence measured. */
const char *ctrl_session_glue_codec_audio(void);
int         ctrl_session_glue_hw(void);

bool ctrl_session_glue_run(const ctrl_session_glue_params *p,
                              ctrl_session_glue_stats *out_stats);

#ifdef __cplusplus
}
#endif
