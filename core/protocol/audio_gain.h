/* audio_gain.h - output volume, as a PURE function.
 *
 * The Shadow stream arrives at a fixed level: neither the server nor Opus
 * exposes a volume control. On console the system volume is rarely enough -
 * the dock headphone output and the built-in speakers do not have the same
 * efficiency, and a Windows VM whose mixer sits at 30% is already attenuated
 * when it reaches us. Hence a gain applied on our side, AT THE LAST MOMENT
 * (just before output), so that it covers everything played whatever the
 * decode path.
 *
 * The rule that matters is CLAMPING. A gain applied naively to an `int16_t`
 * OVERFLOWS: 20000 * 2 = 40000, which does not fit in a signed 16-bit integer
 * and WRAPS to -25536. The sound does not become too loud, it becomes a
 * crackle - and the defect is only audible at high volume on loud passages,
 * so not during a quiet test. We therefore clamp explicitly.
 *
 * Pure - no state, no I/O, no getenv - so it can be checked offline by
 * tests/test_audio_gain.c, with no console and no virtual machine.
 */
#ifndef AUDIO_GAIN_H
#define AUDIO_GAIN_H

#include <stddef.h>
#include <stdint.h>

/* Bounds of the setting, in percent. Above 100 this is a BOOST: the signal
 * then goes past what the source contained, and loud passages clip. That is
 * accepted and it is the point (an over-quiet VM stays inaudible otherwise),
 * but it is why 100 remains the default. */
#define AUDIO_GAIN_MIN 0u
#define AUDIO_GAIN_MAX 300u
#define AUDIO_GAIN_DEFAUT 100u

/* Applies `pourcent` to `n` interleaved samples, IN PLACE.
 *
 * At 100 the buffer is not touched at all: that is the common case, and it
 * must cost nothing and risk nothing. */
static inline void audio_gain_apply(int16_t *pcm, size_t n, uint32_t pourcent)
{
    if (!pcm || pourcent == AUDIO_GAIN_DEFAUT) return;

    if (pourcent > AUDIO_GAIN_MAX) pourcent = AUDIO_GAIN_MAX;

    if (pourcent == 0) {
        for (size_t i = 0; i < n; i++) pcm[i] = 0;
        return;
    }

    for (size_t i = 0; i < n; i++) {
        /* In 32 bits: 32767 * 300 fits easily, the computation does not
         * overflow BEFORE the clamp. The order is what matters - clamp after
         * multiplying in 32 bits, never multiply in 16. */
        int32_t v = ((int32_t)pcm[i] * (int32_t)pourcent) / 100;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
    }
}

/* === EQV1(c) 2026-09-11 - A VOLUME CHANGE RAMPS ACROSS ONE BUFFER ===
 *
 * audio_gain_apply takes a new percentage from the next sample on, and a step
 * in the gain is a step in the waveform: a click. Measured offline on
 * Opus-decoded music: 7455 LSB for 100 -> 0 % or 100 -> 200 %, 14715 for
 * 100 -> 300 %. This ramps linearly from `from` to `to` across the `frames`
 * interleaved frames of `channels` channels and lands EXACTLY on `to` at the
 * last frame: 0 LSB against an ideal 10 ms ramp on a 480-frame buffer (on a
 * 960-frame one the ramp spans 20 ms and 1233 LSB of click are left).
 *
 * Equal percentages fall back to audio_gain_apply, bit for bit - so 100 %
 * still touches nothing. The same clamping rule: multiply wide (64 bits here,
 * the weight carries a factor `frames`), clamp after. */
static inline void audio_gain_ramp(int16_t *pcm, size_t frames, int channels,
                                   uint32_t from, uint32_t to)
{
    if (!pcm || frames == 0 || channels < 1) return;
    if (from > AUDIO_GAIN_MAX) from = AUDIO_GAIN_MAX;
    if (to   > AUDIO_GAIN_MAX) to   = AUDIO_GAIN_MAX;
    if (from == to) { audio_gain_apply(pcm, frames * (size_t)channels, to); return; }

    const int64_t den = 100 * (int64_t)frames;
    for (size_t t = 0; t < frames; t++) {
        const int64_t num = (int64_t)from * (int64_t)frames
                          + ((int64_t)to - (int64_t)from) * (int64_t)(t + 1);
        for (int c = 0; c < channels; c++) {
            const size_t i = t * (size_t)channels + (size_t)c;
            int64_t v = ((int64_t)pcm[i] * num) / den;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            pcm[i] = (int16_t)v;
        }
    }
}

#endif /* AUDIO_GAIN_H */
