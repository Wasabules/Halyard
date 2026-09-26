/* eq — tonal correction of the sound coming from the remote machine.
 *
 * === WHY BIQUADS, AND NOT SOMETHING ELSE (S90, 2026-08-29) ===
 *
 * The constraint is LATENCY. This sound accompanies a picture that already
 * arrives with a delay of its own, and everything added here adds to it.
 *
 * A linear-phase equaliser (FIR) delays the signal by half its length: for a
 * useful resolution in the bass that means several thousand samples, hence tens
 * of milliseconds. Unacceptable here, which is why this module does not offer
 * one.
 *
 * A cascade of biquads (IIR, minimum phase) has NO algorithmic latency: the
 * output sample is produced from the input sample of the same instant. A group
 * delay remains, but it is a few samples and concentrated around the corrected
 * frequencies — inaudible for this use.
 *
 * The cost is negligible: five bands, two channels, 48,000 samples per second,
 * five floating-point operations per band — under three million operations per
 * second on a processor that does a billion.
 *
 * === WHAT THIS MODULE IS ===
 *
 * PURE. No I/O, no `getenv`, no allocation, no global state: everything lives in
 * the `eq_t` - or the `eq_chain_t` - the caller owns. That is what makes it
 * verifiable offline
 * (tests/test_eq.c) — and it needs that more than most, for a precise reason: a
 * wrong biquad coefficient does not show. Either the filter DIVERGES — the sound
 * blows up within milliseconds — or it corrects the wrong frequency, or by the
 * wrong amount, and you spend hours tuning by ear a filter that does not do what
 * it claims.
 *
 * So the test measures the REAL response: it injects a sine at the centre
 * frequency and compares the output amplitude to the requested gain. No
 * coefficient mistake survives that check.
 *
 * The formulas are those of Robert Bristow-Johnson's "Audio EQ Cookbook", the
 * field's reference for twenty years.
 */
#ifndef SHADOW_EQ_H
#define SHADOW_EQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Five bands. Enough to correct a speaker or a source, too few to get lost in —
 * and above all few enough to fit on a console screen without scrolling. A
 * thirty-band graphic equaliser would be unmanageable on a gamepad and would not
 * correct anything more. */
#define EQ_BANDS 5

/* The ORDER of this enum is persisted: `settings` writes the band type as an
 * integer into the settings file. Adding a value at the end is free; inserting
 * or reordering silently changes the filters of every existing installation. */
typedef enum {
    EQ_OFF = 0,        /* the band does nothing and costs nothing */
    EQ_HIGHPASS,       /* cuts below `freq` — the bass that built-in speakers
                        * cannot reproduce and that only eats headroom */
    EQ_LOWSHELF,       /* lifts or lowers the whole bass */
    EQ_PEAK,           /* dips or bumps around `freq`; `q` sets the width */
    EQ_HIGHSHELF,
    EQ_LOWPASS,
    EQ_TYPE_COUNT
} eq_type_t;

/* The name `eq_band_s` exists so a header can forward-declare it without
 * pulling this one in — `settings.hpp` takes advantage of that, and it is
 * included everywhere. */
typedef struct eq_band_s {
    eq_type_t type;
    float     freq;     /* Hz — clamped to [20, sample_rate/2 - 100] */
    float     q;        /* clamped to [0.1, 10]; 0.707 = no resonance */
    float     gain_db;  /* clamped to [-18, +18]; ignored for high/low-pass */
} eq_band_t;

typedef struct {
    /* Normalised coefficients (a0 = 1), per band. */
    float b0[EQ_BANDS], b1[EQ_BANDS], b2[EQ_BANDS];
    float a1[EQ_BANDS], a2[EQ_BANDS];
    bool  active[EQ_BANDS];

    /* State, per band AND PER CHANNEL. Sharing it between channels would blend
     * left and right through the filter — a cross-reverberation effect,
     * perfectly audible, and one that no frequency-response measurement would
     * ever show. */
    float z1[EQ_BANDS][2], z2[EQ_BANDS][2];

    /* Attenuation applied at the output so that no setting can clip. Computed at
     * configuration time by evaluating the cascade's response over a frequency
     * grid: that is exact, unlike "the sum of the positive gains", which
     * overestimates as soon as two bands overlap. 1 = none. */
    float trim;

    bool  bypass;       /* no band active: `eq_process` touches nothing */
} eq_t;

/* Bounds exposed, so the UI offers exactly what the module accepts — a screen
 * offering a setting the engine then clamps is lying to the user. */
#define EQ_FREQ_MIN     20.0f
#define EQ_Q_MIN         0.1f
#define EQ_Q_MAX        10.0f
#define EQ_GAIN_MIN    -18.0f
#define EQ_GAIN_MAX     18.0f

/* Computes the coefficients. `count` bands read from `bands` (at most
 * EQ_BANDS). Every value is CLAMPED here, never rejected: they come from a
 * settings file a human may have edited, and an equaliser silent because one
 * frequency was 0 would be harder to diagnose than a clamped one.
 *
 * The filter comes back FROM REST (state zeroed). That suits a filter nobody
 * listens to yet, and it is NOT how new settings go in under a running stream:
 * zeroing a band whose coefficients did not change IS a click. So identical
 * settings leave the running filter alone - see eq_same() - and a genuine
 * change is crossfaded - see eq_chain_t. (EQV1, 2026-09-11: this comment used
 * to say the opposite, that zeroing avoids the click.) */
void eq_configure(eq_t *e, const eq_band_t *bands, int count, uint32_t sample_rate,
                  bool auto_trim);

/* Clears the state without touching the coefficients. Call it when the source
 * changes: state inherited from another sound produces a transient on the
 * first sample.
 *
 * EQV2 2026-09-11 - it had NO caller, while the filter lives in a process
 * global (media/audio.c): a session that ended during sound handed its filter
 * memory to the next one, played out as a thump in the first buffer. Measured
 * on the real audio.c, HANDHELD, 50 random cuts: median -24 dBFS, worst
 * -8.1 dBFS, gone by 13 ms (68 ms under HEADPHONES). It now runs once per
 * session, before the audio decoder exists (audio_eq_session_start).
 * "Resuming after silence" is deliberately NOT a caller: a reset under a
 * running stream is itself a transient (~-21 dBFS measured). */
void eq_reset(eq_t *e);

/* EQV1(a) 2026-09-11 - true when `a` and `b` are the SAME FILTER, bit for bit:
 * same bypass, same trim, same active bands with the same five coefficients.
 * The state is ignored, and so are the coefficients of inactive bands - they
 * never touch the signal. Re-installing the same filter used to zero its state
 * anyway: a broadband click on every pause-menu row that re-applies the
 * settings (hardware Opus, audio quality). */
bool eq_same(const eq_t *a, const eq_t *b);

/* === EQV1(b) 2026-09-11 - INSTALLING A CHANGED FILTER UNDER A RUNNING STREAM ===
 *
 * A new filter cannot take over from one sample to the next without a
 * transient, and both obvious ways of giving it a state are wrong. MEASURED on
 * the real eq.c (bench_dsp_EQV1: 4 presets x every pause-menu notch x 7 signals
 * x 9 switch instants = 19440 cases, plus the 20 profile switches):
 *   - zeroing it, what eq_configure returns, restarts every band from rest: a
 *     broadband click, worst 16552 LSB (-5.9 dBFS) over ~9 ms;
 *   - carrying the old state is WORSE for a changed band. The state of a
 *     transposed direct form II is a coefficient-weighted sum, and a
 *     low-frequency pole amplifies the mismatch: carrying it between bands of
 *     the same type reached 28430 LSB (-1.2 dBFS) where zeroing gave 15335, and
 *     even the exact per-band carry is worse than zeroing in 1541 cases (a band
 *     after a changed one has seen a different input).
 * What is never worse than zeroing, in 0 of the 19440 cases: run the new filter
 * BESIDE the old one for 20 ms - from rest, or from the old state for a band
 * whose five coefficients are identical - then crossfade over 10 ms. Worst
 * notch 639-769 LSB, worst profile switch 150-266. The new setting is heard
 * 20 ms after the press and complete at 30 ms.
 *
 * PURE like the rest of this module: the caller owns the chain and serialises
 * every call on it (media/audio.c: `g_eq_mtx`). The scratch buffer lives
 * inside it - no copy of a buffer on the stack, where a libnx thread has
 * little room, and no allocation on the audio path. */
#define EQ_WARM_FRAMES          960u   /* 20 ms at 48 kHz */
#define EQ_FADE_FRAMES          480u   /* 10 ms */
#define EQ_CHAIN_SCRATCH_FRAMES 480u   /* longer buffers go through in chunks */

typedef struct {
    eq_t     live;       /* what is audible */
    eq_t     pend;       /* its replacement: warming up, then fading in */
    eq_t     next;       /* a newer target that arrived DURING a fade */
    bool     pending;    /* `pend` is in use */
    bool     queued;     /* `next` is in use */
    uint32_t warm_left;  /* frames of warm-up left before the fade */
    uint32_t fade_pos;   /* frames of the fade already played */
    int16_t  scratch[EQ_CHAIN_SCRATCH_FRAMES * 2];
} eq_chain_t;

typedef enum {
    EQ_CHAIN_SAME = 0,   /* the filter already targeted: nothing moves */
    EQ_CHAIN_ARMED,      /* warm-up then fade armed (a warming target replaced) */
    EQ_CHAIN_QUEUED,     /* a fade is playing - never cut: this one follows it */
    EQ_CHAIN_CANCELLED,  /* back to the audible filter before its replacement
                          * was heard: dropped, as if nothing had happened */
    EQ_CHAIN_SWAPPED     /* crossfade off: installed at once, from rest */
} eq_chain_result_t;

/* `live` NULL gives the neutral filter. Otherwise it is taken AS IS, state
 * included. */
void eq_chain_init(eq_chain_t *c, const eq_t *live);

/* Hands over a newly configured filter (its state is ignored). It is compared
 * with the TARGET - the queued filter, else the pending one, else the live one:
 * the same filter changes nothing. With `crossfade` false a changed filter
 * replaces everything at once, from rest - the behaviour before EQV1(b), kept
 * as the SHADOW_EQ_CROSSFADE=0 revert. Copies structures only, so it is cheap
 * under a lock. Returns an eq_chain_result_t. */
int eq_chain_set(eq_chain_t *c, const eq_t *fresh, bool crossfade);

/* IN PLACE, like eq_process - and IS eq_process in steady state. The second
 * filter runs only while a change is in flight. `channels` is 1 or 2. */
void eq_chain_process(eq_chain_t *c, int16_t *pcm, size_t frames, int channels);

/* EQV2 with the chain: the latest target becomes the live filter, FROM REST,
 * and nothing is left in flight. For a new session - see eq_reset(). */
void eq_chain_reset(eq_chain_t *c);

/* The latest filter asked for - what the settings describe. Non-null for a
 * non-null chain. */
const eq_t *eq_chain_target(const eq_chain_t *c);

/* false: eq_chain_process would leave the signal untouched (a neutral live
 * filter and nothing in flight). */
bool eq_chain_armed(const eq_chain_t *c);

/* Processes `frames` interleaved frames, IN PLACE. `channels` is 1 or 2.
 *
 * Does NOTHING when no band is active — that is the default, and it must cost
 * nothing. */
void eq_process(eq_t *e, int16_t *pcm, size_t frames, int channels);

/* Gain of the cascade at `freq`, in dB. Used to draw the curve and by the test.
 * Read-only: does not touch the state. */
float eq_response_db(const eq_t *e, float freq, uint32_t sample_rate);

/* --- Presets ----------------------------------------------------------- */

/* Presets are TIED TO THE CONSOLE'S MODE rather than chosen once and for all,
 * because the problem to correct is not the same: docked, the sound goes to a
 * TV or an amplifier, which needs nothing; handheld, it comes out of two
 * speakers a few centimetres wide, unable to reproduce bass and clipping when
 * you send them any.
 *
 * The ORDER is persisted here too — see `eq_type_t`. */
typedef enum {
    EQ_PRESET_FLAT = 0,     /* nothing at all — the default when docked */
    EQ_PRESET_HANDHELD,     /* built-in speakers */
    EQ_PRESET_HEADPHONES,
    EQ_PRESET_VOICE,        /* dialogue and communication */
    EQ_PRESET_NIGHT,        /* listening at low volume */
    EQ_PRESET_CUSTOM,       /* the user's own bands */
    EQ_PRESET_COUNT
} eq_preset_t;

/* Fills `bands` (EQ_BANDS entries) with the requested preset. Returns the number
 * of useful bands. `EQ_PRESET_CUSTOM` returns 0 and touches nothing: the caller
 * then keeps its own. */
int eq_preset(eq_preset_t p, eq_band_t *bands);

/* Short name of a preset, for the screen and the log. Never null.
 *
 * NOT TRANSLATED, DELIBERATELY: the returned strings are i18n KEYS, looked up in
 * `resources/i18n/` files. They are a data interface, like the settings-file
 * keys — renaming one here without renaming it in every locale file gives a
 * screen that displays its own key. */
const char *eq_preset_key(eq_preset_t p);
const char *eq_type_key(eq_type_t t);

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_EQ_H */
