/* test_audio_gain.c - the output volume, and above all its clipping.
 *
 * Each check names its COUNTER-CASE: the exact input that would produce an
 * audible defect. A failure here therefore says what has just been undone.
 */
#include "../core/protocol/audio_gain.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static void neutre(void)
{
    int16_t pcm[4] = { -32768, -1, 1, 32767 };
    int16_t copie[4];
    memcpy(copie, pcm, sizeof pcm);

    audio_gain_apply(pcm, 4, 100);
    CHECK(memcmp(pcm, copie, sizeof pcm) == 0,
            "at 100 % the buffer must be returned INTACT, extremes included");

    audio_gain_apply(NULL, 4, 200);   /* must not crash */
    audio_gain_apply(pcm, 0, 200);    /* must do nothing */
    CHECK(memcmp(pcm, copie, sizeof pcm) == 0, "zero samples = no effect");
}

static void attenuation_applied(void)
{
    int16_t pcm[3] = { 1000, -1000, 0 };
    audio_gain_apply(pcm, 3, 50);
    CHECK(pcm[0] == 500 && pcm[1] == -500 && pcm[2] == 0, "50 % halves");

    int16_t muet[3] = { 32767, -32768, 1234 };
    audio_gain_apply(muet, 3, 0);
    CHECK(muet[0] == 0 && muet[1] == 0 && muet[2] == 0,
            "0 % cuts the sound completely");
}

static void boost_and_clipping(void)
{
    int16_t pcm[2] = { 1000, -1000 };
    audio_gain_apply(pcm, 2, 200);
    CHECK(pcm[0] == 2000 && pcm[1] == -2000,
            "200 % doubles a signal that has room to double");

    /* COUNTER-CASE - THE WRAPAROUND.
     * 20000 * 2 = 40000, which does NOT fit in an int16_t. An implementation
     * that multiplies in 16 bits returns -25536: a strongly positive sample
     * becomes strongly negative. To the ear that is not "too loud", it is a
     * crackle - and it only appears on the loud passages, hence never during a
     * quiet test. */
    int16_t fort[2] = { 20000, -20000 };
    audio_gain_apply(fort, 2, 200);
    CHECK(fort[0] == 32767,
            "COUNTER-CASE: a positive overflow CLIPS at the maximum, it does not wrap");
    CHECK(fort[1] == -32768,
            "COUNTER-CASE: a negative overflow CLIPS at the minimum");

    /* Maximum gain on the maximum signal: the arithmetic worst case. */
    int16_t extreme[2] = { 32767, -32768 };
    audio_gain_apply(extreme, 2, AUDIO_GAIN_MAX);
    CHECK(extreme[0] == 32767 && extreme[1] == -32768,
            "COUNTER-CASE: max gain on the max signal - clips cleanly");

    /* A percentage beyond the bound is clamped to the bound, not silently
     * refused: the caller must not be able to ask for the impossible. */
    int16_t outside[1] = { 1000 };
    audio_gain_apply(outside, 1, 100000);
    CHECK(outside[0] == 3000, "an absurd percentage is clamped to AUDIO_GAIN_MAX");
}

/* === EQV1(c) 2026-09-11 - A VOLUME CHANGE RAMPS ACROSS ONE BUFFER ===
 * The instant step was a click: 7455 LSB for 100 -> 0 % on Opus-decoded
 * music. The ramp must be INVISIBLE when nothing changes, land exactly on the
 * new gain, and keep the clamping rule. */
static void ramp(void)
{
    static int16_t a[960], b[960];
    const uint32_t G[] = { 0, 50, 100, 250, 300 };
    int same = 1;
    for (unsigned g = 0; g < sizeof G / sizeof G[0]; g++) {
        for (int i = 0; i < 960; i++) a[i] = b[i] = (int16_t)((i * 7919) % 65536 - 32768);
        audio_gain_apply(a, 960, G[g]);
        audio_gain_ramp(b, 480, 2, G[g], G[g]);
        if (memcmp(a, b, sizeof a) != 0) same = 0;
    }
    CHECK(same, "at a constant gain the ramp IS audio_gain_apply, bit for bit - 100 % still "
                "touches nothing");

    /* COUNTER-CASE - THE STEP. 100 -> 0 % used to cut from one sample to the
     * next: 10000 then 0. */
    int16_t s[480 * 2];
    for (int i = 0; i < 480 * 2; i++) s[i] = 10000;
    audio_gain_ramp(s, 480, 2, 100, 0);
    int falling = 1;
    for (int t = 1; t < 480; t++)
        if (s[t * 2] > s[(t - 1) * 2] || s[t * 2] != s[t * 2 + 1]) falling = 0;
    CHECK(s[0] >= 10000 - 10000 / 480 - 1 && s[0] < 10000,
          "COUNTER-CASE: 100 -> 0 % no longer drops at once - the first frame moves 1/480 of the way");
    CHECK(s[479 * 2] == 0 && s[479 * 2 + 1] == 0,
          "... and the last frame lands exactly on the new gain");
    CHECK(falling, "... falling steadily, the same on both channels");

    /* COUNTER-CASE - THE WRAPAROUND, on the way up. */
    int16_t c[480 * 2];
    for (int t = 0; t < 480; t++) { c[t * 2] = 20000; c[t * 2 + 1] = -20000; }
    audio_gain_ramp(c, 480, 2, 100, 300);
    int clean = 1;
    for (int t = 0; t < 480; t++)
        if (c[t * 2] < 20000 || c[t * 2 + 1] > -20000) clean = 0;
    CHECK(clean && c[479 * 2] == 32767 && c[479 * 2 + 1] == -32768,
          "COUNTER-CASE: ramping to 300 % on a loud signal CLIPS, it does not wrap");

    int16_t o[2] = { 1000, -1000 };
    audio_gain_ramp(o, 1, 2, 1000, 100000);
    CHECK(o[0] == 3000 && o[1] == -3000,
          "absurd percentages are clamped to AUDIO_GAIN_MAX at both ends");

    audio_gain_ramp(NULL, 480, 2, 100, 0);    /* must not crash */
    int16_t u[2] = { 5, 6 };
    audio_gain_ramp(u, 0, 2, 100, 0);
    CHECK(u[0] == 5 && u[1] == 6, "no frame, no effect");
}

int main(void)
{
    printf("== output volume (gain and clipping) ==\n");
    neutre();
    attenuation_applied();
    boost_and_clipping();
    ramp();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
