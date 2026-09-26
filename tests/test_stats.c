/* test_stats - `core/stats.c`, the snapshot the metrics panel reads.
 *
 * === WHAT THIS SUITE EXISTS TO PREVENT (AUD-INS-3, 2026-09-11) ===
 *
 * Two threads published into this struct by whole-struct get / modify /
 * publish: the decode thread (video counters, and the ONLY writer of the audio
 * counters) and the session loop (network counters, every 250 ms). Each wrote
 * back its snapshot of the OTHER's fields. When one publish fell between the
 * other's get and publish, fresher values were replaced by older ones: a
 * counter stepped BACK, and RateMeter took it for a restart and showed 0 (L21).
 *
 * The fix gives each field group ONE writer thread and a merge that copies only
 * that group. The counter-case is first run as the exact interleaving, with no
 * thread, then as the same pattern under two real threads.
 *
 * Built with -pthread; no other dependency (MinGW winpthreads, Linux). */
#include "../core/common/stats.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* A distinct value in every field, so that a stray write shows. */
static void canary(session_stats_t *s, uint32_t b)
{
    memset(s, 0, sizeof *s);
    s->rtp_video_packets = b + 1;  s->rtp_audio_packets = b + 2;  s->rtp_other = b + 3;
    s->rtp_video_bytes = b + 4;    s->rtp_audio_bytes = b + 5;
    s->chunks_expected = b + 6;    s->chunks_missing = b + 7;     s->chunks_orphan_lost = b + 8;
    s->frames_trunc = b + 9;       s->kernel_drops = b + 10;      s->kernel_drops_valid = (b & 1) != 0;
    s->delay_offset_us = b + 11;
    s->ctrl_rtt_us = b + 12;       s->ctrl_rtt_avg_us = b + 13;   s->ctrl_rtt_p90_us = b + 14;
    s->ctrl_rtt_jitter_us = b + 15;
    s->h264_frames_decoded = b + 16; s->h264_decode_errors = b + 17; s->h264_async_processed = b + 18;
    s->dec_queue_dropped = b + 19; s->h264_width = (int)b + 20;   s->h264_height = (int)b + 21;
    s->opus_packets = b + 22;      s->opus_decoded = b + 23;      s->opus_pushed = b + 24;
    s->opus_errors = b + 25;       s->opus_dup_skipped = b + 26;  s->opus_invalid = b + 27;
    s->opus_ring_full = b + 28;
    s->session_seconds = (int)b + 29; s->rtp_video_stuck_secs = (int)b + 30;
    s->opus_lost = b + 31;
}

static int net_eq(const session_stats_t *a, const session_stats_t *b)
{
    return a->rtp_video_packets == b->rtp_video_packets && a->rtp_video_bytes == b->rtp_video_bytes
        && a->rtp_audio_packets == b->rtp_audio_packets && a->rtp_audio_bytes == b->rtp_audio_bytes
        && a->opus_dup_skipped == b->opus_dup_skipped && a->opus_lost == b->opus_lost
        && a->chunks_expected == b->chunks_expected
        && a->chunks_missing == b->chunks_missing && a->chunks_orphan_lost == b->chunks_orphan_lost
        && a->frames_trunc == b->frames_trunc && a->kernel_drops == b->kernel_drops
        && a->kernel_drops_valid == b->kernel_drops_valid && a->delay_offset_us == b->delay_offset_us
        && a->ctrl_rtt_us == b->ctrl_rtt_us && a->ctrl_rtt_avg_us == b->ctrl_rtt_avg_us
        && a->ctrl_rtt_p90_us == b->ctrl_rtt_p90_us && a->ctrl_rtt_jitter_us == b->ctrl_rtt_jitter_us
        && a->session_seconds == b->session_seconds && a->rtp_video_stuck_secs == b->rtp_video_stuck_secs;
}
static int video_eq(const session_stats_t *a, const session_stats_t *b)
{
    return a->h264_frames_decoded == b->h264_frames_decoded
        && a->h264_decode_errors == b->h264_decode_errors && a->dec_queue_dropped == b->dec_queue_dropped;
}
static int audio_eq(const session_stats_t *a, const session_stats_t *b)
{
    return a->opus_packets == b->opus_packets && a->opus_decoded == b->opus_decoded
        && a->opus_pushed == b->opus_pushed && a->opus_errors == b->opus_errors
        && a->opus_invalid == b->opus_invalid && a->opus_ring_full == b->opus_ring_full;
}
static int ungrouped_eq(const session_stats_t *a, const session_stats_t *b)
{
    return a->rtp_other == b->rtp_other && a->h264_async_processed == b->h264_async_processed
        && a->h264_width == b->h264_width && a->h264_height == b->h264_height;
}

/* 1. A merge writes its own group and nothing else. */
static void isolation(void)
{
    static const unsigned G[3] = {SESSION_STATS_NET, SESSION_STATS_VIDEO, SESSION_STATS_AUDIO};
    static const char *name[3] = {"NET", "VIDEO", "AUDIO"};
    session_stats_t a, b, out;
    canary(&a, 1000);
    canary(&b, 5000);
    for (int g = 0; g < 3; g++) {
        char what[160];
        session_stats_reset();
        session_stats_publish(&a);
        session_stats_merge(&b, G[g]);
        session_stats_get(&out);
        snprintf(what, sizeof what, "merge(%s) writes every %s field", name[g], name[g]);
        CHECK((g == 0 ? net_eq(&out, &b) : g == 1 ? video_eq(&out, &b) : audio_eq(&out, &b)), what);
        snprintf(what, sizeof what, "merge(%s) leaves the other two groups' canaries untouched", name[g]);
        CHECK((g == 0 || net_eq(&out, &a)) && (g == 1 || video_eq(&out, &a))
              && (g == 2 || audio_eq(&out, &a)), what);
        snprintf(what, sizeof what, "merge(%s) leaves the ungrouped fields untouched", name[g]);
        CHECK(ungrouped_eq(&out, &a), what);
    }
    session_stats_reset();
    session_stats_publish(&a);
    session_stats_merge(&b, 0);
    session_stats_merge(NULL, SESSION_STATS_NET | SESSION_STATS_VIDEO | SESSION_STATS_AUDIO);
    session_stats_get(&out);
    CHECK(net_eq(&out, &a) && video_eq(&out, &a) && audio_eq(&out, &a) && ungrouped_eq(&out, &a),
          "merge with no group, or with NULL, changes nothing");
    session_stats_merge(&b, SESSION_STATS_NET | SESSION_STATS_VIDEO | SESSION_STATS_AUDIO);
    session_stats_get(&out);
    CHECK(net_eq(&out, &b) && video_eq(&out, &b) && audio_eq(&out, &b) && ungrouped_eq(&out, &a),
          "the three groups together: all grouped fields, still no ungrouped one");
}

/* 2. L17: the start of a session clears EVERYTHING, grouped or not. */
static void reset_clears(void)
{
    session_stats_t a, out, zero;
    canary(&a, 77);
    session_stats_publish(&a);
    session_stats_reset();
    session_stats_get(&out);
    memset(&zero, 0, sizeof zero);
    CHECK(memcmp(&out, &zero, sizeof zero) == 0,
          "L17: session_stats_reset zeroes every field, none survives into the next session");
}

/* 3. The exact interleaving, with no thread: deterministic. */
static void interleaving(void)
{
    session_stats_t g, s, out;

    /* The session loop publishes INSIDE the decode thread's window. */
    session_stats_reset();
    session_stats_get(&g);                                    /* decode thread: get */
    session_stats_get(&s); s.rtp_video_bytes = 250000; s.chunks_expected = 180;
    session_stats_publish(&s);                                /* session: a whole RMW */
    g.h264_frames_decoded = 30; g.opus_decoded = 25;
    session_stats_publish(&g);                                /* decode: publishes its stale copy */
    session_stats_get(&out);
    CHECK(out.rtp_video_bytes == 0 && out.chunks_expected == 0,
          "COUNTER-CASE (demonstration): the old get/modify/publish loses the session's "
          "update - the bytes step back from 250000 to 0");

    session_stats_reset();
    memset(&g, 0, sizeof g); g.h264_frames_decoded = 30;     /* decode: prepares its group */
    memset(&s, 0, sizeof s); s.rtp_video_bytes = 250000; s.chunks_expected = 180;
    session_stats_merge(&s, SESSION_STATS_NET);
    session_stats_merge(&g, SESSION_STATS_VIDEO);
    memset(&s, 0, sizeof s); s.opus_decoded = 25;
    session_stats_merge(&s, SESSION_STATS_AUDIO);
    session_stats_get(&out);
    CHECK(out.rtp_video_bytes == 250000 && out.chunks_expected == 180
          && out.h264_frames_decoded == 30 && out.opus_decoded == 25,
          "same interleaving with merges: both updates kept");

    /* The decode thread publishes INSIDE the session loop's window. */
    session_stats_reset();
    session_stats_get(&s);                                    /* session: get */
    session_stats_get(&g); g.h264_frames_decoded = 30; g.opus_decoded = 25;
    session_stats_publish(&g);                                /* decode: a whole RMW */
    s.rtp_video_bytes = 250000;
    session_stats_publish(&s);                                /* session: publishes its stale copy */
    session_stats_get(&out);
    CHECK(out.h264_frames_decoded == 0 && out.opus_decoded == 0,
          "COUNTER-CASE (demonstration): the reverse interleaving loses the decode "
          "thread's video AND audio counters");
}

/* 4-5. Two real writer threads and a reader. */
typedef struct {
    int old_pattern;
    volatile int stop;
    unsigned long long it_d, it_s, reads, regress, torn;
} stress_t;

static void *thread_d(void *p)                /* the decode thread: VIDEO */
{
    stress_t *x = (stress_t *)p;
    uint32_t h = 0;
    while (!__atomic_load_n(&x->stop, __ATOMIC_RELAXED)) {
        session_stats_t s;
        ++h;
        if (x->old_pattern) {
            session_stats_get(&s);
            s.h264_frames_decoded = h; s.h264_decode_errors = h / 3;
            sched_yield();                    /* widen the window the old code had */
            session_stats_publish(&s);
        } else {
            memset(&s, 0, sizeof s);
            s.h264_frames_decoded = h; s.h264_decode_errors = h / 3;
            session_stats_merge(&s, SESSION_STATS_VIDEO);
        }
        x->it_d++;
    }
    return NULL;
}

static void *thread_s(void *p)                /* the session thread: NET and AUDIO */
{
    stress_t *x = (stress_t *)p;
    uint32_t b = 0;
    while (!__atomic_load_n(&x->stop, __ATOMIC_RELAXED)) {
        session_stats_t s;
        ++b;
        if (x->old_pattern) {
            session_stats_get(&s);
            s.rtp_video_packets = b; s.rtp_video_bytes = (uint64_t)b * 1100; s.chunks_expected = b;
            s.opus_decoded = b; s.opus_packets = 2 * b;
            sched_yield();
            session_stats_publish(&s);
        } else {
            memset(&s, 0, sizeof s);
            s.rtp_video_packets = b; s.rtp_video_bytes = (uint64_t)b * 1100; s.chunks_expected = b;
            session_stats_merge(&s, SESSION_STATS_NET);
            memset(&s, 0, sizeof s);
            s.opus_decoded = b; s.opus_packets = 2 * b;
            session_stats_merge(&s, SESSION_STATS_AUDIO);
        }
        x->it_s++;
    }
    return NULL;
}

static void *thread_r(void *p)                /* the panel: every field must only go up */
{
    stress_t *x = (stress_t *)p;
    session_stats_t prev;
    memset(&prev, 0, sizeof prev);
    while (!__atomic_load_n(&x->stop, __ATOMIC_RELAXED)) {
        session_stats_t w;
        session_stats_get(&w);
        x->reads++;
        if (w.rtp_video_bytes < prev.rtp_video_bytes || w.chunks_expected < prev.chunks_expected
            || w.h264_frames_decoded < prev.h264_frames_decoded || w.opus_decoded < prev.opus_decoded)
            x->regress++;
        /* Fields a writer sets together must be seen together. */
        if (w.rtp_video_bytes != (uint64_t)w.rtp_video_packets * 1100
            || w.h264_decode_errors != w.h264_frames_decoded / 3
            || w.opus_packets != 2 * w.opus_decoded)
            x->torn++;
        prev = w;
        if (x->old_pattern && x->regress >= 20) __atomic_store_n(&x->stop, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void run_stress(stress_t *x, double seconds)
{
    pthread_t d, s, r;
    struct timespec t0, t;
    session_stats_reset();
    x->stop = 0;
    pthread_create(&d, NULL, thread_d, x);
    pthread_create(&s, NULL, thread_s, x);
    pthread_create(&r, NULL, thread_r, x);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec nap = {0, 10 * 1000 * 1000};
        nanosleep(&nap, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (__atomic_load_n(&x->stop, __ATOMIC_RELAXED)) break;
        if ((double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9 >= seconds) break;
    }
    __atomic_store_n(&x->stop, 1, __ATOMIC_RELAXED);
    pthread_join(d, NULL);
    pthread_join(s, NULL);
    pthread_join(r, NULL);
}

static void stress(void)
{
    stress_t fix, old;
    memset(&fix, 0, sizeof fix);
    memset(&old, 0, sizeof old);

    run_stress(&fix, 1.0);
    printf("  merge: %llu + %llu publications, %llu reads, %llu regressions, %llu torn\n",
           fix.it_d, fix.it_s, fix.reads, fix.regress, fix.torn);
    CHECK(fix.it_d > 1000 && fix.it_s > 1000 && fix.reads > 1000,
          "the stress really ran (both writers and the reader made progress)");
    CHECK(fix.regress == 0, "one writer per group: no counter ever steps back");
    CHECK(fix.torn == 0, "a group is always seen whole");

    old.old_pattern = 1;
    run_stress(&old, 1.0);
    printf("  old get/modify/publish: %llu + %llu publications, %llu reads, %llu regressions\n",
           old.it_d, old.it_s, old.reads, old.regress);
    CHECK(old.regress > 0,
          "COUNTER-CASE: the old whole-struct get/modify/publish, same threads, same reader, "
          "loses updates - a counter steps back");
}

int main(void)
{
    printf("== the metrics panel's snapshot (core/stats.c) ==\n");
    isolation();
    reset_clears();
    interleaving();
    stress();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
