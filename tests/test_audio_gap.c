/* test_audio_gap - streaming/audio_gap.h: output underruns, two-sided and
 * confirmed.
 *
 * === WHAT THIS SUITE EXISTS TO PREVENT (OUT-1, 2026-09-11) ===
 *
 * No output path counted an underrun, and the counter the finding first
 * proposed counts the wrong thing. This suite drives the REAL header through
 * the two output models of the assessment (assess_OUT-1/classifier_sim.c: same
 * generator, same seed, same scenarios in the same order - the archived
 * numbers reproduce bit for bit on MinGW and on glibc), with the ground truth
 * known by construction. Each case carries its counter-case: the rule as the
 * finding first wrote it, failing exactly where the assessment measured it.
 *   1. a silent VM gives 0 - counting every raw -EPIPE gives 88 in 90 s;
 *   2. an 80 ms stall gives 1 on ALSA and 4 on audout - the one-sided audout
 *      rule finds 1 of the 4;
 *   3. nine sound ends give 0 false counts - the one-sided rule gives 18 (9
 *      when the idle frames decode silent), two-sided without confirmation 9;
 *   4. the same counts at volume 0 - a peak read after the REAL gain reads 0;
 *   5. one count per episode - the finding's -EPIPE + app-level count counts
 *      two episodes twice, and so does a caller that marks a lost chunk played;
 *   6. FLAC, 480-sample blocks of real PCM - a -59 dBFS passage is heard at
 *      16, and missed at the finding's 64.
 * Plus the header's own edges: the confirmation window, the gap estimate,
 * xrun kept apart, and state that must be reset per session.
 *
 * The models (from classifier_sim.c), 0.25 ms ticks:
 *   - Switch audout: 4 buffers of one 10 ms frame, played back to back; a
 *     finished buffer becomes visible to the reclaim loop `lag` later (L11).
 *   - Linux ALSA on a hw device: ring + play thread as media/audio.c (wait
 *     until a chunk is there, 10 ms timed wait, blocking writei), buffer 20 ms,
 *     period 5 ms, snd_pcm_set_params's start threshold = stop threshold =
 *     buffer size -> XRUN when it drains, writei in XRUN -> -EPIPE, recover
 *     -> PREPARED, the chunk is LOST. L13 catch-up on the ring.
 * TRUTH: a dry episode is a real underrun iff the last frame played before it
 * and the next frame the source produced are audible and belong to the same
 * continuous segment. */
#include "../core/protocol/audio_gap.h"
#include "../core/protocol/audio_gain.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* ================================================= 1. the header, directly */

#define T0 1000000   /* a monotonic time is never 0 */

/* `n` frames at the 10 ms cadence, none observed dry, all accepted. */
static void play(audio_gap_t *g, int64_t *t, int n, bool audible)
{
    for (int i = 0; i < n; i++) {
        audio_gap_refill(g, *t, false, audible);
        audio_gap_played(g, *t, 10000, -1);
        *t += 10000;
    }
}

/* Ten frames, then the output observed dry at a refill 45 ms after the ten
 * ran out, then ONE more frame `next_dt` later. Returns the underruns. */
static uint32_t one_gap(bool prev_aud, bool refill_aud, int64_t next_dt, bool next_aud)
{
    audio_gap_t g; audio_gap_reset(&g);
    int64_t t = T0;
    play(&g, &t, 9, true);
    play(&g, &t, 1, prev_aud);
    t += 45000;
    audio_gap_refill(&g, t, true, refill_aud);
    audio_gap_played(&g, t, 10000, -1);
    audio_gap_refill(&g, t + next_dt, false, next_aud);
    uint32_t n = 0;
    audio_gap_read(&g, &n, NULL, NULL);
    return n;
}

static void unit(void)
{
    /* The peak scan. */
    {
        const int16_t z[4] = { 0, 0, 0, 0 };
        const int16_t m[3] = { 5, -32768, 7 };
        const int16_t a[2] = { 16, -16 };
        const int16_t b[2] = { 3, -17 };
        CHECK(audio_gap_peak(z, 4) == 0 && !audio_gap_audible(0), "digital silence: peak 0, inaudible");
        CHECK(audio_gap_peak(m, 3) == 32768, "-32768 reads 32768 (an int32_t: it does not wrap to itself)");
        CHECK(!audio_gap_audible(audio_gap_peak(a, 2)) && audio_gap_audible(audio_gap_peak(b, 2)),
              "the threshold: a peak of 16 is inaudible, 17 is audible, negative samples count");
        CHECK(audio_gap_peak(NULL, 10) == 0 && audio_gap_peak(z, 0) == 0, "a NULL or empty buffer reads 0");
    }
    /* Session start. */
    {
        audio_gap_t g; audio_gap_reset(&g);
        CHECK(!audio_gap_query_useful(&g, true), "nothing accepted yet: asking the output could count nothing");
        const unsigned ev = audio_gap_refill(&g, T0, true, true);
        CHECK(ev == AUDIO_GAP_NONE && g.dry_seen == 0,
              "the empty output before the first accepted frame is a start, not a gap");
    }
    /* A counted underrun, and its gap. */
    {
        audio_gap_t g; audio_gap_reset(&g);
        int64_t t = T0;
        play(&g, &t, 10, true);                               /* runs out at T0 + 100 ms */
        CHECK(audio_gap_query_useful(&g, true), "after audible frames, an audible refill is worth asking about");
        unsigned ev = audio_gap_refill(&g, T0 + 145000, true, true);
        uint32_t n = 9, ms = 9;
        audio_gap_read(&g, &n, &ms, NULL);
        CHECK(ev == AUDIO_GAP_OPENED && n == 0 && g.pend_gap_us == 45000,
              "dry between two audible frames opens a candidate, counts nothing yet, gap 45 ms");
        audio_gap_played(&g, T0 + 145000, 10000, -1);
        ev = audio_gap_refill(&g, T0 + 155000, false, true);
        audio_gap_read(&g, &n, &ms, NULL);
        CHECK(ev == AUDIO_GAP_COUNTED && n == 1 && ms == 45,
              "the next audible frame 10 ms later confirms it: underrun=1 (45 ms)");
    }
    /* The four conditions, one at a time. */
    CHECK(one_gap(true, true, 10000, true) == 1, "all four conditions: one underrun");
    CHECK(one_gap(true, true, 30000, true) == 1, "the next frame at exactly 30 ms still confirms");
    CHECK(one_gap(true, true, 30001, true) == 0,
          "the next frame 30.001 ms later: the stream did not resume its cadence, nothing counted");
    CHECK(one_gap(true, true, 10000, false) == 0,
          "the next frame is silent: a sound that ended (Opus's overlap tail made the refill audible)");
    CHECK(one_gap(false, true, 10000, true) == 0,
          "the frame before the gap was silent: a sound starting, not an underrun");
    CHECK(one_gap(true, false, 10000, true) == 0, "the refill frame is silent: nothing counted");
    /* One refill confirms the previous candidate AND opens a new one. */
    {
        audio_gap_t g; audio_gap_reset(&g);
        int64_t t = T0;
        play(&g, &t, 10, true);
        audio_gap_refill(&g, t + 30000, true, true);
        audio_gap_played(&g, t + 30000, 10000, -1);
        const unsigned ev = audio_gap_refill(&g, t + 60000, true, true);   /* 20 ms dry again */
        CHECK(ev == (AUDIO_GAP_COUNTED | AUDIO_GAP_OPENED),
              "two stalls 30 ms apart: the second refill confirms the first AND opens its own candidate");
    }
    /* One count per episode. */
    {
        audio_gap_t g; audio_gap_reset(&g);
        int64_t t = T0;
        play(&g, &t, 10, true);
        t += 40000;
        unsigned ev1 = audio_gap_refill(&g, t, true, true);     /* XRUN: this chunk hits -EPIPE */
        audio_gap_xrun(&g);                                      /* ... and is lost: no played() */
        CHECK(!audio_gap_query_useful(&g, true),
              "right after a dry observation, asking again is useless until a frame is accepted");
        unsigned ev2 = audio_gap_refill(&g, t + 10000, true, true);   /* PREPARED, delay 0 */
        audio_gap_played(&g, t + 10000, 10000, -1);
        play(&g, &t, 5, true);
        uint32_t n = 0, xr = 0;
        audio_gap_read(&g, &n, NULL, &xr);
        CHECK(ev1 == AUDIO_GAP_OPENED && ev2 == AUDIO_GAP_COUNTED && n == 1 && g.dry_seen == 1 && xr == 1,
              "a second dry observation with no accepted frame between is the SAME episode: counted once");
    }
    {
        audio_gap_t g; audio_gap_reset(&g);
        int64_t t = T0;
        play(&g, &t, 10, true);
        t += 40000;
        audio_gap_refill(&g, t, true, true);
        audio_gap_xrun(&g);
        audio_gap_played(&g, t, 10000, -1);                     /* THE CALLER BUG: the lost chunk marked played */
        audio_gap_refill(&g, t + 10000, true, true);
        audio_gap_played(&g, t + 10000, 10000, -1);
        play(&g, &t, 5, true);
        uint32_t n = 0;
        audio_gap_read(&g, &n, NULL, NULL);
        CHECK(n == 2,
              "COUNTER-CASE: played() for a chunk lost to -EPIPE opens a second episode for the same silence (2)");
    }
    /* The gap estimate. */
    {
        audio_gap_t g; audio_gap_reset(&g);
        int64_t t = T0;
        play(&g, &t, 2, true);
        audio_gap_refill(&g, t, false, true);
        audio_gap_played(&g, t, 10000, 25000);                  /* the backend says 25 ms queued */
        audio_gap_refill(&g, t + 55000, true, true);
        CHECK(g.pend_gap_us == 30000, "queued_us given: the output ran out at +25 ms, a refill at +55 ms is a 30 ms gap");
    }
    {
        audio_gap_t g; audio_gap_reset(&g);
        int64_t t = T0;
        play(&g, &t, 10, true);                                 /* the clock says: runs out at t */
        audio_gap_refill(&g, t - 5000, true, true);             /* the device says dry 5 ms earlier */
        CHECK(g.candidates == 1 && g.pend_gap_us == 0,
              "a device faster than the play-out clock: still a candidate, gap clamped at 0, never negative");
    }
    /* xrun= stays apart. */
    {
        audio_gap_t g; audio_gap_reset(&g);
        audio_gap_xrun(&g); audio_gap_xrun(&g); audio_gap_xrun(&g);
        uint32_t n = 9, ms = 9, xr = 0;
        audio_gap_read(&g, &n, &ms, &xr);
        CHECK(xr == 3 && n == 0 && ms == 0, "three raw xruns: xrun=3, underrun=0 (0 ms)");
        audio_gap_read(&g, NULL, NULL, NULL);
        CHECK(one_gap(true, true, 10000, true) == 1, "... and an underrun does not touch xrun= (read with NULL outputs)");
    }
    /* Per session. */
    {
        for (int arm = 0; arm < 2; arm++) {
            audio_gap_t g; audio_gap_reset(&g);
            int64_t t = T0;
            play(&g, &t, 500, true);                            /* session 1 ends in the middle of music */
            t += 35000000;                                      /* back to the VM list, reconnect */
            if (arm == 0) audio_gap_reset(&g);                  /* struct audio_decoder, calloc'd per session */
            audio_gap_refill(&g, t, true, true);                /* session 2: the output starts empty */
            audio_gap_played(&g, t, 10000, -1);
            play(&g, &t, 50, true);
            uint32_t n = 0;
            audio_gap_read(&g, &n, NULL, NULL);
            if (arm == 0)
                CHECK(n == 0, "session 2 starts with an empty output and music: 0");
            else
                CHECK(n == 1, "COUNTER-CASE: state kept across sessions (a function static) counts an "
                              "underrun at every session start (1)");
        }
    }
}

/* ======================================== 2. the models of the assessment */

#define FR          480
#define SPT         12
#define TICK        250          /* us */
#define FINDING_THR 64           /* the finding's threshold, for its own rules */
#define A_BUF       960
#define A_START     960
#define A_PERIOD    240
#define MAXF        40000

typedef struct { int64_t arr; int peak; int seg; } frame_t;
static frame_t F[MAXF];
static int NF;

static uint64_t rs = 88172645463325252ull;
static double ur(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (double)(rs >> 11) / 9007199254740992.0; }
static double nr(void) { double a = ur(), b = ur(); if (a < 1e-12) a = 1e-12; return sqrt(-2 * log(a)) * cos(6.283185307179586 * b); }

static void add(int64_t tprod, int pk, int seg, double sig_ms)
{
    if (NF >= MAXF) { printf("too many frames\n"); exit(2); }
    F[NF].arr = tprod + 15000 + (int64_t)(fabs(nr()) * sig_ms * 1000.0);
    F[NF].peak = pk; F[NF].seg = seg; NF++;
}
static void stall(int64_t ts, int64_t d)
{
    for (int i = 0; i < NF; i++) if (F[i].arr >= ts && F[i].arr < ts + d) F[i].arr = ts + d;
}
static void finish(void) { for (int i = 1; i < NF; i++) if (F[i].arr < F[i - 1].arr) F[i].arr = F[i - 1].arr; }
static int truth_after(int L) { return L >= 0 && L + 1 < NF && F[L].seg > 0 && F[L + 1].seg == F[L].seg; }

/* The scenarios, 90 s each - verbatim. */
static void sc_music(double sig, const int64_t *st, const int64_t *sd, int ns)
{
    NF = 0;
    for (int k = 0; k < 9000; k++) add(k * 10000LL, 10000, 1, sig);
    for (int i = 0; i < ns; i++) stall(st[i], sd[i]);
    finish();
}
static void sc_idle(void)
{
    NF = 0;
    for (int64_t t = 0; t < 90000000LL; t += (int64_t)(250000 + ur() * 170000)) add(t, 0, 0, 2.0);
    finish();
}
static void sc_sound_ends(int tail)
{
    NF = 0; int64_t t = 0; int seg = 1;
    for (int c = 0; c < 9; c++) {
        for (int k = 0; k < 500; k++) { add(t, 10000, seg, 2.0); t += 10000; }
        seg++;
        int64_t end = t + 5000000; int first = 1;
        t += (int64_t)(250000 + ur() * 170000);
        while (t < end) { add(t, first ? tail : 0, 0, 2.0); first = 0; t += (int64_t)(250000 + ur() * 170000); }
        t = end;
    }
    finish();
}
static void sc_game(void)
{
    NF = 0; int64_t t = 0; int seg = 1;
    while (t < 90000000LL) {
        int aud = (int)((0.3 + ur() * 1.7) * 100);
        for (int k = 0; k < aud; k++) { add(t, 10000, seg, 3.0); t += 10000; }
        seg++;
        int sil = (int)((0.2 + ur() * 0.8) * 100);
        for (int k = 0; k < sil; k++) { add(t, 0, 0, 3.0); t += 10000; }
    }
    for (int64_t s = 7500000; s < 90000000LL; s += 15000000) stall(s, 80000);
    finish();
}

/* The ground-truth episodes, and what each classifier counted in them. */
typedef struct {
    int     truth;      /* dry in the middle of continuous sound */
    int     seg;        /* segment of the last frame played before it */
    int64_t start;      /* when the output ran dry */
    int     cF;         /* the finding's rule */
    int     cand;       /* the header: candidates opened in it (two-sided, unconfirmed) */
    int     cnt;        /* the header: underruns counted in it */
    int64_t err;        /* the header's gap estimate - the true gap, at its candidate */
} ep_t;
static ep_t EP[MAXF];
static int NEP;
static int open_ep(int last, int64_t t)
{
    if (NEP >= MAXF) { printf("too many episodes\n"); exit(2); }
    memset(&EP[NEP], 0, sizeof EP[NEP]);
    EP[NEP].truth = truth_after(last);
    EP[NEP].seg   = last >= 0 ? F[last].seg : 0;
    EP[NEP].start = t;
    return NEP++;
}

typedef struct {
    int64_t lag;              /* audout: a finished buffer is reclaimed this long after */
    int     gain_pct;         /* the output gain, applied by the REAL audio_gain_apply */
    bool    peak_after_gain;  /* THE TRAP: the peak read after the gain */
    int     thr;              /* 0 = the header's audibility; else peak > thr */
    bool    delay_rule;       /* ALSA: dry = XRUN || delay <= 0 (plugins), not XRUN only */
    bool    lost_played;      /* CALLER BUG: played() for the chunk lost to -EPIPE */
    bool    queued;           /* ALSA: played() is given the device's queue (snd_pcm_delay) */
} opt_t;

#define NSEG 8
typedef struct {
    long dry, truth;                     /* ground truth: dry episodes, real underruns */
    long tp, fp, multi, cnt_total;       /* the header, attributed to episodes */
    long cand_tp, cand_fp;               /* two-sided candidates, i.e. without condition 4 */
    long F_tp, F_fp, F_multi, F_total;   /* the finding's rule */
    long epipe, r1, r2, r2_near;
    long queries, queries_ungated, drops, obs_dry, orphan;
    long prep_empty;                     /* ALSA: data back after a wait, device PREPARED and empty */
    long truth_seg[NSEG], cnt_seg[NSEG];
    uint32_t underruns, gap_ms, xruns, dry_seen;
    int64_t err_min, err_max;            /* gap estimate - truth over counted episodes, us */
} res_t;

/* A frame's peak through the real code: a buffer whose peak is `sym`, the real
 * audio_gain_apply, and the header's scan before or after it. */
static const int PAT[8] = { 331, -500, 1000, -250, 0, 120, -870, 200 };
static int peak_of(const opt_t *o, int sym)
{
    int16_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (int16_t)(sym * PAT[i] / 1000);
    const int32_t pre = audio_gap_peak(buf, 8);
    audio_gain_apply(buf, 8, (uint32_t)o->gain_pct);
    const int32_t post = audio_gap_peak(buf, 8);
    return o->peak_after_gain ? post : pre;
}
static bool audible_of(const opt_t *o, int pk) { return o->thr ? pk > o->thr : audio_gap_audible(pk); }

static void attribute(unsigned ev, int *cand_ep, int cur_ep, int64_t t, const audio_gap_t *g, res_t *r)
{
    if (ev & AUDIO_GAP_COUNTED) {
        if (*cand_ep >= 0) EP[*cand_ep].cnt++; else r->orphan++;
    }
    if (ev & AUDIO_GAP_OPENED) {
        if (cur_ep < 0) { r->orphan++; *cand_ep = -1; return; }
        *cand_ep = cur_ep;
        EP[cur_ep].cand++;
        EP[cur_ep].err = g->pend_gap_us - (t - EP[cur_ep].start);
    }
}

static void tally(res_t *r, const audio_gap_t *g)
{
    r->err_min = INT64_MAX; r->err_max = INT64_MIN;
    for (int i = 0; i < NEP; i++) {
        const ep_t *e = &EP[i];
        const int s = e->seg < NSEG ? e->seg : NSEG - 1;
        r->dry++; r->truth += e->truth;
        if (e->truth) r->truth_seg[s]++;
        r->cnt_total += e->cnt;
        r->cnt_seg[s] += e->cnt;
        if (e->cnt) {
            if (e->truth) r->tp++; else r->fp++;
            if (e->err < r->err_min) r->err_min = e->err;
            if (e->err > r->err_max) r->err_max = e->err;
        }
        if (e->cnt > 1) r->multi++;
        if (e->cand) { if (e->truth) r->cand_tp++; else r->cand_fp++; }
        if (e->cF) { if (e->truth) r->F_tp++; else r->F_fp++; }
        if (e->cF > 1) r->F_multi++;
        r->F_total += e->cF;
    }
    if (r->err_min == INT64_MAX) r->err_min = r->err_max = 0;
    audio_gap_read(g, &r->underruns, &r->gap_ms, &r->xruns);
    r->dry_seen = g->dry_seen;
}

/* Switch audout. */
static void run_audout(const opt_t *o, res_t *r)
{
    memset(r, 0, sizeof *r); NEP = 0;
    audio_gap_t g; audio_gap_reset(&g);
    int qf[8], qfin[8]; int64_t qt[8]; int qn = 0, pos = 0;
    int played_any = 0, dry = 0, cur_ep = -1, last_played = -1, prev_pk = 0, cand_ep = -1;
    int fi = 0;
    const int64_t tend = F[NF - 1].arr + 200000;
    for (int64_t t = 0; t <= tend; t += TICK) {
        while (fi < NF && F[fi].arr <= t) {
            while (qn > 0 && qfin[0] && qt[0] + o->lag <= t) {             /* reclaim loop */
                for (int j = 1; j < qn; j++) { qf[j - 1] = qf[j]; qfin[j - 1] = qfin[j]; qt[j - 1] = qt[j]; }
                qn--;
            }
            const int pk = peak_of(o, F[fi].peak);
            const bool aud = audible_of(o, pk);
            /* The finding's rule: one-sided, on our own in-flight count. */
            if (qn == 0 && played_any && dry && prev_pk > FINDING_THR) EP[cur_ep].cF++;
            /* The rule: ask the driver when our count says <= 2 buffers AND the
             * answer can count; dry iff played == submitted. */
            if (qn <= AUDIO_GAP_QUERY_INFLIGHT && played_any) r->queries_ungated++;
            bool obs = false;
            if (qn <= AUDIO_GAP_QUERY_INFLIGHT && audio_gap_query_useful(&g, aud)) {
                r->queries++;
                obs = dry != 0;
            }
            if (obs) r->obs_dry++;
            attribute(audio_gap_refill(&g, t, obs, aud), &cand_ep, cur_ep, t, &g, r);
            if (qn < 4) {
                qf[qn] = fi; qfin[qn] = 0; qt[qn] = 0; qn++; prev_pk = pk; dry = 0;
                audio_gap_played(&g, t, 10000, -1);
            } else r->drops++;
            fi++;
        }
        int k = -1;
        for (int j = 0; j < qn; j++) if (!qfin[j]) { k = j; break; }
        if (k >= 0) {
            played_any = 1; pos += SPT;
            if (pos >= FR) { pos = 0; qfin[k] = 1; qt[k] = t; last_played = qf[k]; }
        } else if (played_any && !dry) { dry = 1; cur_ep = open_ep(last_played, t); }
    }
    tally(r, &g);
}

/* Linux ALSA, hw device. */
enum { PREP, RUN, XRUN };
enum { TOP, WAIT, WRITE };
static void run_alsa(const opt_t *o, res_t *r)
{
    memset(r, 0, sizeof *r); NEP = 0;
    audio_gap_t g; audio_gap_reset(&g);
    int rq[256], rh = 0, rn = 0;                       /* ring of frame indices */
    int dqf[16], dqr[16], dn = 0, hw = 0, st = PREP;   /* device FIFO */
    int th = TOP, cur = -1, cur_pk = 0, rem = 0, came_from_wait = 0, after_write = 0, r2_done = 0, r2_pending = 0;
    int64_t wait_start = 0;
    int last_written_pk = 0, dry = 0, cur_ep = -1, last_played = -1, cand_ep = -1;
    int fi = 0;
    const int64_t tend = F[NF - 1].arr + 200000;
    for (int64_t t = 0; t <= tend; t += TICK) {
        int signaled = 0;
        while (fi < NF && F[fi].arr <= t) {
            if (rn == 256) { rh = (rh + 1) % 256; rn--; }
            rq[(rh + rn) % 256] = fi; rn++;
            if (rn * FR > 2880) while (rn > 3) { rh = (rh + 1) % 256; rn--; }   /* L13 */
            signaled = 1; fi++;
        }
        /* The device plays one tick. */
        if (st == RUN) {
            int need = SPT;
            while (need > 0 && dn > 0) {
                int c = need < dqr[0] ? need : dqr[0];
                dqr[0] -= c; hw -= c; need -= c; last_played = dqf[0];
                if (dqr[0] == 0) { for (int j = 1; j < dn; j++) { dqf[j - 1] = dqf[j]; dqr[j - 1] = dqr[j]; } dn--; }
            }
            if (hw == 0) {
                st = XRUN;
                if (!dry) {
                    dry = 1; cur_ep = open_ep(last_played, t);
                    if (r2_pending) { EP[cur_ep].cF++; r2_pending = 0; }
                }
            }
        }
        /* The play thread. */
        for (int guard = 0; guard < 16; guard++) {
            if (th == TOP) {
                if (rn > 0) {
                    cur = rq[rh]; rh = (rh + 1) % 256; rn--;
                    cur_pk = peak_of(o, F[cur].peak);
                    const bool aud = audible_of(o, cur_pk);
                    /* The moment the plugins' test (delay <= 0) would also call dry:
                     * after a recover, before anything was written again. */
                    if (came_from_wait && st == PREP && hw <= 0 && g.have_prev) r->prep_empty++;
                    /* The rule: when data comes back after a WAIT, ask the device. */
                    bool obs = false;
                    if (came_from_wait && audio_gap_query_useful(&g, aud)) {
                        r->queries++;
                        obs = st == XRUN || (o->delay_rule && hw <= 0);
                    }
                    if (obs) r->obs_dry++;
                    attribute(audio_gap_refill(&g, t, obs, aud), &cand_ep, cur_ep, t, &g, r);
                    if (r2_pending) { r->r2_near++; r2_pending = 0; }   /* R2 fired, no xrun followed */
                    came_from_wait = 0; rem = FR; th = WRITE;
                    continue;
                }
                th = WAIT; wait_start = t; came_from_wait = 1; r2_done = 0;
                /* The finding's app-level rule, R1: once, at wait entry. */
                if (after_write && last_written_pk > FINDING_THR && st != XRUN && hw < A_PERIOD) r->r1++;
                break;
            }
            if (th == WAIT) {
                if (!(signaled || t - wait_start >= 10000)) break;
                /* R2: at every wake-up of the wait that follows a write, once. */
                if (after_write && !r2_done && last_written_pk > FINDING_THR && st != XRUN && hw < A_PERIOD) {
                    r->r2++; r2_done = 1;
                    if (dry) EP[cur_ep].cF++; else r2_pending = 1;
                }
                if (rn > 0) { th = TOP; continue; }
                wait_start = t; break;
            }
            if (th == WRITE) {
                if (st == XRUN) {                                       /* -EPIPE */
                    r->epipe++; EP[cur_ep].cF++;                        /* the finding: every -EPIPE */
                    audio_gap_xrun(&g);
                    if (o->lost_played) audio_gap_played(&g, t, 10000, -1);   /* CALLER BUG */
                    st = PREP; hw = 0; dn = 0; after_write = 0;         /* recover; the chunk is lost */
                    th = TOP; continue;
                }
                int room = A_BUF - hw, n = rem < room ? rem : room;
                if (n > 0) {
                    if (dn > 0 && dqf[dn - 1] == cur) dqr[dn - 1] += n;   /* same chunk, partial writes */
                    else { dqf[dn] = cur; dqr[dn] = n; dn++; }
                    hw += n; rem -= n;
                    if (st == PREP && hw >= A_START) { st = RUN; dry = 0; }
                }
                if (rem == 0) {
                    last_written_pk = cur_pk; after_write = 1;
                    audio_gap_played(&g, t, 10000, o->queued ? (int64_t)hw * TICK / SPT : -1);
                    th = TOP; continue;
                }
                break;                                                  /* blocked in writei */
            }
        }
    }
    tally(r, &g);
}

static void print_res(const char *label, const res_t *r)
{
    printf("   %-17s: dry %4ld truth %4ld | rule %3ld/%-2ld x2 %ld | cand %3ld/%-2ld | finding %3ld/%-3ld (total %ld, x2 %ld) |"
           " q %ld (ungated %ld) | err [%lld, %lld] us | underrun=%u (%u ms) xrun=%u\n",
           label, r->dry, r->truth, r->tp, r->fp, r->multi, r->cand_tp, r->cand_fp,
           r->F_tp, r->F_fp, r->F_total, r->F_multi, r->queries, r->queries_ungated,
           (long long)r->err_min, (long long)r->err_max, r->underruns, r->gap_ms, r->xruns);
}

typedef struct { res_t a5, a10, l; } trio_t;

static void run3(const char *name, int gain_pct, bool after, trio_t *out)
{
    opt_t oa5  = { .lag = 5000,  .gain_pct = gain_pct, .peak_after_gain = after };
    opt_t oa10 = { .lag = 10000, .gain_pct = gain_pct, .peak_after_gain = after };
    opt_t ol   = { .lag = 0,     .gain_pct = gain_pct, .peak_after_gain = after };
    run_audout(&oa5, &out->a5);
    run_audout(&oa10, &out->a10);
    run_alsa(&ol, &out->l);
    printf("%s\n", name);
    print_res("audout lag 5 ms", &out->a5);
    print_res("audout lag 10 ms", &out->a10);
    print_res("ALSA hw", &out->l);
}

/* What must hold for the rule on every scenario and model. The gap estimate:
 * exact on audout (it plays what it is handed at once), and on ALSA read up to
 * one chunk long through the play-out clock (the device waits for a full
 * 20 ms buffer before it restarts). */
static void rule_holds(const char *what, const res_t *r, bool alsa_clock)
{
    char m[300];
    snprintf(m, sizeof m, "%s: every real underrun counted (%ld of %ld), no false count, none twice",
             what, r->tp, r->truth);
    CHECK(r->tp == r->truth && r->fp == 0 && r->multi == 0, m);
    snprintf(m, sizeof m, "%s: underrun= (%u) is the sum of the episode counts, none unattributed",
             what, r->underruns);
    CHECK((long)r->underruns == r->cnt_total && r->orphan == 0, m);
    snprintf(m, sizeof m, "%s: xrun= (%u) is the raw -EPIPE count, kept apart", what, r->xruns);
    CHECK((long)r->xruns == r->epipe, m);
    if (alsa_clock) {
        snprintf(m, sizeof m, "%s: gap estimate - truth in [0, 10 ms] (play-out clock, start threshold)", what);
        CHECK(r->err_min >= 0 && r->err_max <= 10000, m);
    } else {
        snprintf(m, sizeof m, "%s: gap estimate exact", what);
        CHECK(r->err_min == 0 && r->err_max == 0, m);
    }
}

static void trio_holds(const char *name, const trio_t *x)
{
    char w[120];
    snprintf(w, sizeof w, "%s, audout lag 5 ms", name);  rule_holds(w, &x->a5, false);
    snprintf(w, sizeof w, "%s, audout lag 10 ms", name); rule_holds(w, &x->a10, false);
    snprintf(w, sizeof w, "%s, ALSA", name);             rule_holds(w, &x->l, true);
}

static void models(void)
{
    trio_t s1, s1b, s5pre, s5post, s1c, s2, s3a, s3b, s4;
    printf("Counts are tp/fp: tp = real underrun (dry in the middle of continuous sound), fp = anything else.\n");

    /* The order is the archived run's: the generator's state carries over. */
    { int64_t st[] = { 45000000 }, sd[] = { 80000 }; sc_music(2.0, st, sd, 1); }
    run3("S1 music, one 80 ms stall at 45 s", 100, false, &s1);
    trio_holds("S1", &s1);
    CHECK(s1.a5.underruns == 4 && s1.a10.underruns == 4 && s1.l.underruns == 1,
          "S1: one 80 ms stall = 4 underruns on audout (both lags), 1 on ALSA");
    CHECK(s1.a5.F_tp == 1 && s1.a5.truth == 4,
          "COUNTER-CASE S1: the one-sided audout rule finds 1 of the 4 - releases are seen late (L11)");

    { int64_t st[] = { 11000000, 33000000, 55000000, 77000000 }, sd[] = { 300000, 300000, 300000, 300000 };
      sc_music(3.0, st, sd, 4); }
    run3("S1b music, 300 ms stall every 22 s", 100, false, &s1b);
    trio_holds("S1b", &s1b);
    CHECK(s1b.a5.underruns == 10 && s1b.a10.underruns == 10 && s1b.l.underruns == 4,
          "S1b: four 300 ms stalls = 10 underruns on audout, 4 on ALSA");
    run3("S5 = S1b at volume 0, the peak read BEFORE the gain (the rule)", 0, false, &s5pre);
    trio_holds("S5 pre-gain", &s5pre);
    CHECK(s5pre.a5.underruns == s1b.a5.underruns && s5pre.a10.underruns == s1b.a10.underruns
          && s5pre.l.underruns == s1b.l.underruns && s5pre.l.gap_ms == s1b.l.gap_ms,
          "SHADOW_VOLUME=0: the same counts and the same ms as at 100 %");
    run3("S5 = S1b at volume 0, the peak read AFTER the gain (the trap)", 0, true, &s5post);
    CHECK(s5post.a5.truth == 10 && s5post.a5.underruns == 0 && s5post.a10.underruns == 0 && s5post.l.underruns == 0,
          "COUNTER-CASE S5: a peak read after the real audio_gain_apply at 0 % reads 0 - every underrun lost");
    CHECK(s5post.l.xruns == s1b.l.xruns && s5post.l.xruns > 0,
          "... while xrun=, which reads no peak, is unchanged: it cannot tell sound from silence");

    { sc_music(12.0, NULL, NULL, 0); }
    run3("S1c music, heavy jitter (sigma 12 ms)", 100, false, &s1c);
    trio_holds("S1c", &s1c);
    CHECK(s1c.a5.underruns == 469 && s1c.a10.underruns == 898 && s1c.l.underruns == 24,
          "S1c: 469 / 898 underruns on audout (lag 5 / 10 ms), 24 on ALSA");
    CHECK(s1c.a5.F_tp == 185 && s1c.a10.F_tp == 139,
          "COUNTER-CASE S1c: the one-sided audout rule finds 185 of 469 and 139 of 898");
    CHECK(s1c.l.F_multi == 2 && s1c.l.multi == 0,
          "COUNTER-CASE S1c, one count per episode: the finding's -EPIPE + app-level rule counts 2 "
          "episodes twice; the rule counts none twice");
    /* One count per episode on the plugins' test (delay <= 0), the caller's
     * side of that contract, and the gap given the device's own queue. Same
     * S1c frames: under heavy jitter a refill lost to -EPIPE can leave the ring
     * empty, so the next frame finds the device PREPARED with delay 0. Run
     * here - the next scenario overwrites the frames. */
    {
        res_t d, bug, q;
        opt_t od   = { .gain_pct = 100, .delay_rule = true };
        opt_t obug = { .gain_pct = 100, .delay_rule = true, .lost_played = true };
        opt_t oq   = { .gain_pct = 100, .queued = true };
        run_alsa(&od, &d);     print_res("ALSA delay<=0", &d);
        run_alsa(&obug, &bug); print_res("ALSA caller bug", &bug);
        run_alsa(&oq, &q);     print_res("ALSA queued_us", &q);
        rule_holds("S1c, ALSA, dry = XRUN || delay <= 0", &d, true);
        char m[300];
        snprintf(m, sizeof m, "S1c delay <= 0: the device is found PREPARED and empty %ld times after a "
                 "lost refill, and the rule still counts %u, none twice", d.prep_empty, d.underruns);
        CHECK(d.prep_empty > 0 && d.underruns == s1c.l.underruns && d.multi == 0, m);
        CHECK(bug.multi == 7 && bug.underruns == 31,
              "COUNTER-CASE S1c: a caller that marks the chunk lost to -EPIPE as played counts 7 episodes "
              "twice (underrun=31 for 24)");
        CHECK(q.tp == q.truth && q.fp == 0 && q.err_min == 0 && q.err_max == 0,
              "S1c, ALSA given snd_pcm_delay after each write: the gap estimate is exact");
    }

    { sc_idle(); }
    run3("S2 silent VM idle (f4 ff fe, ~3/s)", 100, false, &s2);
    trio_holds("S2", &s2);
    CHECK(s2.a5.underruns == 0 && s2.a10.underruns == 0 && s2.l.underruns == 0 && s2.l.xruns == 88,
          "S2 silent VM: underrun=0 everywhere, while xrun=88 carries the idle gaps apart");
    CHECK(s2.l.F_fp == 88 && s2.l.F_total == 88,
          "COUNTER-CASE S2: counting every raw -EPIPE gives 88 false underruns in 90 s of silence");
    CHECK(s2.a5.queries == 0 && s2.l.queries == 0 && s2.a5.queries_ungated == 265,
          "S2: on a silent VM the rule asks the output NOTHING - the ungated audout query ran 265 IPCs");

    { sc_sound_ends(2439); }
    run3("S3a 9 sounds end, 1st idle frame peak 2439", 100, false, &s3a);
    trio_holds("S3a", &s3a);
    CHECK(s3a.a5.fp == 0 && s3a.a10.fp == 0 && s3a.l.underruns == 0,
          "S3a: nine sound ends with Opus's overlap tail - 0 false counts");
    CHECK(s3a.a5.F_fp == 18 && s3a.a5.F_tp == 0,
          "COUNTER-CASE S3a: the one-sided audout rule counts 18 false underruns (and none of the real ones)");
    CHECK(s3a.a5.cand_fp == 9 && s3a.a10.cand_fp == 9 && s3a.l.cand_fp == 9,
          "COUNTER-CASE S3a: two-sided WITHOUT the confirmation, 9 candidates are sound ends on both "
          "outputs - condition 4 drops them");

    { sc_sound_ends(0); }
    run3("S3b 9 sounds end, idle frames silent", 100, false, &s3b);
    trio_holds("S3b", &s3b);
    CHECK(s3b.a5.F_fp == 9 && s3b.a5.fp == 0 && s3b.a5.cand_fp == 0,
          "COUNTER-CASE S3b: silent idle frames, the one-sided rule still counts 9; the rule not even a candidate");

    { sc_game(); }
    run3("S4 game: continuous cadence, 80 ms stall/15 s", 100, false, &s4);
    trio_holds("S4", &s4);
    CHECK(s4.a5.underruns == 9 && s4.a10.underruns == 9 && s4.l.underruns == 4,
          "S4 game: 9 underruns on audout, 4 on ALSA");
    CHECK(s4.l.F_fp == 2, "COUNTER-CASE S4: the finding's ALSA rule counts 2 false underruns");
}

/* ============================================= 3. FLAC, 480-sample blocks */

/* One block: 480 stereo frames of a 440 Hz sine at `amp` (0 = digital
 * silence, exactly what a lossless decoder returns), phase continuous. Its
 * peak comes from the header's own scan. */
static int flac_block_peak(int amp, int64_t *phase)
{
    int16_t pcm[2 * FR];
    for (int n = 0; n < FR; n++) {
        const double v = amp * sin(6.283185307179586 * 440.0 * (double)(*phase + n) / 48000.0);
        const int16_t s = (int16_t)lrint(v);
        pcm[2 * n] = s; pcm[2 * n + 1] = s;
    }
    *phase += FR;
    return (int)audio_gap_peak(pcm, 2 * FR);
}

static void flac(void)
{
    int64_t t = 0, ph = 0, quiet_at;
    int pk_music = -1, pk_quiet = -1, pk_sil = -1;
    NF = 0;
    for (int k = 0; k < 2000; k++) { int p = flac_block_peak(3277, &ph); if (k == 7) pk_music = p; add(t, p, 1, 2.0); t += 10000; }
    for (int64_t e = t + 5000000; t < e; t += (int64_t)(250000 + ur() * 170000)) {
        int p = flac_block_peak(0, &ph); if (pk_sil < 0) pk_sil = p; add(t, p, 0, 2.0);
    }
    t = (t + 9999) / 10000 * 10000;
    quiet_at = t;
    for (int k = 0; k < 2000; k++) { int p = flac_block_peak(36, &ph); if (k == 7) pk_quiet = p; add(t, p, 2, 2.0); t += 10000; }
    for (int64_t e = t + 5000000; t < e; t += (int64_t)(250000 + ur() * 170000)) add(t, flac_block_peak(0, &ph), 0, 2.0);
    stall(10000000, 80000);                  /* in the -20 dBFS passage */
    stall(quiet_at + 10000000, 80000);       /* in the -59 dBFS passage */
    finish();

    printf("FLAC 480-sample blocks: -20 dBFS then -59 dBFS, one 80 ms stall in each, sounds end in digital silence\n");
    printf("   block peaks: music %d, quiet %d, silence %d\n", pk_music, pk_quiet, pk_sil);
    CHECK(pk_music >= 3270 && pk_music <= 3277 && pk_quiet == 36 && pk_sil == 0,
          "FLAC blocks through the scan: -20 dBFS peaks ~3277, -59 dBFS peaks 36, lossless silence exactly 0");

    res_t a, l, a64, l64;
    opt_t oa = { .lag = 5000, .gain_pct = 100 }, ol = { .gain_pct = 100 };
    opt_t oa64 = { .lag = 5000, .gain_pct = 100, .thr = FINDING_THR }, ol64 = { .gain_pct = 100, .thr = FINDING_THR };
    run_audout(&oa, &a);     print_res("audout (push_pcm)", &a);
    run_alsa(&ol, &l);       print_res("ALSA hw", &l);
    run_audout(&oa64, &a64); print_res("audout, thr 64", &a64);
    run_alsa(&ol64, &l64);   print_res("ALSA, thr 64", &l64);
    rule_holds("FLAC, audout", &a, false);
    rule_holds("FLAC, ALSA", &l, true);
    CHECK(a.truth_seg[2] > 0 && a.cnt_seg[2] == a.truth_seg[2] && l.truth_seg[2] > 0 && l.cnt_seg[2] == l.truth_seg[2],
          "FLAC: the stall in the -59 dBFS passage is counted, on both outputs");
    CHECK(a.cand_fp == 0 && l.cand_fp == 0,
          "FLAC: a sound ending in lossless silence is not even a candidate - no overlap tail");
    CHECK(a64.cnt_seg[2] == 0 && l64.cnt_seg[2] == 0 && a64.cnt_seg[1] == a.cnt_seg[1],
          "COUNTER-CASE FLAC: at the finding's threshold of 64 the -59 dBFS passage's underruns vanish "
          "(the -20 dBFS ones stay)");
}

int main(void)
{
    printf("== output underruns, two-sided and confirmed (streaming/audio_gap.h) ==\n");
    unit();
    models();
    flac();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
