/* audio_gap.h - output underruns: the audio output ran dry in the middle of
 * continuous sound. Counted two-sided and confirmed, time as a parameter.
 *
 * PURE module: no dependency, no global state, no clock, no getenv - testable
 * offline (tests/test_audio_gap.c), same shape as freeze_stat.h. The caller
 * (media/audio.c, one site per output backend) owns the clock, the question
 * it asks the output, the toggle and the log strings; this file only decides
 * WHAT is an underrun. Header-only, and valid C++.
 *
 * === OUT-1 2026-09-11 - EVERY OUTPUT PATH WAS BLIND TO ITS OWN GAPS ===
 *
 * Nothing counted an underrun. On ALSA snd_pcm_recover() returns 0 for -EPIPE,
 * -ESTRPIPE and -EINTR, and with silent=1 prints nothing (checked on libasound
 * 1.2.14): 'writei FAIL' only means that the RECOVERY failed. So L12's "20 ms,
 * NO underrun (writei FAIL = 0)" read a line that cannot fire for one. audout
 * counts overflow only (buf_drop_count). And [L5] audio/file samples the queue
 * right after a write, at its fullest: even a minimum could never show it dry.
 *
 * WHY THIS RULE, measured in a simulator with known ground truth (ALSA hw
 * model and audout model, 0.25 ms ticks, 90 s per scenario - scratch
 * assess_OUT-1/classifier_sim.c, ported into the test):
 *  - the finding's ALSA counter, every -EPIPE, counts 88 in 90 s on a SILENT
 *    VM: snd_pcm_set_params puts the start threshold at the buffer size, so a
 *    lone 10 ms idle frame every ~300 ms makes a hw device cycle prepare ->
 *    start -> xrun again and again. Raw xruns stay a SEPARATE counter here
 *    (audio_gap_xrun), never added into the underruns;
 *  - its audout counter, "in-flight == 0 after an audible frame", is
 *    one-sided: every sound that ENDS drains the queue after an audible frame,
 *    9 to 18 false counts on nine sound ends. And our in-flight count sees
 *    releases late (L11), so it misses short gaps: 1 of the 4 dry episodes of
 *    one 80 ms stall, 185 of 469 under heavy jitter;
 *  - an audibility test read AFTER the gain reads 0 everywhere under
 *    SHADOW_VOLUME=0 - the setting every unattended campaign uses.
 * The rule below counts 0 false underruns in every scenario and finds every
 * real one.
 *
 * THE RULE. One underrun is counted when ALL of these hold:
 *  1. the output was OBSERVED dry when a frame reached it (the refill);
 *  2. the frame handed to the output before the gap was audible;
 *  3. the refill frame is audible;
 *  4. the NEXT frame reaches the output within AUDIO_GAP_CONFIRM_US and is
 *     audible - the stream resumed its cadence, so the source was continuous.
 * (2) and (3) set apart idle frames and the start of a sound; (4) sets apart a
 * sound that ends: Opus's first idle frame after music decodes with the
 * overlap tail (peak 2439), audible, and then the stream goes quiet. Without
 * (4) nine sound ends give 9 false counts.
 *
 * AUDIBILITY is the peak of the DECODED PCM, before the EQ and before the gain
 * (both rewrite the buffer in place). The silent VM's idle packet `f4 ff fe`
 * decodes to 480 samples of exact 0 (libopus 1.6.1, 5 decodes of 5); FLAC is
 * lossless, so its silence is exact 0 too. Opus's decay after music reads
 * 2424, 63, 13, 11: the threshold of 16 hears the first two frames of it.
 * Blind spot, stated: a passage quieter than -66 dBFS (peak <= 16) reads as
 * silence. The finding's 64 was blind below -54 dBFS: at -51 dBFS, 218 of 295
 * Opus-decoded frames read as silence. Cost of the scan: 171.6 ns per 10 ms
 * frame on the i7-9750H.
 *
 * "OBSERVED DRY" is the backend's own answer, taken at the refill:
 *  - audout: after the reclaim loop, when our in-flight count says
 *    AUDIO_GAP_QUERY_INFLIGHT buffers or fewer, ONE
 *    audoutGetAudioOutPlayedSampleCount(); dry if played == samples_submitted.
 *    Asked before the new frame is counted in flight. Only when the answer can
 *    count (audio_gap_query_useful): a silent VM costs no IPC at all;
 *  - ALSA: when the play thread gets data back after WAITING, dry if the state
 *    is XRUN (hw device) or snd_pcm_delay() <= 0 (plugins);
 *  - WASAPI (Windows, OUT-3): dry if GetCurrentPadding() reads 0 at the refill.
 *
 * ONE COUNT PER EPISODE. An episode is a run of dry observations with no frame
 * ACCEPTED by the output between them. On ALSA the refill that meets the XRUN
 * hits -EPIPE and is lost; the next refill finds the device PREPARED with
 * delay 0 - still the same silence, counted once. Which is why
 * audio_gap_played() must be called only for a frame the output accepted.
 *
 * THE GAP LENGTH, in the log's "(X ms)", is an ESTIMATE of how long the output
 * had nothing to play before the refill: from the instant what it was handed
 * should have run out. When the backend reports what it holds after a write
 * (queued_us >= 0: snd_pcm_delay, GetCurrentPadding) that instant is exact
 * while the device runs. Otherwise a play-out clock at the nominal rate is
 * used: exact on an output that starts playing what it is handed at once
 * (audout, WASAPI), up to the device clock's drift over one continuous run;
 * on an ALSA hw device it reads up to (start threshold - one chunk) long,
 * +10 ms at L12's 20 ms buffer, because the device waits for a full buffer
 * before it restarts. On ALSA the 10 ms chunk lost to the -EPIPE is not in
 * this figure: xrun= counts those.
 *
 * THREADS. One writer: the thread that feeds the output (the decode thread on
 * audout, the play thread on ALSA and WASAPI). The three published counters
 * are written with __atomic stores and read with audio_gap_read() from any
 * thread; each is exact, the three may be one event apart.
 *
 * PER SESSION. The state lives in struct audio_decoder, calloc'd per session.
 * Never in a function static: a frame of session N would then stand as the
 * "audible frame before the gap" of session N+1's first refill - which is
 * dry, since every session starts empty - and every session start would count
 * an underrun (the test's two-session counter-case). CLAUDE.md names this
 * defect family first.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Audible = decoded peak above this (|sample| over all channels). */
#define AUDIO_GAP_AUDIBLE          16
/* Condition 4: the next frame reaches the output within this. */
#define AUDIO_GAP_CONFIRM_US    30000
/* audout: ask the driver only when our in-flight count says this many buffers
 * or fewer. Our count runs about one buffer ahead of the driver (L11); in the
 * simulator this finds every dry episode, at 3 to 56 queries a second during
 * sound. */
#define AUDIO_GAP_QUERY_INFLIGHT    2

/* Returned by audio_gap_refill(). Flags: one refill can confirm the previous
 * candidate AND open a new one (two stalls 10-30 ms apart). */
enum {
    AUDIO_GAP_NONE    = 0,
    AUDIO_GAP_OPENED  = 1,   /* a candidate: dry between two audible frames */
    AUDIO_GAP_COUNTED = 2,   /* the previous candidate was confirmed: +1 underrun */
};

/* Session state. Zeroed (calloc, or audio_gap_reset) is a fresh session. */
typedef struct {
    /* The writer thread's own state. */
    bool     have_prev;         /* the output accepted a frame this session */
    bool     prev_audible;      /* ... and that frame was audible */
    bool     cur_audible;       /* the frame of the latest refill */
    bool     played_since_dry;  /* a frame was accepted since the last dry observation */
    bool     pending;           /* a candidate waits for condition 4 */
    int64_t  pend_us;           /* when its refill reached the output */
    int64_t  pend_gap_us;       /* its gap estimate */
    int64_t  drain_us;          /* when what the output was handed should run out */
    int64_t  gap_us;            /* confirmed gaps, total */
    uint32_t dry_seen;          /* episodes observed after the first accepted frame */
    uint32_t candidates;        /* two-sided, confirmed or not (diagnostic) */
    /* Published: written by the writer thread only, read with audio_gap_read(). */
    uint32_t underruns;         /* underrun= */
    uint32_t gap_ms;            /* its "(X ms)" */
    uint32_t xruns;             /* xrun= : raw -EPIPE / -ESTRPIPE, every idle gap included */
} audio_gap_t;

/* memset rather than a compound literal: the header then also compiles as C++. */
static inline void audio_gap_reset(audio_gap_t *g)
{
    memset(g, 0, sizeof *g);
}

/* Peak |sample| over `n` interleaved samples (all channels). 32768 for -32768:
 * an int32_t, so the most negative sample does not wrap to itself. */
static inline int32_t audio_gap_peak(const int16_t *pcm, size_t n)
{
    int32_t pk = 0;
    if (!pcm) return 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : (int32_t)pcm[i];
        if (v > pk) pk = v;
    }
    return pk;
}

static inline bool audio_gap_audible(int32_t peak)
{
    return peak > AUDIO_GAP_AUDIBLE;
}

/* Whether asking the output "are you dry?" at this refill can count anything:
 * only after an audible accepted frame, for an audible refill, once per
 * episode. The audout caller uses it to spare the IPC: a silent VM then costs
 * no query at all (the ungated rule asks ~3 times a second on idle frames). */
static inline bool audio_gap_query_useful(const audio_gap_t *g, bool audible)
{
    return g->have_prev && g->prev_audible && g->played_since_dry && audible;
}

/* A frame reaches the output. Call it for EVERY frame, observed or not, before
 * the frame is counted in flight: it is also what confirms or drops the
 * previous candidate. `dry` = the output was observed dry just now (false when
 * nothing was asked). `audible` = audio_gap_audible(peak of the decoded PCM,
 * before EQ and gain). `now_us` = a monotonic time. */
static inline unsigned audio_gap_refill(audio_gap_t *g, int64_t now_us, bool dry, bool audible)
{
    unsigned ev = AUDIO_GAP_NONE;
    if (g->pending) {
        if (now_us - g->pend_us <= AUDIO_GAP_CONFIRM_US && audible) {
            g->gap_us += g->pend_gap_us;
            __atomic_fetch_add(&g->underruns, 1u, __ATOMIC_RELAXED);
            __atomic_store_n(&g->gap_ms, (uint32_t)(g->gap_us / 1000), __ATOMIC_RELAXED);
            ev |= AUDIO_GAP_COUNTED;
        }
        g->pending = false;
    }
    g->cur_audible = audible;
    /* Before the first accepted frame the output is empty by construction:
     * that is a start, not a gap. */
    if (dry && g->have_prev && g->played_since_dry) {
        g->played_since_dry = false;
        g->dry_seen++;
        if (g->prev_audible && audible) {
            int64_t gap = now_us - g->drain_us;
            if (gap < 0) gap = 0;
            g->candidates++;
            g->pending     = true;
            g->pend_us     = now_us;
            g->pend_gap_us = gap;
            ev |= AUDIO_GAP_OPENED;
        }
    }
    return ev;
}

/* The output ACCEPTED the frame of the latest refill. Never call it for a
 * frame it refused (audout: no free buffer) or lost (ALSA: -EPIPE): the next
 * dry observation would then open a second episode for the same silence.
 * `dur_us` = the frame's duration. `queued_us` = what the output holds right
 * after this write, when the backend says so; -1 when it does not (the
 * play-out clock is used). */
static inline void audio_gap_played(audio_gap_t *g, int64_t now_us, int64_t dur_us, int64_t queued_us)
{
    g->have_prev        = true;
    g->prev_audible     = g->cur_audible;
    g->played_since_dry = true;
    if (queued_us >= 0)
        g->drain_us = now_us + queued_us;
    else
        g->drain_us = (g->drain_us > now_us ? g->drain_us : now_us) + dur_us;
}

/* A raw -EPIPE / -ESTRPIPE from the output. A separate counter: on a hw device
 * it includes every idle gap of a silent VM (88 in 90 s in the simulator). */
static inline void audio_gap_xrun(audio_gap_t *g)
{
    __atomic_fetch_add(&g->xruns, 1u, __ATOMIC_RELAXED);
}

/* The published counters, from any thread. Any pointer may be NULL. */
static inline void audio_gap_read(const audio_gap_t *g, uint32_t *underruns,
                                  uint32_t *gap_ms, uint32_t *xruns)
{
    if (underruns) *underruns = __atomic_load_n(&g->underruns, __ATOMIC_RELAXED);
    if (gap_ms)    *gap_ms    = __atomic_load_n(&g->gap_ms, __ATOMIC_RELAXED);
    if (xruns)     *xruns     = __atomic_load_n(&g->xruns, __ATOMIC_RELAXED);
}
