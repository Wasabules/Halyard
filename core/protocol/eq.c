/* eq — see eq.h for why biquads, and why this module is pure. */
#include "eq.h"

#include <math.h>
#include <string.h>

/* Denormal guard. A denormalised float — smaller than ~1e-38 — takes a slow
 * path on many processors, sometimes a hundred times slower, and that is
 * exactly what an IIR filter's state produces during silence: it decays
 * geometrically towards zero without ever reaching it. The symptom would be CPU
 * use EXPLODING the moment the sound goes quiet, which is the last place anyone
 * would think to look.
 *
 * So we force the state to zero below an inaudible floor. 1e-15 is thirty
 * orders of magnitude below one 16-bit sample: it changes nothing about the
 * sound and closes the problem. */
#define EQ_FLOOR 1e-15f

static float clampf(float v, float lo, float hi)
{
    if (!(v == v)) return lo;         /* NaN: it fails both comparisons */
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Coefficients for one band. Formulas from the "Audio EQ Cookbook" (RBJ).
 *
 * They are copied verbatim, and that is deliberate: rewriting them "more
 * readably" is the surest way to slip in a mistake that will not show. The
 * verification lives elsewhere — in tests/test_eq.c, which measures the real
 * response rather than re-reading the formulas. */
static bool coeffs(const eq_band_t *b, uint32_t sample_rate,
                   float *b0, float *b1, float *b2, float *a1, float *a2)
{
    if (b->type == EQ_OFF) return false;

    const float nyquist = (float)sample_rate * 0.5f;
    /* We stop 100 Hz below Nyquist: at the limit `tan(w0/2)` goes to infinity
     * and the coefficients become nonsense. This is not a theoretical
     * precaution — a settings file containing 24000 on a 48 kHz output would
     * produce exactly that case. */
    const float f   = clampf(b->freq, EQ_FREQ_MIN, nyquist - 100.0f);
    const float q   = clampf(b->q, EQ_Q_MIN, EQ_Q_MAX);
    const float gdb = clampf(b->gain_db, EQ_GAIN_MIN, EQ_GAIN_MAX);

    /* A peak or a shelf at 0 dB does nothing: we switch it off rather than run
     * five multiplications per sample through it. */
    if ((b->type == EQ_PEAK || b->type == EQ_LOWSHELF
         || b->type == EQ_HIGHSHELF) && fabsf(gdb) < 0.05f)
        return false;

    const float w0    = 2.0f * 3.14159265358979f * f / (float)sample_rate;
    const float cosw0 = cosf(w0);
    const float sinw0 = sinf(w0);
    const float alpha = sinw0 / (2.0f * q);
    /* `A` is the SQUARE ROOT of the linear gain for peaks and shelves: that is
     * the cookbook's convention, and forgetting it gives a filter that corrects
     * twice too much (in dB). It is the classic mistake, and the test catches
     * it. */
    const float A = powf(10.0f, gdb / 40.0f);

    float B0, B1, B2, A0, A1, A2;

    switch (b->type) {
    case EQ_HIGHPASS:
        B0 =  (1.0f + cosw0) * 0.5f;
        B1 = -(1.0f + cosw0);
        B2 =  (1.0f + cosw0) * 0.5f;
        A0 =   1.0f + alpha;
        A1 =  -2.0f * cosw0;
        A2 =   1.0f - alpha;
        break;

    case EQ_LOWPASS:
        B0 =  (1.0f - cosw0) * 0.5f;
        B1 =   1.0f - cosw0;
        B2 =  (1.0f - cosw0) * 0.5f;
        A0 =   1.0f + alpha;
        A1 =  -2.0f * cosw0;
        A2 =   1.0f - alpha;
        break;

    case EQ_PEAK:
        B0 = 1.0f + alpha * A;
        B1 = -2.0f * cosw0;
        B2 = 1.0f - alpha * A;
        A0 = 1.0f + alpha / A;
        A1 = -2.0f * cosw0;
        A2 = 1.0f - alpha / A;
        break;

    case EQ_LOWSHELF: {
        const float s = 2.0f * sqrtf(A) * alpha;
        B0 =        A * ((A + 1.0f) - (A - 1.0f) * cosw0 + s);
        B1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
        B2 =        A * ((A + 1.0f) - (A - 1.0f) * cosw0 - s);
        A0 =             (A + 1.0f) + (A - 1.0f) * cosw0 + s;
        A1 =    -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
        A2 =             (A + 1.0f) + (A - 1.0f) * cosw0 - s;
        break;
    }

    case EQ_HIGHSHELF: {
        const float s = 2.0f * sqrtf(A) * alpha;
        B0 =        A * ((A + 1.0f) + (A - 1.0f) * cosw0 + s);
        B1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
        B2 =        A * ((A + 1.0f) + (A - 1.0f) * cosw0 - s);
        A0 =             (A + 1.0f) - (A - 1.0f) * cosw0 + s;
        A1 =     2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
        A2 =             (A + 1.0f) - (A - 1.0f) * cosw0 - s;
        break;
    }

    default:
        return false;
    }

    if (!(A0 > 1e-12f) || !(A0 == A0)) return false;   /* division impossible */

    *b0 = B0 / A0;  *b1 = B1 / A0;  *b2 = B2 / A0;
    *a1 = A1 / A0;  *a2 = A2 / A0;

    /* STABILITY guard. A biquad is stable when its poles lie inside the unit
     * circle, which reads `|a2| < 1` and `|a1| < 1 + a2`. An unstable band does
     * not merely correct badly: it makes the signal DIVERGE, and the sound
     * saturates within milliseconds. The cookbook does not produce that on
     * bounded inputs, but the guard costs nothing and turns a future mistake
     * into a silent band rather than an accident. */
    if (!(fabsf(*a2) < 0.9999f) || !(fabsf(*a1) < 1.0f + *a2)) return false;

    return true;
}

/* Magnitude of ONE band's response at angular frequency `w`. */
static float band_magnitude(float b0, float b1, float b2, float a1, float a2, float w)
{
    const float c1 = cosf(w),  s1 = sinf(w);
    const float c2 = cosf(2.0f * w), s2 = sinf(2.0f * w);
    const float nr = b0 + b1 * c1 + b2 * c2;
    const float ni =     -b1 * s1 - b2 * s2;
    const float dr = 1.0f + a1 * c1 + a2 * c2;
    const float di =      -a1 * s1 - a2 * s2;
    const float dm = dr * dr + di * di;
    if (dm < 1e-20f) return 1.0f;
    return sqrtf((nr * nr + ni * ni) / dm);
}

void eq_reset(eq_t *e)
{
    if (!e) return;
    memset(e->z1, 0, sizeof e->z1);
    memset(e->z2, 0, sizeof e->z2);
}

/* One band's five coefficients, compared as BITS. `==` on floats would call a
 * NaN different from itself, and a guard that answers "different" for
 * identical settings brings the click back. The question is "did eq_configure
 * produce the same thing", and it produces the same bits from the same input. */
static bool band_coeffs_same(const eq_t *a, const eq_t *b, int i)
{
    return memcmp(&a->b0[i], &b->b0[i], sizeof(float)) == 0
        && memcmp(&a->b1[i], &b->b1[i], sizeof(float)) == 0
        && memcmp(&a->b2[i], &b->b2[i], sizeof(float)) == 0
        && memcmp(&a->a1[i], &b->a1[i], sizeof(float)) == 0
        && memcmp(&a->a2[i], &b->a2[i], sizeof(float)) == 0;
}

bool eq_same(const eq_t *a, const eq_t *b)
{
    if (!a || !b) return false;
    if (a->bypass != b->bypass) return false;
    if (memcmp(&a->trim, &b->trim, sizeof a->trim) != 0) return false;
    for (int i = 0; i < EQ_BANDS; i++) {
        if (a->active[i] != b->active[i]) return false;
        if (a->active[i] && !band_coeffs_same(a, b, i)) return false;
    }
    return true;
}

void eq_configure(eq_t *e, const eq_band_t *bands, int count, uint32_t sample_rate,
                  bool auto_trim)
{
    if (!e) return;
    memset(e, 0, sizeof *e);
    e->trim = 1.0f;
    e->bypass = true;
    if (!bands || count <= 0 || sample_rate < 8000u) return;
    if (count > EQ_BANDS) count = EQ_BANDS;

    for (int i = 0; i < count; i++) {
        if (coeffs(&bands[i], sample_rate,
                   &e->b0[i], &e->b1[i], &e->b2[i], &e->a1[i], &e->a2[i])) {
            e->active[i] = true;
            e->bypass = false;
        }
    }
    if (e->bypass) return;

    if (!auto_trim) return;

    /* === THE TRIM, AND WHY IT IS MEASURED RATHER THAN DEDUCED ===
     *
     * A band bumping by +6 dB clips a signal already near the ceiling, and an
     * `int16_t` saturating is not gentle compression: it is hard clipping, which
     * is heard as a crackle.
     *
     * The obvious answer is "attenuate by the sum of the positive gains". It is
     * WRONG as soon as two bands overlap: their gains do not add exactly, and
     * you attenuate too much — the equaliser then makes the sound quieter for no
     * visible reason.
     *
     * So we evaluate the cascade's REAL response over a logarithmic grid and
     * take its maximum. 192 points from 20 Hz to Nyquist: it is done ONCE, at
     * configuration time, and the resolution is ample for a filter whose
     * narrowest band is a tenth of an octave. */
    float peak = 1.0f;
    const float f_lo = 20.0f, f_hi = (float)sample_rate * 0.5f - 100.0f;
    for (int k = 0; k < 192; k++) {
        const float f = f_lo * powf(f_hi / f_lo, (float)k / 191.0f);
        const float w = 2.0f * 3.14159265358979f * f / (float)sample_rate;
        float m = 1.0f;
        for (int i = 0; i < EQ_BANDS; i++)
            if (e->active[i])
                m *= band_magnitude(e->b0[i], e->b1[i], e->b2[i],
                                    e->a1[i], e->a2[i], w);
        if (m > peak) peak = m;
    }
    e->trim = 1.0f / peak;
}

float eq_response_db(const eq_t *e, float freq, uint32_t sample_rate)
{
    if (!e || sample_rate == 0) return 0.0f;
    const float w = 2.0f * 3.14159265358979f * freq / (float)sample_rate;
    float m = e->trim > 0.0f ? e->trim : 1.0f;
    for (int i = 0; i < EQ_BANDS; i++)
        if (e->active[i])
            m *= band_magnitude(e->b0[i], e->b1[i], e->b2[i], e->a1[i], e->a2[i], w);
    return 20.0f * log10f(m > 1e-9f ? m : 1e-9f);
}

void eq_process(eq_t *e, int16_t *pcm, size_t frames, int channels)
{
    if (!e || !pcm || e->bypass || frames == 0) return;
    if (channels < 1) channels = 1;
    if (channels > 2) channels = 2;

    const float trim = (e->trim > 0.0f && e->trim <= 1.0f) ? e->trim : 1.0f;

    for (size_t t = 0; t < frames; t++) {
        for (int c = 0; c < channels; c++) {
            float x = (float)pcm[t * (size_t)channels + c];

            for (int i = 0; i < EQ_BANDS; i++) {
                if (!e->active[i]) continue;
                /* Transposed direct form II. Preferred over direct form I for a
                 * reason that matters in floating point: its state holds values
                 * on the order of the signal, not of their intermediate sums,
                 * which limits the precision lost on a high-Q filter. */
                const float y = e->b0[i] * x + e->z1[i][c];
                e->z1[i][c] = e->b1[i] * x - e->a1[i] * y + e->z2[i][c];
                e->z2[i][c] = e->b2[i] * x - e->a2[i] * y;

                /* See EQ_FLOOR: without this the state decays into denormals
                 * during silence and the filter's cost is multiplied — at the
                 * exact moment when there is nothing to filter. */
                if (fabsf(e->z1[i][c]) < EQ_FLOOR) e->z1[i][c] = 0.0f;
                if (fabsf(e->z2[i][c]) < EQ_FLOOR) e->z2[i][c] = 0.0f;

                x = y;
            }

            x *= trim;
            /* FINAL clamp, always: the trim makes clipping very unlikely, it
             * does not make it impossible — an unstable band that escaped the
             * guard, or a signal already clipped at the input, would suffice.
             * And an `int16_t` that overflows does not become loud, it WRAPS:
             * the sound goes from too loud to crackling. */
            if (x >  32767.0f) x =  32767.0f;
            if (x < -32768.0f) x = -32768.0f;
            pcm[t * (size_t)channels + c] = (int16_t)x;
        }
    }
}

/* --- The chain (EQV1(b), see eq.h) ------------------------------------ */

/* EXACT carry: a band keeps the old state only when its own five coefficients
 * are identical. Never across a changed band - that is the carry eq.h shows to
 * be worse than zeroing. */
static void carry_unchanged_bands(eq_t *fresh, const eq_t *live)
{
    for (int i = 0; i < EQ_BANDS; i++) {
        if (!fresh->active[i] || !live->active[i] || !band_coeffs_same(fresh, live, i))
            continue;
        memcpy(fresh->z1[i], live->z1[i], sizeof fresh->z1[i]);
        memcpy(fresh->z2[i], live->z2[i], sizeof fresh->z2[i]);
    }
}

static void chain_arm(eq_chain_t *c, const eq_t *fresh)
{
    c->pend = *fresh;
    eq_reset(&c->pend);
    carry_unchanged_bands(&c->pend, &c->live);
    c->pending   = true;
    c->warm_left = EQ_WARM_FRAMES;
    c->fade_pos  = 0;
}

void eq_chain_init(eq_chain_t *c, const eq_t *live)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
    if (live) { c->live = *live; return; }
    /* A zeroed eq_t is NOT neutral: bypass false, trim 0. */
    c->live.trim   = 1.0f;
    c->live.bypass = true;
}

const eq_t *eq_chain_target(const eq_chain_t *c)
{
    if (!c) return NULL;
    return c->queued ? &c->next : (c->pending ? &c->pend : &c->live);
}

bool eq_chain_armed(const eq_chain_t *c)
{
    return c && (c->pending || !c->live.bypass);
}

int eq_chain_set(eq_chain_t *c, const eq_t *fresh, bool crossfade)
{
    if (!c || !fresh) return EQ_CHAIN_SAME;
    if (eq_same(fresh, eq_chain_target(c))) return EQ_CHAIN_SAME;

    if (!crossfade) {
        c->live = *fresh;
        eq_reset(&c->live);
        c->pending = c->queued = false;
        c->warm_left = c->fade_pos = 0;
        return EQ_CHAIN_SWAPPED;
    }
    if (c->pending && c->fade_pos > 0) {
        /* A fade is AUDIBLE: cutting it would be a jump of w x (new - old).
         * The newest target waits for it; an older queued one is dropped. */
        c->next   = *fresh;
        c->queued = true;
        return EQ_CHAIN_QUEUED;
    }
    if (eq_same(fresh, &c->live)) {
        /* Back to what is audible before its replacement was heard. */
        c->pending = c->queued = false;
        return EQ_CHAIN_CANCELLED;
    }
    chain_arm(c, fresh);      /* idle, or still warming (inaudible): replace */
    c->queued = false;
    return EQ_CHAIN_ARMED;
}

/* One stretch of the linear crossfade. The weight of the new filter is
 * (pos + 1) / EQ_FADE_FRAMES, so the fade's last frame IS the new filter.
 * Rounded to nearest, then clamped - an int16_t that overflows wraps. */
static void chain_mix(int16_t *out, const int16_t *nw, size_t n, size_t ch, uint32_t pos0)
{
    for (size_t t = 0; t < n; t++) {
        float w = (float)(pos0 + (uint32_t)t + 1u) / (float)EQ_FADE_FRAMES;
        if (w > 1.0f) w = 1.0f;
        for (size_t k = 0; k < ch; k++) {
            const size_t i = t * ch + k;
            float v = (1.0f - w) * (float)out[i] + w * (float)nw[i];
            v = v >= 0.0f ? v + 0.5f : v - 0.5f;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out[i] = (int16_t)v;
        }
    }
}

void eq_chain_process(eq_chain_t *c, int16_t *pcm, size_t frames, int channels)
{
    if (!c || !pcm || frames == 0) return;
    if (channels < 1) channels = 1;       /* eq_process's own clamp - and the */
    if (channels > 2) channels = 2;       /* scratch holds two channels */
    const size_t ch = (size_t)channels;

    while (frames > 0) {
        if (!c->pending) { eq_process(&c->live, pcm, frames, channels); return; }

        const bool warming = c->warm_left > 0;
        size_t n = frames;
        const size_t lim = warming ? (size_t)c->warm_left
                                   : (size_t)(EQ_FADE_FRAMES - c->fade_pos);
        if (n > lim) n = lim;
        if (n > EQ_CHAIN_SCRATCH_FRAMES) n = EQ_CHAIN_SCRATCH_FRAMES;

        memcpy(c->scratch, pcm, n * ch * sizeof *pcm);
        eq_process(&c->live, pcm, n, channels);
        eq_process(&c->pend, c->scratch, n, channels);

        if (warming) {
            c->warm_left -= (uint32_t)n;           /* the old filter stays audible */
        } else {
            chain_mix(pcm, c->scratch, n, ch, c->fade_pos);
            c->fade_pos += (uint32_t)n;
            if (c->fade_pos >= EQ_FADE_FRAMES) {
                c->live    = c->pend;              /* promoted */
                c->pending = false;
                if (c->queued) {
                    c->queued = false;
                    if (!eq_same(&c->next, &c->live)) chain_arm(c, &c->next);
                }
            }
        }
        pcm    += n * ch;
        frames -= n;
    }
}

void eq_chain_reset(eq_chain_t *c)
{
    if (!c) return;
    if (c->queued)       c->live = c->next;
    else if (c->pending) c->live = c->pend;
    c->pending = c->queued = false;
    c->warm_left = c->fade_pos = 0;
    eq_reset(&c->live);
}

/* --- Presets ----------------------------------------------------------- */

int eq_preset(eq_preset_t p, eq_band_t *b)
{
    if (!b) return 0;
    memset(b, 0, sizeof(eq_band_t) * EQ_BANDS);

    switch (p) {
    case EQ_PRESET_FLAT:
        return 0;

    case EQ_PRESET_HANDHELD:
        /* Built-in speakers. Two problems, and they are linked: they reproduce
         * NOTHING below ~250 Hz, and the amplitude we send there anyway makes
         * their membrane saturate — so the bass you cannot hear also degrades
         * the mids you can. We cut it firmly.
         * The small dip at 3.5 kHz answers these speakers' presence bump, which
         * makes high sounds harsh at high volume. */
        b[0].type = EQ_HIGHPASS;   b[0].freq =  160.0f; b[0].q = 0.707f;
        b[1].type = EQ_PEAK;       b[1].freq =  500.0f; b[1].q = 1.0f;  b[1].gain_db = -2.0f;
        b[2].type = EQ_PEAK;       b[2].freq = 1800.0f; b[2].q = 1.2f;  b[2].gain_db = +3.0f;
        b[3].type = EQ_PEAK;       b[3].freq = 3500.0f; b[3].q = 1.5f;  b[3].gain_db = -2.5f;
        b[4].type = EQ_HIGHSHELF;  b[4].freq = 8000.0f; b[4].q = 0.707f; b[4].gain_db = +1.5f;
        return 5;

    case EQ_PRESET_HEADPHONES:
        /* Headphones do reach down. We give back a little of the bass the stream
         * lost in encoding, and soften the region where low-bitrate Opus turns
         * harsh. */
        b[0].type = EQ_HIGHPASS;   b[0].freq =   30.0f; b[0].q = 0.707f;
        b[1].type = EQ_LOWSHELF;   b[1].freq =  100.0f; b[1].q = 0.707f; b[1].gain_db = +2.0f;
        b[2].type = EQ_PEAK;       b[2].freq = 3000.0f; b[2].q = 1.0f;  b[2].gain_db = -1.5f;
        return 3;

    case EQ_PRESET_VOICE:
        /* Dialogue. Remove the mud in the low mids, lift the intelligibility
         * band, cut what carries no speech. */
        b[0].type = EQ_HIGHPASS;   b[0].freq =  110.0f; b[0].q = 0.707f;
        b[1].type = EQ_PEAK;       b[1].freq =  350.0f; b[1].q = 1.0f;  b[1].gain_db = -3.0f;
        b[2].type = EQ_PEAK;       b[2].freq = 2500.0f; b[2].q = 0.9f;  b[2].gain_db = +4.0f;
        b[3].type = EQ_HIGHSHELF;  b[3].freq = 9000.0f; b[3].q = 0.707f; b[3].gain_db = -3.0f;
        return 4;

    case EQ_PRESET_NIGHT:
        /* Listening at low volume. The ear loses bass and treble as the level
         * drops (equal-loudness contours): we give them back, which lets you
         * listen quieter without losing anything. It is the old "loudness"
         * button of amplifiers, and it answers a real phenomenon. */
        b[0].type = EQ_LOWSHELF;   b[0].freq =  120.0f; b[0].q = 0.707f; b[0].gain_db = +5.0f;
        b[1].type = EQ_PEAK;       b[1].freq =  800.0f; b[1].q = 0.8f;  b[1].gain_db = -2.0f;
        b[2].type = EQ_HIGHSHELF;  b[2].freq = 6000.0f; b[2].q = 0.707f; b[2].gain_db = +4.0f;
        return 3;

    default:
        return 0;   /* CUSTOM: the caller keeps its own bands */
    }
}

/* The returned strings are i18n keys — see eq.h. They do not follow the code
 * into English; they follow `resources/i18n/`. */
const char *eq_preset_key(eq_preset_t p)
{
    switch (p) {
        case EQ_PRESET_FLAT:        return "eq/preset_flat";
        case EQ_PRESET_HANDHELD:    return "eq/preset_speakers";
        case EQ_PRESET_HEADPHONES:  return "eq/preset_headphones";
        case EQ_PRESET_VOICE:       return "eq/preset_voice";
        case EQ_PRESET_NIGHT:       return "eq/preset_quiet";
        case EQ_PRESET_CUSTOM:      return "eq/preset_custom";
        default:                    return "eq/preset_flat";
    }
}

const char *eq_type_key(eq_type_t t)
{
    switch (t) {
        case EQ_OFF:        return "eq/type_off";
        case EQ_HIGHPASS:   return "eq/type_highpass";
        case EQ_LOWSHELF:   return "eq/type_lowshelf";
        case EQ_PEAK:       return "eq/type_peak";
        case EQ_HIGHSHELF:  return "eq/type_highshelf";
        case EQ_LOWPASS:    return "eq/type_lowpass";
        default:            return "eq/type_off";
    }
}
