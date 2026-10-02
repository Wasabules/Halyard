// Global stats — cf. stats.h.

#include "stats.h"
#include <pthread.h>
#include <string.h>

static session_stats_t   g_stats = {0};
static pthread_mutex_t  g_stats_mtx = PTHREAD_MUTEX_INITIALIZER;

void session_stats_get(session_stats_t *out) {
    if (!out) return;
    pthread_mutex_lock(&g_stats_mtx);
    *out = g_stats;
    pthread_mutex_unlock(&g_stats_mtx);
}

/* === L17 2026-08-29 - ONE SESSION = ONE SET OF MEASUREMENTS ===
 *
 * `g_stats` is a file-scope global that NOTHING reset. Consequence reported
 * from the console: on the second stream of the same launch, the panel shows
 * nonsense values on some rows - precisely the ones the new session path does
 * not republish. They then keep the PREVIOUS session's value, indefinitely, with
 * nothing to signal it.
 *
 * This is the defect family this repo knows by heart - session state in a
 * variable that outlives the session - and it has already produced the black
 * screen from the 3rd session on, sound audible exactly once, mute hardware
 * detectors and a resolution never re-announced. `latency_reset_session` already
 * existed for the same reason, in the same place; this one was missing.
 *
 * The remedy is the same: the start of a session clears. */
void session_stats_reset(void) {
    pthread_mutex_lock(&g_stats_mtx);
    memset(&g_stats, 0, sizeof(g_stats));
    pthread_mutex_unlock(&g_stats_mtx);
}

void session_stats_publish(const session_stats_t *in) {
    if (!in) return;
    pthread_mutex_lock(&g_stats_mtx);
    g_stats = *in;
    pthread_mutex_unlock(&g_stats_mtx);
}

/* === AUD-INS-3 2026-09-11 - ONE WRITER PER GROUP, NO SNAPSHOT REPUBLISHED ===
 *
 * The decode thread and the session loop each did get / modify / publish on
 * the whole struct, so each wrote back ITS copy of the other's fields. A
 * publish landing inside the other's window rolled fresher values back. Rare
 * at the real cadences (a few microseconds each, at ~2 Hz and 4 Hz: about one
 * collision per 57-64 h of 60 fps video), but a step back is read as a restart
 * by RateMeter (L21), and two threads flat out lose thousands of updates a
 * second - tests/test_stats.c keeps that as its counter-case.
 *
 * A merge copies only the caller's group. With one writer thread per group, a
 * field can only ever be overwritten by a newer value from its own writer.
 * Only field copies under the lock: no I/O, no computation. */
void session_stats_merge(const session_stats_t *in, unsigned groups) {
    if (!in) return;
    pthread_mutex_lock(&g_stats_mtx);
    if (groups & SESSION_STATS_NET) {
        g_stats.rtp_video_packets    = in->rtp_video_packets;
        g_stats.rtp_video_bytes      = in->rtp_video_bytes;
        g_stats.rtp_audio_packets    = in->rtp_audio_packets;
        g_stats.rtp_audio_bytes      = in->rtp_audio_bytes;
        g_stats.opus_dup_skipped     = in->opus_dup_skipped;
        g_stats.opus_lost            = in->opus_lost;          /* AUD-DEDUP-3 */
        g_stats.chunks_expected      = in->chunks_expected;
        g_stats.chunks_missing       = in->chunks_missing;
        g_stats.chunks_orphan_lost   = in->chunks_orphan_lost;
        g_stats.frames_trunc         = in->frames_trunc;
        g_stats.kernel_drops         = in->kernel_drops;
        g_stats.kernel_drops_valid   = in->kernel_drops_valid;
        g_stats.delay_offset_us      = in->delay_offset_us;
        g_stats.ctrl_rtt_us          = in->ctrl_rtt_us;
        g_stats.ctrl_rtt_avg_us      = in->ctrl_rtt_avg_us;
        g_stats.ctrl_rtt_p90_us      = in->ctrl_rtt_p90_us;
        g_stats.ctrl_rtt_jitter_us   = in->ctrl_rtt_jitter_us;
        g_stats.session_seconds      = in->session_seconds;
        g_stats.rtp_video_stuck_secs = in->rtp_video_stuck_secs;
    }
    if (groups & SESSION_STATS_VIDEO) {
        g_stats.h264_frames_decoded = in->h264_frames_decoded;
        g_stats.h264_decode_errors  = in->h264_decode_errors;
        g_stats.dec_queue_dropped   = in->dec_queue_dropped;
    }
    if (groups & SESSION_STATS_AUDIO) {
        g_stats.opus_packets   = in->opus_packets;
        g_stats.opus_decoded   = in->opus_decoded;
        g_stats.opus_pushed    = in->opus_pushed;
        g_stats.opus_errors    = in->opus_errors;
        g_stats.opus_invalid   = in->opus_invalid;
        g_stats.opus_ring_full = in->opus_ring_full;
    }
    pthread_mutex_unlock(&g_stats_mtx);
}
