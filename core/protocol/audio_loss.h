/* audio_loss.h - audio frames that never arrived, counted from the sequence
 * numbers the anti-duplicate window ACCEPTS; and how the panel grades them.
 *
 * PURE module: no dependency, no global state, testable offline
 * (tests/test_audio_loss.c). Header-only like audio_dedup.h, and valid C++:
 * the metrics panel includes it for the grading window at the end.
 *
 * === AUD-DEDUP-3 2026-09-11 - THE WINDOW SAW EVERY GAP AND THREW IT AWAY ===
 *
 * audio_dedup_accepte() computes how far a frame is ahead of the highest number
 * seen, and keeps only the verdict "play it or not". Nothing counted a frame
 * that never came: no stat, no key on the 5 s line, no panel row. Losing ONE of
 * the two copies the server sends is harmless (and derivable: audio - 2 x dup).
 * Losing BOTH is a burst, and a burst is exactly the ~300 ms freeze of console
 * Wi-Fi that CLAUDE.md still cannot attribute: while sound plays, :base+30 is a
 * numbered, 100 frames/s witness of the same link.
 *
 * WHY AN ACCOUNTANT BESIDE THE WINDOW, NOT AN OUT-PARAMETER OF IT. Measured on
 * randomized traces against the real audio_dedup.h:
 *  - the window's decision stays bit-identical (0 differing accept decisions on
 *    ~1.2 M packets): this module is fed only the seqs the window accepted and
 *    cannot change what is played (S36);
 *  - counting `ecart - 1` at the forward jump counts as lost a frame that
 *    arrives late and is still played: 25 false losses per 100 k frames at 2 %
 *    jitter. Here a loss is FINAL only when its slot leaves a 64-slot bitmap
 *    unfilled;
 *  - a jump beyond 64 is not a renumbering: a 1.6 s outage is 160 frames, and
 *    setting such jumps apart hid 700 of 716 frames lost in 1 s outages. Only a
 *    jump beyond AUDIO_LOSS_RENUM frames re-primes.
 *
 * THE NUMBERING IS SETTLED BY EXISTING LOGS: the server numbers the frames it
 * SENDS. [AUD8] shows seq 1..7 strictly consecutive ~400 ms apart on a silent
 * VM, in 8 sessions. A DTX silence therefore leaves no gap, and no "previous
 * frame was active" gate is needed - one would hide every loss during silence
 * (24 of 24 in the bench).
 *
 * WHAT IT CANNOT SEE, to keep in mind when reading the number:
 *  - loss BEFORE the 2x redundancy: random single-copy loss reads 0;
 *  - a loss is final 64 frames later: 0.64 s with sound, 13-25 s on a silent
 *    VM (2.5-5 frames/s);
 *  - a window that refuses EVERYTHING as too old (S36): nothing is accepted, so
 *    nothing is noted and the count stays 0. audio_loss_is_stale() lets the
 *    caller count those refusals apart.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* A forward jump beyond this is a renumbering, not an outage: a minute of
 * sound. (The longest outage in the KB, D4's 143.6 s, was VIDEO; S41 measured
 * the audio flowing through it at 200 packets/s.) */
#define AUDIO_LOSS_RENUM 6000u

enum { AUDIO_LOSS_D1 = 0, AUDIO_LOSS_D2_3 = 1, AUDIO_LOSS_D4_64 = 2, AUDIO_LOSS_D65_UP = 3 };

/* Per session, like the window it follows: zero it ({0}) at session start. */
typedef struct {
    uint32_t top;         /* highest seq noted */
    uint64_t seen;        /* bit k = seq (top - k) was noted */
    bool     primed;      /* false = nothing noted yet this session */
    uint32_t noted;       /* frames noted: the accepted audio frames */
    uint32_t lost;        /* FINAL: slots that left the bitmap never noted */
    uint32_t late;        /* noted behind top: reordered, still played */
    uint32_t holes;       /* forward gaps (a jump of 2 or more) */
    uint32_t hole_max;    /* longest single forward gap, in frames */
    uint32_t renum;       /* re-primed on a renumbering, either direction */
    uint32_t deltas[4];   /* forward jumps: 1 / 2-3 / 4-64 / >64 */
} audio_loss_t;

/* Notes one ACCEPTED frame. Returns the forward gap this frame reveals, in
 * frames (0 if none). The return is PROVISIONAL - a late frame may still fill
 * part of it - and is meant for an event line; `lost` only counts what can no
 * longer arrive in time to be played. */
static inline uint32_t audio_loss_note(audio_loss_t *l, uint32_t seq)
{
    l->noted++;
    if (!l->primed) {
        /* Primed ALL-SEEN: the frames before the first one we get are not ours
         * to count. */
        l->primed = true;
        l->top    = seq;
        l->seen   = ~0ULL;
        return 0;
    }
    const int32_t d = (int32_t)(seq - l->top);
    if (d > 0) {
        l->deltas[d == 1 ? AUDIO_LOSS_D1 : d <= 3 ? AUDIO_LOSS_D2_3
                  : d <= 64 ? AUDIO_LOSS_D4_64 : AUDIO_LOSS_D65_UP]++;
        if ((uint32_t)d > AUDIO_LOSS_RENUM) {
            l->renum++;
            l->top  = seq;
            l->seen = ~0ULL;
            return 0;
        }
        if (d >= 64) {
            /* The whole bitmap leaves, and so does every slot the jump skips
             * beyond the new one. */
            l->lost += (64u - (uint32_t)__builtin_popcountll(l->seen)) + (uint32_t)(d - 64);
            l->seen  = 0;
        } else {
            /* The d oldest slots leave: the ones never noted are lost. */
            l->lost += (uint32_t)d - (uint32_t)__builtin_popcountll(l->seen >> (64 - d));
            l->seen <<= d;
        }
        l->seen |= 1;
        l->top   = seq;
        if (d > 1) {
            l->holes++;
            if ((uint32_t)(d - 1) > l->hole_max) l->hole_max = (uint32_t)(d - 1);
        }
        return (uint32_t)(d - 1);
    }
    if (d == 0) return 0;   /* top itself again: the window never lets that through */
    if (d > -64) {
        l->seen |= 1ULL << (-d);
        l->late++;
        return 0;
    }
    /* Accepted although far behind: only possible once the WINDOW itself
     * re-primed on a backward renumbering. Follow it rather than freeze. */
    l->renum++;
    l->top  = seq;
    l->seen = ~0ULL;
    return 0;
}

/* A frame the window REFUSED: true if it was too old to hold a slot, false if
 * it was a duplicate. `highest` is the window's highest seq, read AFTER the
 * refusal - a refusal leaves audio_dedup_t untouched, so it is the value the
 * decision used. Kept apart from the duplicates so that S36 - a window that
 * refuses everything as too old - reads as a number instead of as silence. */
static inline bool audio_loss_is_stale(uint32_t highest, uint32_t seq)
{
    return (int32_t)(seq - highest) <= -64;
}

/* Frames still MISSING inside the bitmap: behind `top`, never noted, and not
 * yet final - a late frame may still fill them. A reading that cannot wait 64
 * frames uses lost + pending: a 300 ms freeze leaves ~30 missing frames, fewer
 * than 64, so `lost` alone would read 0 right after it (the D4 companion
 * line); and at a session's end nothing can arrive any more. */
static inline uint32_t audio_loss_pending(const audio_loss_t *l)
{
    return l->primed ? 64u - (uint32_t)__builtin_popcountll(l->seen) : 0u;
}

/* === AUD-INS-1 2026-09-11 - HOW THE PANEL GRADES THE LOSS ROW ===
 *
 * Over the last 10 s, and only when those 10 s EXPECTED at least
 * AUDIO_LOSS_GRADE_MIN frames (accepted + lost), that is while sound plays
 * (100 frames/s). A silent VM sends 2.5-5 frames/s: ONE lost frame in 50 would
 * read 2 %, Bad, on a VM playing nothing. The floor is on EXPECTED frames, not
 * accepted ones: a heavy loss during sound (400 accepted, 300 lost in 10 s)
 * would otherwise fall under the floor and read neutral.
 *
 * Fed with two cumulative counters, as often as the caller likes; one snapshot
 * a second is kept. A counter that steps back is a new session (L17 resets the
 * published stats): the window starts again rather than compute a negative
 * delta (L21). Zero it ({0}) to start. */
#define AUDIO_LOSS_WIN_SLOTS   10u    /* seconds */
#define AUDIO_LOSS_GRADE_MIN   500u   /* expected frames in the window */
#define AUDIO_LOSS_WARN_PERMIL 5u     /* > 0.5 % */
#define AUDIO_LOSS_BAD_PERMIL  20u    /* > 2 % */

typedef enum {
    AUDIO_LOSS_GRADE_NONE = 0,   /* too few frames to grade: a neutral count */
    AUDIO_LOSS_GRADE_OK,         /* graded, 0.5 % or less */
    AUDIO_LOSS_GRADE_WARN,
    AUDIO_LOSS_GRADE_BAD
} audio_loss_grade_t;

typedef struct {
    uint32_t acc[AUDIO_LOSS_WIN_SLOTS + 1];    /* one snapshot a second */
    uint32_t lost[AUDIO_LOSS_WIN_SLOTS + 1];
    uint32_t cur_acc, cur_lost;               /* the latest sample */
    unsigned head;                            /* next snapshot slot */
    unsigned n;                               /* snapshots held */
    int64_t  next_ms;                         /* when the next snapshot is due */
} audio_loss_win_t;

static inline void audio_loss_win_sample(audio_loss_win_t *w, int64_t now_ms,
                                         uint32_t accepted, uint32_t lost)
{
    if (w->n > 0 && (accepted < w->cur_acc || lost < w->cur_lost)) {
        w->n    = 0;             /* the counters restarted: a new session */
        w->head = 0;
    }
    w->cur_acc  = accepted;
    w->cur_lost = lost;
    /* next_ms starts at 0: the first sample is kept at once, as wanted. */
    if (w->n > 0 && now_ms < w->next_ms) return;
    w->acc[w->head]  = accepted;
    w->lost[w->head] = lost;
    w->head = (w->head + 1) % (AUDIO_LOSS_WIN_SLOTS + 1);
    if (w->n < AUDIO_LOSS_WIN_SLOTS + 1) w->n++;
    w->next_ms = now_ms + 1000;
}

/* The grade of the last ~10 s, and what it was computed on (`expected`,
 * `lost`; both 0 when nothing is held). Either pointer may be NULL. */
static inline audio_loss_grade_t audio_loss_win_grade(const audio_loss_win_t *w,
                                                      uint32_t *expected, uint32_t *lost)
{
    uint32_t e = 0, l = 0;
    if (w->n > 0) {
        const unsigned oldest = (w->head + (AUDIO_LOSS_WIN_SLOTS + 1) - w->n)
                              % (AUDIO_LOSS_WIN_SLOTS + 1);
        l = w->cur_lost - w->lost[oldest];
        e = (w->cur_acc - w->acc[oldest]) + l;
    }
    if (expected) *expected = e;
    if (lost)     *lost     = l;
    if (e < AUDIO_LOSS_GRADE_MIN) return AUDIO_LOSS_GRADE_NONE;
    if ((uint64_t)l * 1000u > (uint64_t)e * AUDIO_LOSS_BAD_PERMIL)  return AUDIO_LOSS_GRADE_BAD;
    if ((uint64_t)l * 1000u > (uint64_t)e * AUDIO_LOSS_WARN_PERMIL) return AUDIO_LOSS_GRADE_WARN;
    return AUDIO_LOSS_GRADE_OK;
}
