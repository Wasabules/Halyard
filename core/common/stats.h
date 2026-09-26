// Global runtime stats, written by the session path and read by the UI overlay.
//
// === AUD-INS-3 2026-09-11 - THREE WRITER THREADS, ONE PER FIELD GROUP ===
//
// This header used to say that only the session's main loop writes. In fact the
// decode thread wrote too, and both published by whole-struct get / modify /
// publish, each writing back its snapshot of the OTHER's fields. A publish that
// landed between the other's get and publish rolled fresher values back:
// RateMeter read the step back as a restart (L21). And because the decode thread
// was the only writer of the audio counters, a video outage froze them while
// the sound played.
//
// Writers now call session_stats_merge() with their own group only:
//   NET   - the session loop's 250 ms block (ctrl_session.c)
//   VIDEO - the decode thread (glue_decode_frame)
//   AUDIO - the session thread, from the glue's on_audio
//           (SHADOW_AUDIO_STATS_TICK=0 hands it back to the decode thread)
// Reading happens on the Borealis thread (StreamView::draw, net_test). The mutex
// makes every copy atomic; one writer per group makes it lossless.

#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // RTP (network)
    uint32_t rtp_video_packets;
    uint32_t rtp_audio_packets;
    uint32_t rtp_other;

    // Cumulative bytes - used by halyard-cli to compute the bitrate.
    uint64_t rtp_video_bytes;
    uint64_t rtp_audio_bytes;

    /* === S117 2026-09-03 - LOSS, AT LAST READABLE FROM OUTSIDE ===
     *
     * These counters had existed for months inside `ctrl_session_stats` and
     * reached the PUBLIC structure by no path at all: the panel showed rates and
     * not one integrity figure. A user watching their picture degrade while
     * playing therefore could not see their own loss - they had to quit, pull
     * the log over FTP and read a statistics line. This is exactly the family
     * CLAUDE.md names, seen from the other end: the counter was WRITTEN, and
     * nobody could READ it.
     *
     * WHAT `chunks_missing / chunks_expected` IS WORTH, AND ITS LIMIT. It is a
     * LOWER BOUND on real loss, and the code says so: a picture whose opening
     * chunk (index 0) is lost is never opened, so it enters NEITHER of these two
     * counters (vid_reasm.c, the `return` that precedes any accounting). Its
     * chunks land in `chunks_orphan_lost`, published here beside them for that
     * precise reason: together they say what either alone hides. */
    uint32_t chunks_expected;      /* chunks expected, cumulative over the session */
    uint32_t chunks_missing;       /* missing at flush - A LOWER BOUND */
    uint32_t chunks_orphan_lost;   /* chunks of pictures never opened */
    uint32_t frames_trunc;         /* pictures dropped, or emitted truncated */
    uint32_t kernel_drops;         /* SO_RXQ_OVFL: our own buffers */
    bool     kernel_drops_valid;   /* ING-1: false where the platform cannot count them (Windows, Switch) */

    /* One-way clock offset, in microseconds (vid_reasm: `g_delay_offset_us`).
     * Its ABSOLUTE value means nothing - the two clocks are independent. It is
     * its DRIFT that speaks: when it climbs, a queue is filling somewhere along
     * the path, and that is the signal saying we are asking for more than the
     * link carries. */
    int64_t  delay_offset_us;

    /* === S118 - THE ROUND TRIP TO THE VM, IN MICROSECONDS ===
     *
     * The ONLY network latency this client measures. The per-stage report counts
     * twelve measurements and eleven are local; the twelfth, the spread of the
     * UDP burst, measures jitter and never delay - a link at a steady 200 ms
     * reads there exactly like a link at 5 ms.
     *
     * Measured on the control channel's heartbeat, which we already send every
     * 500 ms and whose sequence number the server echoes.
     *
     * TO SAY ON SCREEN: this is an application-level TCP+TLS round trip to the
     * VM, NOT the latency of the video path, which is UDP and may behave
     * differently. Writing "latency" full stop would promise the other one. */
    uint32_t ctrl_rtt_us;          /* the most recent */
    uint32_t ctrl_rtt_avg_us;
    uint32_t ctrl_rtt_p90_us;      /* matters more than the mean: cf L5 */
    uint32_t ctrl_rtt_jitter_us;

    // Video decode (h264)
    uint32_t h264_frames_decoded;
    uint32_t h264_decode_errors;
    uint32_t h264_async_processed;
    uint32_t dec_queue_dropped;    /* CONC-4: [G38] decode-queue drops, this session */
    int      h264_width;
    int      h264_height;

    // Audio (opus)
    uint32_t opus_packets;
    uint32_t opus_decoded;
    uint32_t opus_pushed;
    uint32_t opus_errors;
    uint32_t opus_dup_skipped;  /* duplicates dropped: the server sends every frame twice */
    uint32_t opus_invalid;      /* invalid Opus framing, dropped before decoding */
    uint32_t opus_ring_full;    /* bursts the output buffer could not absorb */
    /* AUD-DEDUP-3 2026-09-11 - audio frames that never arrived in EITHER copy
     * (the server sends each one twice). A FINAL count: a frame is lost once 64
     * later frames went by without it (streaming/audio_loss.h), so a late frame
     * that still plays is never counted. Counted at ingress by the session loop
     * and published in the NET group, not with the other opus_* fields: it must
     * keep moving through a video outage. Loss AFTER the 2x redundancy - a
     * single lost copy reads 0. */
    uint32_t opus_lost;


    // Session-relative seconds elapsed (since the session started)
    int      session_seconds;

    // Freeze detection: seconds since the last video datagram arrived (UDP or
    // TCP). 0 = stream alive. Written by the session loop at 4 Hz (VI1,
    // 2026-09-11 - it had had no writer since S112, so the banner never
    // showed); the stream view draws a banner from 5 s. There is no automatic
    // reconnection on the native path.
    int      rtp_video_stuck_secs;
} session_stats_t;

/* AUD-INS-3 - the field groups, one writer thread each. `opus_dup_skipped` and
 * `opus_lost` are counted by the receive loop, so they are NET, not AUDIO. The
 * fields in no group (rtp_other, h264_async_processed, h264_width/height)
 * have no writer on the native path; only
 * session_stats_publish() reaches them. */
enum {
    SESSION_STATS_NET   = 1u,
    SESSION_STATS_VIDEO = 2u,
    SESSION_STATS_AUDIO = 4u,
};

// Snapshot of the stats. Returns 0 and zeroes when no session is active.
void session_stats_get(session_stats_t *out);

/* Copies ONLY the fields of `groups` from `in`, under the lock. The caller must
 * be that group's one writer thread; the other fields of `in` are ignored. */
void session_stats_merge(const session_stats_t *in, unsigned groups);

/* Replaces the whole struct. Since AUD-INS-3 no writer of the application calls
 * it - each merges its own group. tests/test_stats.c keeps it to replay the old
 * pattern as its counter-case. Never pair it with session_stats_get() on two
 * threads: that is the lost update AUD-INS-3 removed. */
void session_stats_publish(const session_stats_t *in);

/* L17 - clears the published measurements. Call at the START of a session:
 * without it, the rows the new session does not republish keep the previous
 * one's values, and the panel lies without saying so. */
void session_stats_reset(void);

#ifdef __cplusplus
}
#endif
