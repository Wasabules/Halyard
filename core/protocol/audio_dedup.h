/* audio_dedup.h - every audio frame reaches us TWICE; which copy to play.
 *
 * PURE module: no dependency, no global state, testable offline
 * (tests/test_audio_dedup.c). Same intent as vid_wire.c and idr_policy.h -
 * isolate a rule that cost a full measurement campaign.
 *
 * The server sends each Opus frame twice on the `:base+30` channel: sensible
 * redundancy over a UDP transport with no retransmission (measured: 200 frames
 * per second for 10 ms frames, that is exactly twice real time). Playing both
 * doubles the duration you hear.
 *
 * So we keep a sliding window of the 64 sequence numbers already seen. A
 * window, not a comparison against the last number: with an A B A B
 * alternation, `seq != last` would let one frame in two through. The window
 * also absorbs reordering - a late frame is played if its slot is still free,
 * ignored otherwise.
 *
 * Created 2026-08-25 (campaign S36). ING-A1 (2026-09-11) gave it a way back
 * from a numbering discontinuity; ING-A4 (2026-09-11) its one door,
 * audio_dedup_admit(), with the two counters every audio path shares.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ING-A1 (2026-09-11): this many CONSECUTIVE packets beyond the window
 * re-prime it. 8 packets = 4 frames x 2 copies = 40 ms at 10 ms frames. A
 * two-frame rule was measured too and resynced falsely on stragglers (up to 57
 * differing decisions per million frames at 5 % stragglers); 8 never did. */
#define AUDIO_DEDUP_RESYNC_N 8

/* Window state. Must be reset ON EVERY SESSION: the sequence numbers it
 * compares itself against restart from zero too. Zero-initialisation gives the
 * ING-A1 rule; `no_resync` restores the rule as it stood before. */
typedef struct {
    uint32_t plus_haut;   /* plus haut numero vu */
    uint64_t vues;        /* the last 64, as bits */
    bool     amorcee;     /* false = no frame seen yet */
    bool     no_resync;   /* true = pre-ING-A1 rule (SHADOW_AUDIO_DEDUP_RESYNC=0) */
    uint8_t  beyond_run;  /* consecutive packets beyond the window */
    uint32_t resyncs;     /* re-primes performed: the witness the caller logs */
} audio_dedup_t;

/* Returns true if frame `seq` must be PLAYED, false if it is a duplicate or
 * too late to still hold its slot.
 *
 * COUNTER-CASE (S36, 2026-08-25) - SOUND IN THE FIRST SESSION ONLY.
 * This state lived in a function-level `static`, so it outlived the session.
 * The server renumbers on every stream: the next session sent numbers MUCH
 * LOWER than the retained `plus_haut`, the computed gap came out several
 * hundred thousand negative, and every frame was dropped as "too old".
 * Measured on console: the first session drops 1999 duplicates out of 3998
 * frames (one in two, correct); EVERY later session drops 3997 out of 3997 -
 * a hundred percent, not a single sample played. Hence sound heard exactly
 * once, on the very first launch.
 *
 * COUNTER-CASE (ING-A1, 2026-09-11) - ONE FRAME FAR AHEAD MUTES THE REST.
 * `plus_haut` never moved backwards: once it was ahead of the real stream,
 * every later frame was "too late", for as long as the stream took to catch
 * up. The reachable trigger was our own: a split-frame reassembly slot left by
 * the previous session (ING-A2, aud_reasm.h) completed with the new session's
 * chunk 1 and delivered the OLD, higher number into the fresh window - offline,
 * a FLAC session then played 174 [47..3246] of 6000 frames. A stream renumbered
 * after an AUD16 revival would do the same. A lone straggler is always followed
 * by a fresh packet, which resets the run; a stream that sits entirely beyond
 * the window never is, so 8 in a row re-prime the window. Bench: 0 differing
 * decisions in 658 018 calls of normal operation, decoded PCM identical in
 * 150/150 runs, 0 frames played twice; the stale-slot session 5996 of 6000, a
 * renumbered stream or a stray frame 99.8-99.9 % instead of 0 %; +0.15 ns per
 * call. */
static inline bool audio_dedup_accepte(audio_dedup_t *d, uint32_t seq)
{
    if (!d->amorcee) {
        d->amorcee    = true;
        d->plus_haut  = seq;
        d->vues       = 1;
        d->beyond_run = 0;
        return true;
    }
    const int32_t ecart = (int32_t)(seq - d->plus_haut);
    if (ecart > 0) {
        /* More recent: slide the window forward. */
        d->beyond_run = 0;
        d->vues  = (ecart >= 64) ? 0 : (d->vues << ecart);
        d->vues |= 1;
        d->plus_haut = seq;
        return true;
    }
    if (ecart > -64) {
        d->beyond_run = 0;
        const uint64_t bit = 1ULL << (-ecart);
        if (d->vues & bit) return false;   /* already played */
        d->vues |= bit;
        return true;
    }
    /* Beyond the window: a straggler, or a stream whose numbering jumped. */
    if (d->no_resync || ++d->beyond_run < AUDIO_DEDUP_RESYNC_N)
        return false;   /* too late to keep its slot */
    d->plus_haut  = seq;
    d->vues       = 1;
    d->beyond_run = 0;
    d->resyncs++;
    return true;
}

/* The sequence number of an audio plaintext `[0x12][seq u32 LE]...`; `p` must
 * hold at least 5 bytes. */
static inline uint32_t audio_dedup_seq(const uint8_t *p)
{
    return (uint32_t)p[1] | ((uint32_t)p[2] << 8)
         | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
}

/* === ING-A4 2026-09-11 - THE ONE DOOR, AND ITS TWO COUNTERS ===
 * Every audio frame that reaches the window goes through here: `*arrived`
 * counts it BEFORE the decision, duplicates included, and `*refused` counts a
 * refusal, so `*arrived - *refused` is exactly the frames played. A frame too
 * short to carry a number (under 5 bytes) is counted and played: there is
 * nothing to compare - which is what every path already did.
 *
 * COUNTER-CASE (ING-A4) - the single-packet path counted its frames before the
 * window, the two reassembly paths after it. With split FLAC frames in the
 * stream, which only real sound produces, `arrived - refused` - the panel's
 * count of audio frames played - read 1517 where 1993 had been played (-24 %,
 * live, 2026-09-11). On a silent VM nothing is split and nothing changes. */
static inline bool audio_dedup_admit(audio_dedup_t *d, const uint8_t *p, int len,
                                     uint32_t *arrived, uint32_t *refused)
{
    (*arrived)++;
    if (len < 5) return true;
    if (audio_dedup_accepte(d, audio_dedup_seq(p))) return true;
    (*refused)++;
    return false;
}
