/* test_audio_loss - streaming/audio_loss.h: audio frames that never arrived.
 *
 * === WHAT THIS SUITE EXISTS TO PREVENT (AUD-DEDUP-3 / AUD-INS-1, 2026-09-11) ===
 *
 * The anti-duplicate window sees every sequence gap and used to throw it away:
 * audio loss was counted nowhere. The accountant that now counts it sits BESIDE
 * the window and is fed only what the window accepts. This suite pins three
 * things, each with its counter-case:
 *   1. the count is EXACT against the generator's ground truth (frames whose
 *      two copies were both dropped) - under jitter, bursts, single-copy loss,
 *      silence, the 2^32 wrap and renumbering - where the rules the finding
 *      first proposed are not: a gap counted at the forward jump, jumps beyond
 *      64 set apart, a "previous frame active" gate;
 *   2. the window's decisions are UNCHANGED - counting never alters what is
 *      played (S36, KB §3.31);
 *   3. the panel grades the loss only when the window expected enough frames:
 *      in silence one lost frame in 50 is 2 %, which must not read Bad.
 *
 * The traces are those of the assessment harness (assess_AUD-DEDUP-3/aud3.c):
 * same generator, same seed, same scenarios. */
#include "../core/protocol/audio_dedup.h"
#include "../core/protocol/audio_loss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* ------------------------------------------------------------ the call site */

/* The call site as the session wires it (aud_accept): the window decides,
 * alone; the accountant notes what it accepted, 0x12 frames only (the 0x02
 * stream descriptor shares seq 0); a refusal is sorted into duplicate or
 * stale. */
typedef struct { audio_dedup_t d; audio_loss_t l; uint32_t played, dup, stale; } site_t;

static bool site_accept(site_t *s, uint8_t type, uint32_t seq)
{
    if (audio_dedup_accepte(&s->d, seq)) {
        s->played++;
        if (type == 0x12) audio_loss_note(&s->l, seq);
        return true;
    }
    if (audio_loss_is_stale(s->d.plus_haut, seq)) s->stale++; else s->dup++;
    return false;
}

/* COUNTER-CASE MODEL - the finding's first rule: the gap counted at the
 * forward jump (ecart - 1), a jump beyond 64 set apart as a "jump". Returns the
 * gap it counted. Kept so that a later "simplification" back to it fails here. */
typedef struct { bool primed; uint32_t top, gaps, jumped; } naive_t;

static uint32_t naive_note(naive_t *n, uint32_t seq)
{
    if (!n->primed) { n->primed = true; n->top = seq; return 0; }
    const int32_t d = (int32_t)(seq - n->top);
    if (d <= 0) return 0;
    n->top = seq;
    if (d > 64) { n->jumped += (uint32_t)(d - 1); return 0; }
    n->gaps += (uint32_t)(d - 1);
    return (uint32_t)(d - 1);
}

/* ------------------------------------------------------------ the generator */

typedef struct { double t; uint32_t seq; uint16_t len; } pkt_t;
typedef struct { pkt_t *v; size_t n, cap; } trace_t;

static void tpush(trace_t *tr, double t, uint32_t seq, uint16_t len)
{
    if (tr->n == tr->cap) {
        tr->cap = tr->cap ? tr->cap * 2 : 4096;
        pkt_t *nv = (pkt_t *)realloc(tr->v, tr->cap * sizeof *tr->v);
        if (!nv) { printf("out of memory\n"); exit(2); }
        tr->v = nv;
    }
    tr->v[tr->n].t = t; tr->v[tr->n].seq = seq; tr->v[tr->n].len = len; tr->n++;
}

static int cmp_pkt(const void *a, const void *b)
{
    const double x = ((const pkt_t *)a)->t, y = ((const pkt_t *)b)->t;
    return (x > y) - (x < y);
}

static uint32_t rs = 2463534242u;
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static double ur(void) { return (xr() >> 8) / 16777216.0; }

typedef struct {
    const char *name;
    int      nframes;
    uint32_t base;
    double   p_one;       /* one copy lost */
    double   p_burst;     /* per frame: a both-copies burst starts */
    int      burst_len;
    double   p_jitter;    /* a packet is delayed by 0..jitter_ms */
    double   jitter_ms;
    int      sil_from, sil_to;   /* frames sent as DTX silence: 8 B, 400 ms apart */
} scen_t;

typedef struct {
    uint32_t truth, runs, max_run;     /* ground truth: lost frames, bursts, longest */
    uint32_t packets, differ, top_drift;
    uint32_t played, dup, stale;
    audio_loss_t l;
    uint32_t naive_gaps, naive_jumped, gated;
} res_t;

static res_t run(const scen_t *s)
{
    res_t r; memset(&r, 0, sizeof r);
    trace_t tr = {0};
    double t = 0; int burst_left = 0; uint32_t run_len = 0;
    for (int i = 0; i < s->nframes; i++) {
        const bool silent = i >= s->sil_from && i < s->sil_to;
        const uint16_t len = silent ? 8 : 150;
        const bool tail = i >= s->nframes - 64;      /* clean tail: every slot decided */
        const uint32_t seq = s->base + (uint32_t)i;
        if (!tail && burst_left == 0 && ur() < s->p_burst) burst_left = s->burst_len;
        if (!tail && burst_left > 0) {
            burst_left--; r.truth++;
            if (run_len++ == 0) r.runs++;
            if (run_len > r.max_run) r.max_run = run_len;
            t += silent ? 400 : 10;
            continue;
        }
        run_len = 0;
        const int lose = (!tail && ur() < s->p_one) ? (int)(xr() & 1) : -1;
        for (int c = 0; c < 2; c++) {
            if (c == lose) continue;
            double at = t + c * 4.0;
            if (!tail && ur() < s->p_jitter) at += ur() * s->jitter_ms;
            tpush(&tr, at, seq, len);
        }
        t += silent ? 400 : 10;
    }
    qsort(tr.v, tr.n, sizeof *tr.v, cmp_pkt);

    audio_dedup_t bare = {0};
    site_t site; memset(&site, 0, sizeof site);
    naive_t nv; memset(&nv, 0, sizeof nv);
    uint16_t prev_len = 0; bool have_prev = false;
    for (size_t k = 0; k < tr.n; k++) {
        const uint32_t seq = tr.v[k].seq;
        const bool o = audio_dedup_accepte(&bare, seq);
        const bool a = site_accept(&site, 0x12, seq);
        if (o != a) r.differ++;
        if (a) {
            if (site.l.top != site.d.plus_haut) r.top_drift++;
            const uint32_t g = naive_note(&nv, seq);
            /* the finding's gate: count only after an ACTIVE frame (> 8 B) */
            if (have_prev && prev_len > 8) r.gated += g;
            prev_len = tr.v[k].len; have_prev = true;
        }
    }
    r.packets = (uint32_t)tr.n;
    r.played = site.played; r.dup = site.dup; r.stale = site.stale;
    r.l = site.l;
    r.naive_gaps = nv.gaps; r.naive_jumped = nv.jumped;
    printf("  %-52s truth=%-5u lost=%-5u late=%-4u holes=%-3u max=%-3u | naive gaps=%-5u jumped=%-5u gated=%-4u | differ=%u\n",
           s->name, r.truth, r.l.lost, r.l.late, r.l.holes, r.l.hole_max,
           r.naive_gaps, r.naive_jumped, r.gated, r.differ);
    free(tr.v);
    return r;
}

/* 1. The randomized scenarios of the assessment, each against its ground truth. */
static void scenarios(void)
{
    static const scen_t sc[] = {
        { "1 clean A A B B",                                  100000, 1000, 0,    0,              0,   0,    0,  0, 0 },
        { "2 one copy lost 5%",                               100000, 1000, 0.05, 0,              0,   0,    0,  0, 0 },
        { "3 11 bursts x 30 fr in 370 s (console freeze rate)", 37000, 1000, 0,    11.0 / 37000, 30,  0,    0,  0, 0 },
        { "4 jitter: 2% of packets +0-25 ms, NO loss",        100000, 1000, 0,    0,              0,   0.02, 25, 0, 0 },
        { "5 bursts + 1% one-copy + jitter, across 2^32 wrap", 200000, 0xFFFF0000u, 0.01, 11.0 / 37000, 30, 0.01, 25, 0, 0 },
        { "6 outages of 100 fr (1 s) with sound",               37000, 1000, 0,    5.0 / 37000,  100,  0,    0,  0, 0 },
        { "7 silent VM: DTX 400 ms, seq+1, outages of 3 fr",     5000, 1000, 0,    0.002,         3,   0,    0,  0, 5000 },
        { "8 sound + 400 s of silence, bursts x 30",            60000, 1000, 0.01, 11.0 / 37000, 30,  0.01, 25, 20000, 21000 },
    };
    enum { N = (int)(sizeof sc / sizeof sc[0]) };
    res_t r[N];
    char what[200];
    for (int i = 0; i < N; i++) r[i] = run(&sc[i]);

    for (int i = 0; i < N; i++) {
        snprintf(what, sizeof what, "%s: lost == ground truth (%u), nothing left pending", sc[i].name, r[i].truth);
        CHECK(r[i].l.lost == r[i].truth && audio_loss_pending(&r[i].l) == 0, what);
        snprintf(what, sizeof what, "%s: every accepted frame is noted once", sc[i].name);
        CHECK(r[i].l.noted == r[i].played, what);
        snprintf(what, sizeof what, "%s: the window's decisions are identical with the accountant attached", sc[i].name);
        CHECK(r[i].differ == 0, what);
        snprintf(what, sizeof what, "%s: the accountant's top follows the window's highest seq", sc[i].name);
        CHECK(r[i].top_drift == 0, what);
        snprintf(what, sizeof what, "%s: every refusal sorted, none of them stale (no packet is 64 frames late)", sc[i].name);
        CHECK(r[i].played + r[i].dup + r[i].stale == r[i].packets && r[i].stale == 0, what);
        snprintf(what, sizeof what, "%s: no renumbering", sc[i].name);
        CHECK(r[i].l.renum == 0, what);
    }

    CHECK(r[0].truth == 0 && r[0].l.deltas[AUDIO_LOSS_D1] == (uint32_t)(sc[0].nframes - 1)
          && r[0].l.deltas[AUDIO_LOSS_D2_3] == 0 && r[0].l.deltas[AUDIO_LOSS_D4_64] == 0
          && r[0].l.deltas[AUDIO_LOSS_D65_UP] == 0 && r[0].l.holes == 0 && r[0].l.late == 0,
          "1 clean: every forward jump is 1, no hole, nothing late");
    CHECK(r[1].truth == 0 && r[1].l.lost == 0 && r[1].dup < r[1].played,
          "2 LIMIT, stated: one lost copy in 20 reads 0 - the 2x redundancy hides single-copy loss");
    CHECK(r[2].truth > 0 && r[2].l.holes == r[2].runs && r[2].l.hole_max == r[2].max_run,
          "3 bursts: one hole per burst, and the longest hole is the longest burst");
    CHECK(r[3].l.lost == 0 && r[3].l.late > 0 && r[3].naive_gaps > 0,
          "4 COUNTER-CASE: under jitter the gap counted at the forward jump reports "
          "losses that never happened (the late frames played); the accountant reads 0");
    CHECK(r[4].truth > 0 && sc[4].base + (uint32_t)sc[4].nframes < sc[4].base,
          "5 the trace really crosses 2^32, and still counts exactly");
    CHECK(r[5].naive_gaps < r[5].truth && r[5].naive_jumped > 0
          && r[5].l.holes == r[5].runs && r[5].l.hole_max == r[5].max_run,
          "6 COUNTER-CASE: setting jumps beyond 64 apart hides the 1 s outages; "
          "the accountant counts them as holes of the right length");
    CHECK(r[6].truth > 0 && r[6].gated == 0 && r[6].l.lost == r[6].truth,
          "7 COUNTER-CASE: a 'previous frame active' gate hides every loss on a silent VM; "
          "the seq counts SENT frames, so no gate is needed");
}

/* 2. The edges: renumbering both ways (the backward one through ING-A1's
 * re-sync, audio_dedup.h), session start, the observed silent-VM wire, S36
 * with and without the re-sync, and the provisional gap. */
static void edges(void)
{
    /* Forward renumbering by 2^31, then a burst of 30. */
    {
        site_t s; memset(&s, 0, sizeof s);
        uint32_t q;
        for (q = 1; q <= 1000; q++) { site_accept(&s, 0x12, q); site_accept(&s, 0x12, q); }
        const uint32_t nb = 0x80000000u;
        for (q = nb; q < nb + 1000; q++) {
            if (q >= nb + 400 && q < nb + 430) continue;
            site_accept(&s, 0x12, q); site_accept(&s, 0x12, q);
        }
        CHECK(s.l.lost == 30 && s.l.renum == 1,
              "a forward renumbering (+2^31) re-primes, is not 2^31 lost frames, and the burst after it is counted (30)");
    }
    /* Backward renumbering: the window follows it (ING-A1: 8 packets in a row
     * beyond it re-prime it), and so must the accountant - otherwise every
     * later frame would sit 50 000 behind its top. */
    {
        site_t s; memset(&s, 0, sizeof s);       /* zeroed = the ING-A1 rule */
        uint32_t q;
        for (q = 50000; q < 51000; q++) { site_accept(&s, 0x12, q); site_accept(&s, 0x12, q); }
        for (q = 0; q < 1000; q++) {
            if (q >= 400 && q < 430) continue;
            site_accept(&s, 0x12, q); site_accept(&s, 0x12, q);
        }
        CHECK(s.d.resyncs == 1 && s.stale == 7,
              "a backward renumbering: the window re-syncs on the 8th packet beyond it, the 7 before are stale");
        CHECK(s.l.renum == 1 && s.l.lost == 30,
              "... the accountant follows it (renum=1), and the burst after it is counted (30)");
    }
    /* Session start: seq 1 before the descriptor's seq 0. */
    {
        site_t s; memset(&s, 0, sizeof s);
        static const uint32_t order[] = { 1, 0, 1, 0, 2, 2, 3, 3 };
        for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) site_accept(&s, 0x12, order[i]);
        for (uint32_t q = 4; q < 200; q++) { site_accept(&s, 0x12, q); site_accept(&s, 0x12, q); }
        CHECK(s.l.lost == 0 && s.l.late == 1,
              "start: seq 1 before seq 0 is one late frame, not a loss (primed all-seen)");
    }
    /* The observed silent-VM wire (af2A.log et al.): the 0x02 descriptor at seq
     * 0, twice, then 0x12 frames seq 1.. each twice, 5 a second, 10 minutes. */
    {
        site_t s; memset(&s, 0, sizeof s);
        site_accept(&s, 0x02, 0); site_accept(&s, 0x02, 0);
        for (uint32_t q = 1; q <= 3000; q++) { site_accept(&s, 0x12, q); site_accept(&s, 0x12, q); }
        CHECK(s.l.lost == 0 && s.l.deltas[AUDIO_LOSS_D1] == 2999 && s.l.holes == 0,
              "silent VM wire: 0 lost, every jump 1 - the descriptor is not noted, seq 1 primes");
        CHECK(s.dup == s.played && s.stale == 0,
              "silent VM wire: dup == played (every frame twice), as the 5 s stats line shows");
    }
    /* S36, under the rule as it stood before ING-A1 (SHADOW_AUDIO_DEDUP_RESYNC=0):
     * a window primed high, then a stream that numbers from 1 again. */
    {
        site_t s; memset(&s, 0, sizeof s);
        s.d.no_resync = true;
        uint32_t q;
        for (q = 1000000; q < 1000100; q++) { site_accept(&s, 0x12, q); site_accept(&s, 0x12, q); }
        const uint32_t played0 = s.played, lost0 = s.l.lost;
        for (q = 1; q <= 4000; q++) { site_accept(&s, 0x12, q); site_accept(&s, 0x12, q); }
        CHECK(s.played == played0 && s.l.lost == lost0,
              "S36 without the re-sync, the stated blind spot: nothing plays and the loss count does not move");
        CHECK(s.stale == 8000,
              "S36: the refusals are counted apart as stale - all 8000 packets, none as duplicates");
    }
    /* The same stream under ING-A1's rule (the default): the window re-syncs,
     * and the accountant follows it instead of counting a million lost. */
    {
        site_t s; memset(&s, 0, sizeof s);
        uint32_t q;
        for (q = 1000000; q < 1000100; q++) { site_accept(&s, 0x12, q); site_accept(&s, 0x12, q); }
        const uint32_t played0 = s.played, lost0 = s.l.lost;
        for (q = 1; q <= 4000; q++) { site_accept(&s, 0x12, q); site_accept(&s, 0x12, q); }
        CHECK(s.played - played0 == 3997 && s.stale == 7 && s.l.renum == 1 && s.l.lost == lost0,
              "the same stream under ING-A1: 3997 of 4000 frames play, 7 packets are stale, "
              "and the accountant follows (renum=1) instead of counting a million lost");
    }
    /* The return value is PROVISIONAL, `lost` is FINAL, `pending` is in between. */
    {
        audio_loss_t l; memset(&l, 0, sizeof l);
        uint32_t q, g = 0;
        for (q = 1; q <= 100; q++) audio_loss_note(&l, q);
        CHECK(audio_loss_pending(&l) == 0, "primed all-seen: nothing pending after a clean run");
        g = audio_loss_note(&l, 131);                 /* 101..130 missing */
        CHECK(g == 30 && l.lost == 0 && audio_loss_pending(&l) == 30,
              "a gap of 30 is REPORTED at once (the event line) and PENDING: lost + pending reads "
              "30 right after a 300 ms burst, where lost alone reads 0 (the D4 companion line)");
        audio_loss_note(&l, 115);                      /* one of them arrives late */
        CHECK(audio_loss_pending(&l) == 29 && l.late == 1, "a late frame fills one pending slot");
        for (q = 132; q <= 194; q++) audio_loss_note(&l, q);
        CHECK(l.lost == 29 && l.late == 1 && l.holes == 1 && l.hole_max == 30
              && audio_loss_pending(&l) == 0,
              "64 frames later the loss is final: 29, the late frame that filled a slot is not counted");
        CHECK(l.noted == 165, "every note is counted: 100 + 1 + 1 + 63 = 165 frames noted");
        CHECK(audio_loss_note(&l, 194) == 0 && l.late == 1,
              "the top seq noted again changes nothing (the window never lets it through)");
    }
    /* The stale / duplicate boundary. */
    CHECK(!audio_loss_is_stale(1000, 1000 - 63) && audio_loss_is_stale(1000, 1000 - 64),
          "stale = 64 or more behind the window's highest; 63 behind is a duplicate candidate");
    CHECK(!audio_loss_is_stale(5, 0xFFFFFFF0u) && audio_loss_is_stale(5, 5u - 100u)
          && !audio_loss_is_stale(0xFFFFFFF0u, 5),
          "across the 2^32 wrap: 21 behind is not stale, 100 behind is, and ahead never is");
}

/* 3. The grading window of the panel's row. Rates are in milli-frames per
 * second, so that 99.4 accepted + 0.6 lost frames a second can be fed. */
typedef struct { audio_loss_win_t w; int64_t t_ms; uint64_t acc_m, lost_m; } feed_t;

static void feed(feed_t *f, int ms, uint32_t acc_mfps, uint32_t lost_mfps, int step_ms)
{
    for (int e = 0; e < ms; e += step_ms) {
        f->t_ms  += step_ms;
        f->acc_m  += (uint64_t)acc_mfps  * (uint64_t)step_ms / 1000u;
        f->lost_m += (uint64_t)lost_mfps * (uint64_t)step_ms / 1000u;
        audio_loss_win_sample(&f->w, f->t_ms, (uint32_t)(f->acc_m / 1000u), (uint32_t)(f->lost_m / 1000u));
    }
}

static void grading(void)
{
    uint32_t e, l;
    audio_loss_grade_t g;

    /* A silent VM, 5 frames a second, one frame lost. */
    {
        feed_t f; memset(&f, 0, sizeof f); f.t_ms = 5000000;
        feed(&f, 12000, 5000, 0, 1000);
        f.lost_m += 1000;
        feed(&f, 1000, 5000, 0, 1000);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_NONE && l == 1 && e < AUDIO_LOSS_GRADE_MIN,
              "silence: one lost frame in ~50 is NOT graded - a neutral count");
        CHECK((uint64_t)l * 1000u > (uint64_t)e * AUDIO_LOSS_WARN_PERMIL,
              "COUNTER-CASE: the same window without the floor is over the Warn threshold (~2 %)");
    }
    /* Sound, 100 frames a second: clean, 0.6 %, 3 %. */
    {
        feed_t f; memset(&f, 0, sizeof f); f.t_ms = 5000000;
        feed(&f, 12000, 100000, 0, 1000);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_OK && l == 0 && e >= 1000 && e <= 1100,
              "sound, no loss: graded OK over ~10 s (1000-1100 frames expected)");
    }
    {
        feed_t f; memset(&f, 0, sizeof f); f.t_ms = 5000000;
        feed(&f, 12000, 99400, 600, 1000);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_WARN, "sound, 0.6 % lost: Warn");
    }
    {
        feed_t f; memset(&f, 0, sizeof f); f.t_ms = 5000000;
        feed(&f, 12000, 97000, 3000, 1000);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_BAD, "sound, 3 % lost: Bad");
    }
    /* Heavy loss during sound: 40 accepted + 30 lost a second. */
    {
        feed_t f; memset(&f, 0, sizeof f); f.t_ms = 5000000;
        feed(&f, 12000, 40000, 30000, 1000);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_BAD && e - l < AUDIO_LOSS_GRADE_MIN,
              "COUNTER-CASE: 43 % loss with only ~400 frames accepted is Bad - a floor on "
              "ACCEPTED frames would have shown it neutral");
    }
    /* L17: the published counters restart with the session. */
    {
        feed_t f; memset(&f, 0, sizeof f); f.t_ms = 5000000;
        feed(&f, 12000, 97000, 3000, 1000);
        f.acc_m = 0; f.lost_m = 0;
        feed(&f, 16, 0, 0, 16);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_NONE && e == 0 && l == 0,
              "L17/L21: counters that step back restart the window - no negative delta read as 4 billion");
        feed(&f, 12000, 100000, 0, 1000);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_OK && l == 0,
              "... and the new session is graded on its own frames only");
    }
    /* The window slides: a burst leaves it after ~10 s. */
    {
        feed_t f; memset(&f, 0, sizeof f); f.t_ms = 5000000;
        feed(&f, 12000, 100000, 0, 1000);
        f.lost_m += 50000;
        feed(&f, 3000, 100000, 0, 1000);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_BAD && l == 50, "a burst of 50 lost frames: Bad while it is in the window");
        feed(&f, 12000, 100000, 0, 1000);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_OK && l == 0, "... and OK once it has left the last 10 s");
    }
    /* Sampled at the draw rate, not once a second. */
    {
        feed_t f; memset(&f, 0, sizeof f); f.t_ms = 5000000;
        feed(&f, 15008, 100000, 0, 16);
        g = audio_loss_win_grade(&f.w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_OK && e >= 1000 && e <= 1100,
              "sampled every 16 ms: the same ~10 s window");
    }
    /* Nothing held yet. */
    {
        audio_loss_win_t w; memset(&w, 0, sizeof w);
        g = audio_loss_win_grade(&w, &e, &l);
        CHECK(g == AUDIO_LOSS_GRADE_NONE && e == 0 && l == 0 && audio_loss_win_grade(&w, NULL, NULL) == AUDIO_LOSS_GRADE_NONE,
              "an empty window grades nothing, and accepts NULL outputs");
    }
}

int main(void)
{
    printf("== audio frames that never arrived (streaming/audio_loss.h) ==\n");
    scenarios();
    edges();
    grading();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
