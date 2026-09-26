/* test_eq.c — the equaliser, verified by MEASUREMENT rather than by re-reading.
 *
 * WHY THIS SUITE EXISTS. A wrong biquad coefficient does not show. Either the
 * filter DIVERGES — the sound saturates within milliseconds — or it corrects the
 * wrong frequency, or by the wrong amount, and you spend hours tuning by ear a
 * filter that does not do what it claims.
 *
 * Re-reading the cookbook's formulas proves nothing: that is precisely the
 * exercise where the eye slides past. So we INJECT a sine and MEASURE the output
 * amplitude. No coefficient mistake survives that check.
 *
 * The classic mistake is pinned by name: the cookbook defines `A` as the SQUARE
 * ROOT of the linear gain, and forgetting it gives a filter that corrects exactly
 * twice too much in dB. A test checking only the SIGN of the correction would let
 * it through.
 */
#include "../core/protocol/eq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d — %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

#define SR 48000u

/* Gain at `freq`, measured on a real signal.
 *
 * We throw away the first 20 milliseconds: an IIR filter starts from a zero
 * state and its steady state is not immediate. Measuring inside that window
 * would give a wrong gain, and a test failing for that reason would send you
 * hunting a defect that does not exist.
 *
 * === WHY RMS, AND NOT THE PEAK ===
 *
 * The first version took the peak, and it failed in the treble — three checks
 * out of 85, all above 8 kHz. It was NOT the filter: at 16 kHz sampled at
 * 48 kHz, a sine has only THREE points per period, and none of them necessarily
 * lands on its top. The sampled peak can fall 13 % below the real amplitude
 * depending on phase, i.e. more than a decibel — enough to fail a 0.5 dB
 * tolerance on a perfectly correct filter.
 *
 * The RMS value, by contrast, does not depend on the points' phase: over a few
 * dozen periods it equals the amplitude divided by the square root of two,
 * whatever the frequency. It is the right measure here, and the detour was worth
 * writing down: it would have been easy to conclude there was a coefficient
 * error in the treble and to "fix" a filter that had nothing wrong with it. */
static float measured_gain(const eq_band_t *bands, int count, float freq)
{
    eq_t e;
    eq_configure(&e, bands, count, SR, false);  /* no trim: we measure the filter */

    const size_t total = SR / 4;                /* 250 ms */
    const size_t skip  = SR / 50;               /* 20 ms of settling */
    static int16_t pcm[SR / 4];

    const float amp = 8000.0f;                  /* far from the ceiling: no clipping */
    double rms_in = 0.0;
    for (size_t i = 0; i < total; i++) {
        const float v = amp * sinf(2.0f * (float)M_PI * freq * (float)i / (float)SR);
        pcm[i] = (int16_t)v;
        if (i >= skip) rms_in += (double)v * v;
    }

    eq_process(&e, pcm, total, 1);

    double rms_out = 0.0;
    for (size_t i = skip; i < total; i++)
        rms_out += (double)pcm[i] * pcm[i];

    if (rms_in <= 0.0) return 1.0f;
    return (float)sqrt(rms_out / rms_in);
}

static float db(float linear)
{
    return 20.0f * log10f(linear > 1e-9f ? linear : 1e-9f);
}

/* A band that is off, or an equaliser with no band at all, must return the
 * signal BIT FOR BIT. Not "almost": this is the application's default case, and
 * the slightest alteration would be a permanent degradation of the sound for
 * everyone, with no setting to explain it. */
static void bypass(void)
{
    eq_t e;
    eq_band_t b[EQ_BANDS];
    memset(b, 0, sizeof b);

    eq_configure(&e, b, EQ_BANDS, SR, true);

    static int16_t before[4096], after[4096];
    for (int i = 0; i < 4096; i++) {
        before[i] = (int16_t)((i * 7919) % 65536 - 32768);
        after[i] = before[i];
    }
    eq_process(&e, after, 2048, 2);
    CHECK(memcmp(before, after, sizeof before) == 0,
          "no active band: the signal comes out BIT FOR BIT");

    eq_configure(&e, NULL, 0, SR, true);
    for (int i = 0; i < 4096; i++) after[i] = before[i];
    eq_process(&e, after, 2048, 2);
    CHECK(memcmp(before, after, sizeof before) == 0,
          "no band at all: same");

    /* A peak at 0 dB does nothing either, and must not cost anything. */
    b[0].type = EQ_PEAK; b[0].freq = 1000.0f; b[0].q = 1.0f; b[0].gain_db = 0.0f;
    eq_configure(&e, b, 1, SR, true);
    for (int i = 0; i < 4096; i++) after[i] = before[i];
    eq_process(&e, after, 2048, 2);
    CHECK(memcmp(before, after, sizeof before) == 0,
          "a peak at 0 dB is switched off, not applied");
}

/* THE TEST THAT MATTERS: is the requested gain the gain OBTAINED?
 *
 * COUNTER-CASE — THE FACTOR OF TWO. The cookbook defines `A = 10^(dB/40)`, i.e.
 * the SQUARE ROOT of the linear gain. Writing `10^(dB/20)` — which looks natural
 * — gives a filter that corrects exactly TWICE TOO MUCH in dB. The direction is
 * right, the curve has the right shape, and the setting is off by a factor of
 * two: exactly the kind of mistake you never find by ear. */
static void exact_gain(void)
{
    const float GAINS[] = { -12.0f, -6.0f, -3.0f, 3.0f, 6.0f, 12.0f };
    const float FREQS[] = { 120.0f, 1000.0f, 4000.0f };

    for (unsigned g = 0; g < sizeof GAINS / sizeof GAINS[0]; g++) {
        for (unsigned f = 0; f < sizeof FREQS / sizeof FREQS[0]; f++) {
            eq_band_t b = { EQ_PEAK, FREQS[f], 1.0f, GAINS[g] };
            const float got = db(measured_gain(&b, 1, FREQS[f]));
            const float diff = fabsf(got - GAINS[g]);
            if (diff >= 0.5f)
                printf("     %.0f Hz, asked %+.1f dB, got %+.1f dB\n",
                       (double)FREQS[f], (double)GAINS[g], (double)got);
            CHECK(diff < 0.5f,
                  "COUNTER-CASE: the gain MEASURED at the centre frequency equals "
                  "the requested gain (the cookbook's factor of two)");
        }
    }
}

/* A peak must correct ONLY its region. If it bleeds, two neighbouring bands
 * overlap and the settings become impossible to reason about. */
static void selectivity(void)
{
    eq_band_t b = { EQ_PEAK, 1000.0f, 2.0f, 12.0f };

    CHECK(db(measured_gain(&b, 1, 1000.0f)) > 11.5f, "the peak acts at its centre");
    CHECK(fabsf(db(measured_gain(&b, 1, 60.0f)))   < 1.0f,
          "... and leaves the bass alone four octaves away");
    CHECK(fabsf(db(measured_gain(&b, 1, 16000.0f))) < 1.0f,
          "... and the treble four octaves away");

    /* A larger Q must be NARROWER. That is the definition; inverting it is an
     * easy sign mistake to make inside `alpha`. */
    eq_band_t wide   = { EQ_PEAK, 1000.0f, 0.5f, 12.0f };
    eq_band_t narrow = { EQ_PEAK, 1000.0f, 5.0f, 12.0f };
    CHECK(db(measured_gain(&wide, 1, 2000.0f)) > db(measured_gain(&narrow, 1, 2000.0f)),
          "one octave away, the wide peak corrects MORE than the narrow one");
}

/* A high-pass cuts below its frequency and passes above. And it does NOT depend
 * on the gain: that field is ignored for this type, and a filter that took it
 * into account would behave differently according to an invisible setting. */
static void cutoffs(void)
{
    eq_band_t hp = { EQ_HIGHPASS, 200.0f, 0.707f, 0.0f };
    CHECK(db(measured_gain(&hp, 1, 40.0f))   < -20.0f, "the high-pass cuts two octaves lower");
    CHECK(fabsf(db(measured_gain(&hp, 1, 2000.0f))) < 0.6f, "... and passes above");
    CHECK(fabsf(db(measured_gain(&hp, 1, 200.0f)) + 3.0f) < 1.0f,
          "... and is -3 dB at its frequency, like a Butterworth high-pass");

    eq_band_t hp2 = { EQ_HIGHPASS, 200.0f, 0.707f, 12.0f };
    CHECK(fabsf(db(measured_gain(&hp, 1, 2000.0f)) - db(measured_gain(&hp2, 1, 2000.0f))) < 0.01f,
          "the gain is IGNORED on a high-pass: an unused field must change nothing");

    eq_band_t lp = { EQ_LOWPASS, 4000.0f, 0.707f, 0.0f };
    CHECK(db(measured_gain(&lp, 1, 16000.0f)) < -20.0f, "the low-pass cuts two octaves higher");
    CHECK(fabsf(db(measured_gain(&lp, 1, 400.0f))) < 0.6f, "... and passes below");
}

/* The shelves: flat far from their frequency, at the requested gain on the right
 * side. */
static void shelves(void)
{
    eq_band_t low = { EQ_LOWSHELF, 200.0f, 0.707f, 6.0f };
    CHECK(fabsf(db(measured_gain(&low, 1, 40.0f)) - 6.0f) < 0.7f,
          "the low shelf reaches its gain far below its frequency");
    CHECK(fabsf(db(measured_gain(&low, 1, 8000.0f))) < 0.5f,
          "... and does not touch the treble");

    eq_band_t high = { EQ_HIGHSHELF, 4000.0f, 0.707f, -6.0f };
    CHECK(fabsf(db(measured_gain(&high, 1, 16000.0f)) + 6.0f) < 0.7f,
          "the high shelf reaches its gain far above");
    CHECK(fabsf(db(measured_gain(&high, 1, 200.0f))) < 0.5f,
          "... and does not touch the bass");
}

/* COUNTER-CASE — STATE SHARED BETWEEN CHANNELS. Sharing it would blend left and
 * right through the filter: each channel would hear a residue of the other, with
 * the filter's delay. It is perfectly audible, and NO frequency-response
 * measurement would show it — both channels have the same curve. Only this check
 * catches it.
 *
 * So we send a signal on the LEFT and SILENCE on the right. */
static void independent_channels(void)
{
    eq_band_t b = { EQ_PEAK, 1000.0f, 1.0f, 12.0f };
    eq_t e;
    eq_configure(&e, &b, 1, SR, false);

    static int16_t pcm[4800 * 2];
    for (int i = 0; i < 4800; i++) {
        pcm[i * 2]     = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * 1000.0f * i / SR));
        pcm[i * 2 + 1] = 0;
    }
    eq_process(&e, pcm, 4800, 2);

    int leak = 0;
    for (int i = 0; i < 4800; i++) if (pcm[i * 2 + 1] != 0) leak++;
    CHECK(leak == 0,
          "COUNTER-CASE: a silent channel stays silent — the filter's state is "
          "NOT shared between left and right");
}

/* Stability over the whole settings domain. An unstable band does not merely
 * correct badly: it makes the signal diverge. So we sweep the parameter space
 * and check that no combination produces an aberrant output. */
static void stability(void)
{
    const float FREQS[] = { 20.0f, 50.0f, 200.0f, 1000.0f, 8000.0f, 20000.0f, 23000.0f, 40000.0f };
    const float QS[]    = { 0.05f, 0.1f, 0.707f, 3.0f, 10.0f, 50.0f };
    const float GAINS[] = { -30.0f, -18.0f, 0.0f, 18.0f, 30.0f };
    const eq_type_t TYPES[] = { EQ_HIGHPASS, EQ_LOWSHELF, EQ_PEAK,
                                EQ_HIGHSHELF, EQ_LOWPASS };

    static int16_t pcm[2400];
    int aberrant = 0, tried = 0;

    for (unsigned t = 0; t < sizeof TYPES / sizeof TYPES[0]; t++)
    for (unsigned f = 0; f < sizeof FREQS / sizeof FREQS[0]; f++)
    for (unsigned q = 0; q < sizeof QS / sizeof QS[0]; q++)
    for (unsigned g = 0; g < sizeof GAINS / sizeof GAINS[0]; g++) {
        eq_band_t b = { TYPES[t], FREQS[f], QS[q], GAINS[g] };
        eq_t e;
        eq_configure(&e, &b, 1, SR, true);

        /* A step: the hardest input for a resonant filter. */
        for (int i = 0; i < 2400; i++) pcm[i] = (i < 1200) ? 20000 : -20000;
        eq_process(&e, pcm, 2400, 1);

        tried++;
        /* The internal clamp makes it impossible by construction to leave the
         * `int16_t` range; what we are looking for is a DIVERGENCE, i.e. an
         * output pinned to the ceiling over its whole tail. */
        int clipped = 0;
        for (int i = 1800; i < 2400; i++)
            if (pcm[i] >= 32767 || pcm[i] <= -32768) clipped++;
        if (clipped > 550) aberrant++;
    }
    CHECK(aberrant == 0, "no type x frequency x Q x gain combination diverges");
    printf("     (%d parameter combinations exercised)\n", tried);
}

/* The trim must prevent clipping, and must not fire when there is nothing to
 * compensate — attenuating an equaliser that only cuts would make the sound
 * quieter for no visible reason. */
static void trim(void)
{
    eq_band_t boost = { EQ_PEAK, 1000.0f, 1.0f, 12.0f };
    eq_t e;
    eq_configure(&e, &boost, 1, SR, true);
    CHECK(e.trim < 0.3f, "a +12 dB bump brings an attenuation");
    CHECK(fabsf(eq_response_db(&e, 1000.0f, SR)) < 0.5f,
          "... which brings the response's peak back to 0 dB");

    eq_band_t cut = { EQ_PEAK, 1000.0f, 1.0f, -12.0f };
    eq_configure(&e, &cut, 1, SR, true);
    CHECK(e.trim > 0.999f,
          "a cut alone brings NO attenuation: nothing was clipping");

    /* And on a real signal, nothing sticks to the ceiling any more. */
    eq_configure(&e, &boost, 1, SR, true);
    static int16_t pcm[4800];
    for (int i = 0; i < 4800; i++)
        pcm[i] = (int16_t)(30000.0f * sinf(2.0f * (float)M_PI * 1000.0f * i / SR));
    eq_process(&e, pcm, 4800, 1);
    int clipped = 0;
    for (int i = 2400; i < 4800; i++)
        if (pcm[i] >= 32767 || pcm[i] <= -32768) clipped++;
    CHECK(clipped == 0,
          "a loud signal in a band bumped by +12 dB no longer clips");
}

/* The presets must be coherent: declared, stable, and free of aberrant bands.
 * That is little, but it catches a badly filled table — a `q` at zero or a
 * forgotten frequency. */
static void presets(void)
{
    for (int p = 0; p < EQ_PRESET_COUNT; p++) {
        eq_band_t b[EQ_BANDS];
        const int count = eq_preset((eq_preset_t)p, b);
        CHECK(count >= 0 && count <= EQ_BANDS, "a preset returns a valid band count");
        for (int i = 0; i < count; i++) {
            CHECK(b[i].freq >= EQ_FREQ_MIN && b[i].freq < 24000.0f,
                  "... each of whose frequencies is usable");
            CHECK(b[i].q >= EQ_Q_MIN && b[i].q <= EQ_Q_MAX,
                  "... and each quality factor");
        }
        CHECK(eq_preset_key((eq_preset_t)p) != NULL, "... and the preset has a name");
    }

    /* The handheld preset must do what it claims: cut the bass the speakers do
     * not reproduce, without touching the voice. */
    eq_band_t b[EQ_BANDS];
    const int count = eq_preset(EQ_PRESET_HANDHELD, b);
    eq_t e;
    eq_configure(&e, b, count, SR, true);
    CHECK(eq_response_db(&e, 50.0f, SR) < -12.0f,
          "handheld preset: the useless bass is cut");
    CHECK(eq_response_db(&e, 1800.0f, SR) > eq_response_db(&e, 3500.0f, SR),
          "handheld preset: presence sits above the harsh region");
}

/* =========================================================================
 * EQV2 / EQV1(a), 2026-09-11 - THE FILTER'S MEMORY, ACROSS A SESSION AND
 * ACROSS A RE-APPLY
 *
 * The checks above pin the coefficients. These pin the STATE (z1/z2), which is
 * where the two defects were. The signals are synthesised with an integer hash,
 * not rand(): msvcrt and glibc do not share a rand(), and a check whose input
 * differs per platform proves less. They are the offline bench's
 * (bench_dsp_EQV1): a bass-heavy chord peaking near 22000, a 50 Hz sine at
 * 16000, white noise at +/-8000. Stereo, 10 ms buffers (the ALSA chunk and the
 * wire frame), a change on a buffer edge at 500 ms.
 * ========================================================================= */
#define CH    2
#define BUF   480u
#define TOTAL 36000u    /* 750 ms */
#define EVT   24000u    /* the change, at 500 ms */

static double hash_noise(size_t n, int ch)
{
    uint32_t x = (uint32_t)(n * 2u + (size_t)ch) * 2654435761u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
    return ((double)(x & 0xFFFFu) / 65535.0 - 0.5);
}

enum { MUSIC, BASS, NOISE, NSIG };
static const char *SIG_NAME[NSIG] = { "music", "50 Hz", "noise" };
static int16_t g_sig[NSIG][TOTAL * CH];
static int16_t g_olds[TOTAL * CH], g_news[TOTAL * CH], g_y[TOTAL * CH];

static void gen_signals(void)
{
    const double P2 = 2.0 * 3.14159265358979323846;
    for (size_t n = 0; n < TOTAL; n++)
        for (int c = 0; c < CH; c++) {
            const double t = (double)n / SR, ph = c ? 0.3 : 0.0;
            const double v[NSIG] = {
                8000 * sin(P2 * 55 * t + ph) + 5000 * sin(P2 * 110 * t + 2 * ph)
                + 3000 * sin(P2 * 440 * t + ph) + 1500 * sin(P2 * 880 * t)
                + 2000 * sin(P2 * 1800 * t + ph) + 1200 * sin(P2 * 3500 * t)
                + 600 * sin(P2 * 7000 * t + ph) + 1600 * hash_noise(n, c),
                16000 * sin(P2 * 50 * t + ph),
                16000 * hash_noise(n, c) };
            for (int s = 0; s < NSIG; s++) {
                double x = v[s];
                if (x > 32767) x = 32767;
                if (x < -32768) x = -32768;
                g_sig[s][n * CH + (size_t)c] = (int16_t)x;
            }
        }
}

/* Settings as the application holds them: a preset's five bands and the trim
 * switch (on by default, settings.hpp). */
typedef struct { eq_band_t b[EQ_BANDS]; int n; bool trim; } cfg_t;

static cfg_t preset_cfg(eq_preset_t p)
{
    cfg_t c;
    memset(&c, 0, sizeof c);
    c.n = eq_preset(p, c.b);
    c.trim = true;
    if (p != EQ_PRESET_FLAT) c.n = EQ_BANDS;
    return c;
}

static void conf(eq_t *e, const cfg_t *c)
{
    eq_configure(e, c->n ? c->b : NULL, c->n, SR, c->trim);
}

/* `frames` must be a multiple of BUF. */
static void run_eq(eq_t *e, int16_t *pcm, size_t frames)
{
    for (size_t n = 0; n < frames; n += BUF) eq_process(e, pcm + n * CH, BUF, CH);
}

static int peak_diff(const int16_t *a, const int16_t *b, size_t n)
{
    int pk = 0;
    for (size_t i = 0; i < n; i++) {
        const int d = abs((int)a[i] - (int)b[i]);
        if (d > pk) pk = d;
    }
    return pk;
}

/* EQV2 - COUNTER-CASE: the memory a session leaves behind. The filter lives in
 * a process global (media/audio.c) and eq_reset had no caller, so a session
 * that ended during sound handed its memory to the next one, which played it
 * out as a thump - on the real audio.c, HANDHELD, median -24 dBFS and worst
 * -8.1 dBFS over 13 ms. Here session A ends mid-chord and session B is 100 ms
 * of digital silence: without the reset B is not silent, with it B is exactly
 * zero, and a reset filter IS a clean start. This pins eq_reset; the call
 * site was proven on the real audio.c by the offline bench (bench_dsp_EQV2). */
static void session_reset(void)
{
    const cfg_t H = preset_cfg(EQ_PRESET_HANDHELD);
    eq_t a;
    conf(&a, &H);
    memcpy(g_y, g_sig[MUSIC], sizeof g_y);
    run_eq(&a, g_y, EVT);                     /* session A, cut mid-sound */

    static int16_t tail[4800 * CH], quiet[4800 * CH];
    memset(tail, 0, sizeof tail);
    memset(quiet, 0, sizeof quiet);
    eq_t stale = a;                           /* what session B inherited */
    eq_t reset = a;
    eq_reset(&reset);
    run_eq(&stale, tail, 4800);
    run_eq(&reset, quiet, 4800);
    int peak = 0, nonzero = 0;
    for (size_t i = 0; i < 4800u * CH; i++) {
        if (abs(tail[i]) > peak) peak = abs(tail[i]);
        if (quiet[i] != 0) nonzero++;
    }
    printf("     (EQV2) 100 ms of silence after a session cut mid-chord: %d LSB "
           "without the reset, %d non-zero sample(s) with it\n", peak, nonzero);
    CHECK(peak > 256,
          "COUNTER-CASE (EQV2): without a reset, the previous session's filter "
          "memory plays out in the next one");
    CHECK(nonzero == 0, "(EQV2) after eq_reset a silent session stays silent, every sample 0");

    /* A reset filter is a clean start on real sound too, bit for bit. */
    eq_t fresh;
    conf(&fresh, &H);
    memcpy(g_olds, g_sig[MUSIC], sizeof g_olds);
    memcpy(g_news, g_sig[MUSIC], sizeof g_news);
    run_eq(&reset, g_olds, TOTAL);
    run_eq(&fresh, g_news, TOTAL);
    CHECK(memcmp(g_olds, g_news, sizeof g_olds) == 0,
          "(EQV2) a reset filter is a clean start: bit-identical to a freshly configured one");
}

/* EQV1(a) - eq_same() says "the same filter" exactly when installing the new
 * one would change nothing but the state. */
static void same_filter(void)
{
    const cfg_t H = preset_cfg(EQ_PRESET_HANDHELD);
    eq_t a, b;
    conf(&a, &H);
    conf(&b, &H);
    CHECK(eq_same(&a, &b), "(EQV1a) the same settings give the same filter");

    memcpy(g_y, g_sig[MUSIC], sizeof g_y);
    run_eq(&a, g_y, BUF * 10);
    CHECK(eq_same(&a, &b), "(EQV1a) ... whatever its state: the memory is not a setting");

    cfg_t t = H;
    t.trim = false;
    conf(&b, &t);
    CHECK(!eq_same(&a, &b), "(EQV1a) the auto-trim toggle is a change (the trim differs)");

    cfg_t g = H;
    g.b[2].gain_db = 4.5f;
    conf(&b, &g);
    CHECK(!eq_same(&a, &b), "(EQV1a) one notch on one band is a change");

    const cfg_t F = preset_cfg(EQ_PRESET_FLAT);
    eq_t f1, f2;
    conf(&f1, &F);
    eq_band_t z = { EQ_PEAK, 1000.0f, 1.0f, 0.0f };
    eq_configure(&f2, &z, 1, SR, true);
    CHECK(eq_same(&f1, &f2),
          "(EQV1a) a peak at 0 dB is switched off: the same neutral filter as no band at all");
    CHECK(!eq_same(&f1, &a), "(EQV1a) neutral and handheld differ");

    eq_t zero;
    memset(&zero, 0, sizeof zero);
    CHECK(!eq_same(&zero, &f1),
          "(EQV1a) a zeroed eq_t is not the neutral filter: the first configuration is never skipped");
    CHECK(!eq_same(NULL, &f1) && !eq_same(&f1, NULL), "(EQV1a) a null filter is never the same");
}

/* EQV1(a) - COUNTER-CASE: re-applying IDENTICAL settings, which every
 * pause-menu row that goes through applyToggles does. The old swap installed
 * the new filter zeroed; audio_eq_configure now keeps the running one when
 * eq_same() says nothing changed. Both are replayed here, at the application's
 * own decision point. */
static void reapply_identical(void)
{
    const cfg_t H = preset_cfg(EQ_PRESET_HANDHELD);
    for (int s = 0; s < NSIG; s++) {
        int pk[2] = { 0, 0 };
        for (int keep = 0; keep < 2; keep++) {
            memcpy(g_olds, g_sig[s], sizeof g_olds);
            memcpy(g_y, g_sig[s], sizeof g_y);
            eq_t ref, live;
            conf(&ref, &H);
            conf(&live, &H);
            for (size_t n = 0; n < TOTAL; n += BUF) {
                eq_process(&ref, g_olds + n * CH, BUF, CH);
                if (n == EVT) {
                    eq_t fresh;
                    conf(&fresh, &H);
                    if (!keep || !eq_same(&fresh, &live)) live = fresh;
                }
                eq_process(&live, g_y + n * CH, BUF, CH);
            }
            pk[keep] = peak_diff(g_y, g_olds, TOTAL * CH);
        }
        printf("     (EQV1a) identical settings re-applied, %s: %d LSB with the old "
               "zeroing swap, %d LSB now\n", SIG_NAME[s], pk[0], pk[1]);
        CHECK(pk[1] == 0,
              "(EQV1a) re-applying identical settings leaves the output BIT-IDENTICAL");
        CHECK(pk[0] > 256,
              "COUNTER-CASE (EQV1a): re-installing the same filter with its state zeroed clicks");
    }
}

/* =========================================================================
 * EQV1(b), 2026-09-11 - A GENUINE CHANGE: WARM UP 20 ms, THEN CROSSFADE 10 ms
 *
 * The checks the offline bench wrote for it (bench_dsp_EQV1, checks (a)-(e)),
 * run on the real chain, then its own mechanics. Metric: the peak deviation
 * from an IDEAL 10 ms crossfade between the old and the new steady-state
 * outputs, placed where the policy switches; for the memory alone, the
 * deviation from the new steady state.
 * ========================================================================= */

/* The pause-menu notches: the tables and the stepping rule of
 * activity/stream_activity.cpp (streamMenuEqStep), wrap-around included. */
static const float NOTCH_FREQS[] = { 30, 40, 60, 80, 100, 120, 160, 200, 250, 315, 400,
                                     500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150,
                                     4000, 5000, 6300, 8000, 10000, 12500, 16000 };
static const float NOTCH_QS[]    = { 0.3f, 0.5f, 0.707f, 1.0f, 1.4f, 2.0f, 3.0f, 4.0f, 6.0f };
static const float NOTCH_GAINS[] = { -12, -9, -6, -4.5f, -3, -1.5f, 0, 1.5f, 3, 4.5f, 6, 9, 12 };

static void notch(eq_band_t *b, int band, int param, int dir)
{
    const int step = dir >= 0 ? 1 : -1;
    if (param == 0) {
        int t = (int)b[band].type + step;
        if (t < 0) t = EQ_TYPE_COUNT - 1;
        if (t >= EQ_TYPE_COUNT) t = 0;
        b[band].type = (eq_type_t)t;
        return;
    }
    const float *tab;
    int n;
    float *f;
    if (param == 1) {
        tab = NOTCH_FREQS; n = (int)(sizeof NOTCH_FREQS / sizeof NOTCH_FREQS[0]); f = &b[band].freq;
    } else if (param == 2) {
        tab = NOTCH_QS;    n = (int)(sizeof NOTCH_QS / sizeof NOTCH_QS[0]);       f = &b[band].q;
    } else {
        tab = NOTCH_GAINS; n = (int)(sizeof NOTCH_GAINS / sizeof NOTCH_GAINS[0]); f = &b[band].gain_db;
    }
    int i = 0;
    float e = 1e9f;
    for (int k = 0; k < n; k++) {
        const float d = fabsf(tab[k] - *f);
        if (d < e) { e = d; i = k; }
    }
    *f = tab[(i + (step > 0 ? 1 : n - 1)) % n];
}

/* POL_ZERO is the swap before 2026-09-11; POL_RAW_CARRY the finding's first
 * proposal, refuted - kept to pin WHY; POL_CHAIN_OFF the chain with
 * SHADOW_EQ_CROSSFADE=0. */
enum { POL_ZERO, POL_RAW_CARRY, POL_CHAIN, POL_CHAIN_OFF };
static eq_chain_t g_chain;
/* The steady states of the case in hand, cached: the sweep asks for the same
 * case under two policies. */
static int16_t g_ss_old[TOTAL * CH], g_ss_new[TOTAL * CH];
static const int16_t *g_ss_x;
static cfg_t g_ss_oc, g_ss_nc;

static int transient(const int16_t *x, const cfg_t *oc, const cfg_t *nc, int pol,
                     int *dss_out, int *post_out)
{
    if (x != g_ss_x || memcmp(&g_ss_oc, oc, sizeof *oc) != 0
                    || memcmp(&g_ss_nc, nc, sizeof *nc) != 0) {
        memcpy(g_ss_old, x, sizeof g_ss_old);
        memcpy(g_ss_new, x, sizeof g_ss_new);
        eq_t a, b;
        conf(&a, oc);
        conf(&b, nc);
        run_eq(&a, g_ss_old, TOTAL);
        run_eq(&b, g_ss_new, TOTAL);
        g_ss_x = x; g_ss_oc = *oc; g_ss_nc = *nc;
    }
    memcpy(g_y, x, sizeof g_y);
    eq_t live;
    conf(&live, oc);
    size_t S = EVT;
    const bool chain = (pol == POL_CHAIN || pol == POL_CHAIN_OFF);
    for (size_t n = 0; n < TOTAL; n += BUF) {
        if (n == EVT) {
            eq_t fresh;
            conf(&fresh, nc);
            if (pol == POL_ZERO) {
                live = fresh;
            } else if (pol == POL_RAW_CARRY) {
                for (int i = 0; i < EQ_BANDS; i++)
                    if (fresh.active[i] && live.active[i] && nc->b[i].type == oc->b[i].type) {
                        memcpy(fresh.z1[i], live.z1[i], sizeof fresh.z1[i]);
                        memcpy(fresh.z2[i], live.z2[i], sizeof fresh.z2[i]);
                    }
                live = fresh;
            } else {
                eq_chain_init(&g_chain, &live);
                if (eq_chain_set(&g_chain, &fresh, pol == POL_CHAIN) == EQ_CHAIN_ARMED)
                    S = EVT + EQ_WARM_FRAMES;
            }
        }
        if (chain && n >= EVT) eq_chain_process(&g_chain, g_y + n * CH, BUF, CH);
        else                   eq_process(&live, g_y + n * CH, BUF, CH);
    }
    int pk = 0, post = 0, dss = 0;
    for (size_t n = EVT; n < TOTAL; n++)
        for (int c = 0; c < CH; c++) {
            const size_t i = n * CH + (size_t)c;
            double ideal;
            if (n < S) ideal = g_ss_old[i];
            else if (n >= S + EQ_FADE_FRAMES) ideal = g_ss_new[i];
            else {
                const double w = (double)(n - S + 1) / (double)EQ_FADE_FRAMES;
                ideal = (1 - w) * g_ss_old[i] + w * g_ss_new[i];
            }
            const int d = (int)fabs((double)g_y[i] - ideal);
            if (d > pk) pk = d;
            if (n >= S + EQ_FADE_FRAMES && g_y[i] != g_ss_new[i]) post++;
            if (pol != POL_CHAIN || n >= S + EQ_FADE_FRAMES) {
                const int ds = abs((int)g_y[i] - (int)g_ss_new[i]);
                if (ds > dss) dss = ds;
            }
        }
    if (dss_out) *dss_out = dss;
    if (post_out) *post_out = post;
    return pk;
}

/* (a) identical settings, (b) the auto-trim toggle. */
static void chain_reapply_and_trim(void)
{
    const cfg_t H = preset_cfg(EQ_PRESET_HANDHELD);
    for (int s = 0; s < NSIG; s++)
        CHECK(transient(g_sig[s], &H, &H, POL_CHAIN, NULL, NULL) == 0,
              "(EQV1b-a) through the chain, identical settings leave the output BIT-IDENTICAL");
    cfg_t Ho = H;
    Ho.trim = false;
    for (int s = 0; s < NSIG; s++) {
        int zpost = 0, post = 0;
        const int z = transient(g_sig[s], &H, &Ho, POL_ZERO, NULL, &zpost);
        (void)transient(g_sig[s], &H, &Ho, POL_CHAIN, NULL, &post);
        printf("     (EQV1b-b) auto-trim toggle, %s: %d sample(s) off the trim-off steady "
               "state once the change is complete (old swap: %d, peak %d LSB)\n",
               SIG_NAME[s], post, zpost, z);
        CHECK(post == 0,
              "(EQV1b-b) after an auto-trim toggle and its fade, the output IS the trim-off "
              "steady state, bit for bit");
    }
}

/* (c) COUNTER-CASE - headphones, band 0 frequency LEFT: the 30 Hz high-pass
 * wraps to 16 kHz. Carrying a 30 Hz filter's memory into a 16 kHz one is the
 * worst case of the refuted carry; the chain must stay at or below zeroing. */
static void chain_wrap(void)
{
    const cfg_t P = preset_cfg(EQ_PRESET_HEADPHONES);
    cfg_t Pl = P;
    notch(Pl.b, 0, 1, -1);
    CHECK(Pl.b[0].freq == 16000.0f, "(EQV1b-c) the pause-menu notch LEFT from 30 Hz wraps to 16 kHz");
    for (int s = 0; s < 2; s++) {
        const int16_t *x = s == 0 ? g_sig[BASS] : g_sig[MUSIC];
        int zs = 0, cs = 0, ks = 0;
        const int z = transient(x, &P, &Pl, POL_ZERO, &zs, NULL);
        const int c = transient(x, &P, &Pl, POL_RAW_CARRY, &cs, NULL);
        const int k = transient(x, &P, &Pl, POL_CHAIN, &ks, NULL);
        printf("     (EQV1b-c) %s - vs the new steady state: zeroed %d, carried %d, chain %d "
               "| vs an ideal crossfade: zeroed %d, carried %d, chain %d LSB\n",
               s == 0 ? "50 Hz" : "music", zs, cs, ks, z, c, k);
        if (s == 0)
            CHECK(cs > 4 * zs,
                  "COUNTER-CASE (EQV1b-c): carrying the old state into a changed band is far "
                  "WORSE than zeroing it - why the chain never does");
        CHECK(ks <= zs + 16, "(EQV1b-c) the chain's memory transient stays at or below zeroing + 16 LSB");
        CHECK(k <= z + 16, "(EQV1b-c) ... and so does its deviation from an ideal crossfade");
    }
}

/* (d) every pause-menu notch of the four non-neutral presets, and every
 * profile switch (dock/undock included), on the three signals. */
static void chain_sweep(void)
{
    const eq_preset_t BASES[] = { EQ_PRESET_HANDHELD, EQ_PRESET_HEADPHONES,
                                  EQ_PRESET_VOICE, EQ_PRESET_NIGHT };
    int worse = 0, notch_k = 0, notch_z = 0, prof_k = 0, prof_z = 0, n_notch = 0, n_prof = 0;
    for (unsigned bi = 0; bi < sizeof BASES / sizeof BASES[0]; bi++)
    for (int band = 0; band < EQ_BANDS; band++)
    for (int param = 0; param < 4; param++)
    for (int dir = -1; dir <= 1; dir += 2)
    for (int s = 0; s < NSIG; s++) {
        const cfg_t o = preset_cfg(BASES[bi]);
        cfg_t m = o;
        notch(m.b, band, param, dir);
        const int z = transient(g_sig[s], &o, &m, POL_ZERO, NULL, NULL);
        const int k = transient(g_sig[s], &o, &m, POL_CHAIN, NULL, NULL);
        if (k > z + 16) worse++;
        if (k > notch_k) notch_k = k;
        if (z > notch_z) notch_z = z;
        n_notch++;
    }
    for (int a = 0; a < EQ_PRESET_CUSTOM; a++)
    for (int b = 0; b < EQ_PRESET_CUSTOM; b++) {
        if (a == b) continue;
        for (int s = 0; s < NSIG; s++) {
            const cfg_t o = preset_cfg((eq_preset_t)a), m = preset_cfg((eq_preset_t)b);
            const int z = transient(g_sig[s], &o, &m, POL_ZERO, NULL, NULL);
            const int k = transient(g_sig[s], &o, &m, POL_CHAIN, NULL, NULL);
            if (k > z + 16) worse++;
            if (k > prof_k) prof_k = k;
            if (z > prof_z) prof_z = z;
            n_prof++;
        }
    }
    printf("     (EQV1b-d) %d notches + %d profile switches: worst notch %d LSB (zeroed %d), "
           "worst profile switch %d (zeroed %d), %d case(s) worse than zeroing by more than 16\n",
           n_notch, n_prof, notch_k, notch_z, prof_k, prof_z, worse);
    CHECK(worse == 0, "(EQV1b-d) no notch and no profile switch is worse than zeroing by more than 16 LSB");
    CHECK(notch_k < 1024, "(EQV1b-d) worst pause-menu notch under 1024 LSB");
    CHECK(prof_k < 512, "(EQV1b-d) worst profile switch under 512 LSB");
}

/* (e) the finding's own notch; (f) the revert toggle really reverts. */
static void chain_notch_and_revert(void)
{
    const cfg_t H = preset_cfg(EQ_PRESET_HANDHELD);
    cfg_t Hn = H;
    Hn.b[2].gain_db = 4.5f;
    static int16_t zeroed[TOTAL * CH];
    const int z = transient(g_sig[MUSIC], &H, &Hn, POL_ZERO, NULL, NULL);
    memcpy(zeroed, g_y, sizeof zeroed);
    const int k = transient(g_sig[MUSIC], &H, &Hn, POL_CHAIN, NULL, NULL);
    const int o = transient(g_sig[MUSIC], &H, &Hn, POL_CHAIN_OFF, NULL, NULL);
    printf("     (EQV1b-e) band 2 +3 -> +4.5 dB on music: old swap %d LSB, chain %d, "
           "chain with the crossfade off %d\n", z, k, o);
    CHECK(k < 256, "(EQV1b-e) the finding's notch stays under 256 LSB");
    CHECK(memcmp(zeroed, g_y, sizeof zeroed) == 0,
          "(EQV1b-f) SHADOW_EQ_CROSSFADE=0 restores the old swap EXACTLY for a changed setting");
    CHECK(o > 1024, "COUNTER-CASE (EQV1b-f): ... click included - the revert really reverts");
    CHECK(transient(g_sig[MUSIC], &H, &H, POL_CHAIN_OFF, NULL, NULL) == 0,
          "(EQV1b-f) ... while identical settings stay a no-op with the crossfade off (EQV1a is untoggled)");
}

/* The chain's own mechanics: cancel, replace, queue, promote - and the EQV2
 * session reset once a change can be in flight. */
static void chain_timing(void)
{
    const cfg_t H = preset_cfg(EQ_PRESET_HANDHELD), V = preset_cfg(EQ_PRESET_VOICE),
                F = preset_cfg(EQ_PRESET_FLAT);
    eq_t h, v, f;
    conf(&h, &H);
    conf(&v, &V);
    conf(&f, &F);

    eq_chain_init(&g_chain, NULL);
    CHECK(!eq_chain_armed(&g_chain) && eq_same(eq_chain_target(&g_chain), &f),
          "(EQV1b) a new chain is the neutral filter and costs nothing");

    /* Undo inside the warm-up - inaudible - cancels: nothing was heard. */
    memcpy(g_olds, g_sig[MUSIC], sizeof g_olds);
    memcpy(g_y, g_sig[MUSIC], sizeof g_y);
    eq_t ref = h;
    eq_chain_init(&g_chain, &h);
    int r1 = -1, r2 = -1;
    for (size_t n = 0; n < TOTAL; n += BUF) {
        if (n == EVT)       r1 = eq_chain_set(&g_chain, &v, true);
        if (n == EVT + BUF) r2 = eq_chain_set(&g_chain, &h, true);
        eq_process(&ref, g_olds + n * CH, BUF, CH);
        eq_chain_process(&g_chain, g_y + n * CH, BUF, CH);
    }
    CHECK(r1 == EQ_CHAIN_ARMED && r2 == EQ_CHAIN_CANCELLED,
          "(EQV1b) a change then its undo inside the warm-up: armed, then cancelled");
    CHECK(memcmp(g_y, g_olds, sizeof g_y) == 0,
          "(EQV1b) ... and the output is BIT-IDENTICAL to never having changed");

    /* A newer change during the warm-up replaces the target. */
    eq_chain_init(&g_chain, &h);
    memcpy(g_y, g_sig[MUSIC], sizeof g_y);
    (void)eq_chain_set(&g_chain, &v, true);
    eq_chain_process(&g_chain, g_y, BUF, CH);
    r1 = eq_chain_set(&g_chain, &f, true);
    CHECK(r1 == EQ_CHAIN_ARMED && g_chain.warm_left == EQ_WARM_FRAMES && eq_same(&g_chain.pend, &f),
          "(EQV1b) a newer change during the warm-up replaces the target and warms it up afresh");

    /* During a FADE - audible - a newer change is queued behind it. */
    eq_chain_init(&g_chain, &h);
    memcpy(g_y, g_sig[MUSIC], sizeof g_y);
    int16_t *p = g_y;
    (void)eq_chain_set(&g_chain, &v, true);
    eq_chain_process(&g_chain, p, EQ_WARM_FRAMES + 240, CH);
    p += (EQ_WARM_FRAMES + 240) * CH;
    const bool mid_fade = g_chain.pending && g_chain.warm_left == 0 && g_chain.fade_pos == 240;
    r1 = eq_chain_set(&g_chain, &f, true);
    CHECK(mid_fade && r1 == EQ_CHAIN_QUEUED && eq_same(eq_chain_target(&g_chain), &f),
          "(EQV1b) a change in the middle of a fade is queued: the fade is never cut");
    eq_chain_process(&g_chain, p, 240, CH);
    p += 240 * CH;
    CHECK(eq_same(&g_chain.live, &v) && g_chain.pending && g_chain.warm_left == EQ_WARM_FRAMES
          && eq_same(&g_chain.pend, &f),
          "(EQV1b) ... the fade completes to its own target, then the queued one warms up");
    eq_chain_process(&g_chain, p, EQ_WARM_FRAMES, CH);
    p += EQ_WARM_FRAMES * CH;
    const bool armed_mid = eq_chain_armed(&g_chain);
    eq_chain_process(&g_chain, p, EQ_FADE_FRAMES, CH);
    CHECK(armed_mid && !g_chain.pending && eq_same(&g_chain.live, &f) && !eq_chain_armed(&g_chain),
          "(EQV1b) a fade to neutral runs until it ends, then the chain costs nothing again");

    /* EQV2 with the chain: the session reset PROMOTES the change in flight. */
    eq_chain_init(&g_chain, &h);
    memcpy(g_y, g_sig[MUSIC], sizeof g_y);
    eq_chain_process(&g_chain, g_y, EVT, CH);                 /* a session of sound */
    (void)eq_chain_set(&g_chain, &v, true);
    eq_chain_process(&g_chain, g_y + EVT * CH, 600, CH);      /* still warming */
    eq_chain_reset(&g_chain);
    CHECK(!g_chain.pending && !g_chain.queued && eq_same(&g_chain.live, &v),
          "(EQV2) the session reset promotes the change in flight: the last setting asked for");
    static int16_t quiet[4800 * CH];
    memset(quiet, 0, sizeof quiet);
    eq_chain_process(&g_chain, quiet, 4800, CH);
    int nz = 0;
    for (size_t i = 0; i < 4800u * CH; i++) if (quiet[i] != 0) nz++;
    CHECK(nz == 0, "(EQV2) ... from rest: a silent session stays silent");

    eq_chain_init(&g_chain, &h);
    memcpy(g_y, g_sig[MUSIC], sizeof g_y);
    (void)eq_chain_set(&g_chain, &v, true);
    eq_chain_process(&g_chain, g_y, EQ_WARM_FRAMES + 100, CH);
    (void)eq_chain_set(&g_chain, &f, true);                   /* queued behind the fade */
    eq_chain_reset(&g_chain);
    CHECK(eq_same(&g_chain.live, &f) && !eq_chain_armed(&g_chain),
          "(EQV2) ... and with two changes in flight, the LAST one wins");
}

/* The chain works through a buffer in chunks of its 480-frame scratch, so the
 * buffer size must not change a single sample. */
static void chain_buffers(void)
{
    const cfg_t H = preset_cfg(EQ_PRESET_HANDHELD), N = preset_cfg(EQ_PRESET_NIGHT);
    static const size_t SIZES[] = { 96, 480, 1200, 6000 };
    static int16_t ref[TOTAL * CH];
    int same = 1;
    for (unsigned k = 0; k < sizeof SIZES / sizeof SIZES[0]; k++) {
        eq_t live;
        conf(&live, &H);
        eq_chain_init(&g_chain, &live);
        memcpy(g_y, g_sig[MUSIC], sizeof g_y);
        for (size_t n = 0; n < TOTAL; n += SIZES[k]) {
            if (n == EVT) {
                eq_t fresh;
                conf(&fresh, &N);
                (void)eq_chain_set(&g_chain, &fresh, true);
            }
            eq_chain_process(&g_chain, g_y + n * CH, SIZES[k], CH);
        }
        if (k == 0) memcpy(ref, g_y, sizeof ref);
        else if (memcmp(ref, g_y, sizeof ref) != 0) same = 0;
    }
    CHECK(same, "(EQV1b) the output does not depend on the buffer size - 96, 480, 1200 or "
                "6000 frames, bit for bit, longer than the 480-frame scratch included "
                "(audout hands up to 5760)");

    /* In steady state the chain IS eq_process - here in mono. */
    eq_t a, b;
    conf(&a, &H);
    conf(&b, &H);
    eq_chain_init(&g_chain, &a);
    memcpy(g_olds, g_sig[MUSIC], sizeof g_olds);
    memcpy(g_news, g_sig[MUSIC], sizeof g_news);
    eq_process(&b, g_olds, TOTAL * CH, 1);
    eq_chain_process(&g_chain, g_news, TOTAL * CH, 1);
    CHECK(memcmp(g_olds, g_news, sizeof g_olds) == 0,
          "(EQV1b) in steady state the chain IS eq_process, bit for bit");
}

int main(void)
{
    printf("== equaliser (biquads, response MEASURED rather than re-read) ==\n");
    bypass();
    exact_gain();
    selectivity();
    cutoffs();
    shelves();
    independent_channels();
    stability();
    trim();
    presets();
    gen_signals();
    session_reset();
    same_filter();
    reapply_identical();
    chain_reapply_and_trim();
    chain_wrap();
    chain_sweep();
    chain_notch_and_revert();
    chain_timing();
    chain_buffers();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
