/* test_wav.c - parsing a WAV header against MALFORMED files.
 *
 * WHY THIS SUITE EXISTS. A WAV is a format of blocks CHAINED BY THEIR SIZE:
 * every block announces its length and you advance by it. That is exactly the
 * shape that produces an infinite loop or an out-of-range read as soon as one
 * size is wrong - the same family as `pb_skip_field`'s integer overflow, where a
 * twelve-byte message hung the control thread forever.
 *
 * And these bytes come from a file anyone can drop on the SD card. So we treat
 * them like the wire: hostile by default.
 *
 * Each check names its COUNTER-CASE.
 */
#include "../clients/borealis/ui/wav.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static void pose_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);       p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void pose_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/* Builds a minimal, VALID WAV into `buf`. Returns its total size. `channels`,
 * `bits` and `format` are parameters so the refusal cases start from a healthy
 * file and change only ONE thing - otherwise a refusal does not prove which rule
 * produced it. */
static size_t make_wav(uint8_t *buf, uint16_t format, uint16_t channels,
                        uint16_t bits, uint32_t frequency, uint32_t data_len)
{
    const uint32_t total_size = 4u + (8u + 16u) + (8u + data_len);
    memcpy(buf, "RIFF", 4);
    pose_u32(buf + 4, total_size);
    memcpy(buf + 8, "WAVE", 4);

    memcpy(buf + 12, "fmt ", 4);
    pose_u32(buf + 16, 16);
    pose_u16(buf + 20, format);
    pose_u16(buf + 22, channels);
    pose_u32(buf + 24, frequency);
    pose_u32(buf + 28, frequency * channels * (uint32_t)(bits / 8));  /* debit */
    pose_u16(buf + 32, (uint16_t)(channels * (bits / 8)));            /* alignement */
    pose_u16(buf + 34, bits);

    memcpy(buf + 36, "data", 4);
    pose_u32(buf + 40, data_len);
    for (uint32_t i = 0; i < data_len; i++) buf[44 + i] = (uint8_t)(i & 0xFF);
    return 44u + data_len;
}

static void honnete(void)
{
    uint8_t buf[512];
    wav_info_t w;

    const size_t n = make_wav(buf, 1, 1, 16, 48000, 96);
    CHECK(wav_analyser(buf, n, &w), "a 16-bit mono PCM WAV parses");
    CHECK(w.frequency == 48000u, "sample rate");
    CHECK(w.channels == 1, "channels");
    CHECK(w.offset == 44u, "the data starts after the header");
    CHECK(w.bytes == 96u, "all the data is returned");
    CHECK(w.frames == 48u, "96 bytes of mono 16-bit = 48 frames");

    const size_t n2 = make_wav(buf, 1, 2, 16, 48000, 96);
    CHECK(wav_analyser(buf, n2, &w) && w.channels == 2 && w.frames == 24u,
            "in stereo, the same payload is half as many frames");
}

/* COUNTER-CASE - WHAT WE REFUSE, AND WHY. An unexpected format must give
 * SILENCE, never noise: float PCM read as 16-bit integers produces values
 * unrelated to samples, that is, white noise at full volume in someone's ears. */
static void refusal(void)
{
    uint8_t buf[512];
    wav_info_t w;
    size_t n;

    n = make_wav(buf, 3, 1, 32, 48000, 96);          /* IEEE float */
    CHECK(!wav_analyser(buf, n, &w),
            "COUNTER-CASE: float PCM is REFUSED, not read as 16-bit");

    n = make_wav(buf, 1, 1, 8, 48000, 96);
    CHECK(!wav_analyser(buf, n, &w), "8-bit refused");
    n = make_wav(buf, 1, 1, 24, 48000, 96);
    CHECK(!wav_analyser(buf, n, &w), "24-bit refused");
    n = make_wav(buf, 1, 6, 16, 48000, 96);
    CHECK(!wav_analyser(buf, n, &w), "5.1 refused: we do not know how to downmix it");
    n = make_wav(buf, 1, 0, 16, 48000, 96);
    CHECK(!wav_analyser(buf, n, &w), "zero channels refused");
    n = make_wav(buf, 1, 1, 16, 48000, 0);
    CHECK(!wav_analyser(buf, n, &w), "an empty sound is not a sound");
    n = make_wav(buf, 1, 1, 16, 4000, 96);
    CHECK(!wav_analyser(buf, n, &w), "frequence aberrante basse refusee");
    n = make_wav(buf, 1, 1, 16, 400000, 96);
    CHECK(!wav_analyser(buf, n, &w), "frequence aberrante haute refusee");

    /* An incomplete frame is TRUNCATED, not refused: a file cut short while
     * copying must give the sound it does contain. But never read half a
     * frame. */
    n = make_wav(buf, 1, 2, 16, 48000, 97);          /* 97 = 24 frames + 1 byte */
    CHECK(wav_analyser(buf, n, &w) && w.frames == 24u && w.bytes == 96u,
            "an incomplete last frame is truncated, never read half way");
}

/* COUNTER-CASE - THE HOSTILE SIZES. This is the suite's reason for
     * existing. */
static void hostile(void)
{
    uint8_t buf[512];
    wav_info_t w;

    /* Neither RIFF nor WAVE. A text file, an image, a half-copied WAV. */
    CHECK(!wav_analyser((const uint8_t *)"not a wav at all and so what", 27, &w),
            "an arbitrary file is refused");

    /* Shorter than the container: the first read would already go out. */
    const size_t n = make_wav(buf, 1, 1, 16, 48000, 96);
    for (size_t k = 0; k < 12; k++)
        CHECK(!wav_analyser(buf, k, &w), "a buffer shorter than the RIFF container is refused");

    /* COUNTER-CASE - THE BLOCK THAT ANNOUNCES MORE THAN THE FILE. This is the
     * dangerous case: without the comparison against the REMAINING space, `data`
     * would point past the buffer and the mixer would read foreign memory. */
    pose_u32(buf + 40, 0xFFFFFFFFu);
    CHECK(!wav_analyser(buf, n, &w),
            "COUNTER-CASE: a 4 GB `data` block in a 140-byte file is refused");

    /* COUNTER-CASE - THE INTEGER OVERFLOW. On a target where `size_t` is 32
     * bits, `pos + 8 + size` returns a position SMALLER than `pos`: the loop
     * goes backwards and never ends. The test cannot prove the absence of an
     * infinite loop, but it pins the value that triggered it - if it returns
     * `true`, the guard is gone. */
    make_wav(buf, 1, 1, 16, 48000, 96);
    pose_u32(buf + 16, 0xFFFFFFF8u);                 /* size du bloc `fmt ` */
    CHECK(!wav_analyser(buf, n, &w),
            "COUNTER-CASE: a block size close to 2^32 is refused, never added");

    /* A `fmt ` too short to carry its fields. It fits in the buffer, so only an
     * explicit check of its size rejects it - without that we would read the
     * NEXT block's bytes as a sample rate. */
    make_wav(buf, 1, 1, 16, 48000, 96);
    pose_u32(buf + 16, 8);
    CHECK(!wav_analyser(buf, n, &w), "an 8-byte `fmt ` block is refused");

    /* No `fmt ` chunk: the file does not say what it contains. */
    make_wav(buf, 1, 1, 16, 48000, 96);
    memcpy(buf + 12, "LIST", 4);
    CHECK(!wav_analyser(buf, n, &w), "with no `fmt ` block, we do not guess the format");

    /* COUNTER-CASE - `data` BEFORE `fmt `. The specification does not fix the
     * order. Leaving the loop on the first `data` would make this VALID file
     * unreadable. We build it by hand: `data` (8 bytes of payload) then
     * `fmt `. */
    {
        uint8_t b[128];
        memcpy(b, "RIFF", 4); pose_u32(b + 4, 4 + 8 + 8 + 8 + 16); memcpy(b + 8, "WAVE", 4);
        memcpy(b + 12, "data", 4); pose_u32(b + 16, 8);
        for (int i = 0; i < 8; i++) b[20 + i] = (uint8_t)i;
        memcpy(b + 28, "fmt ", 4); pose_u32(b + 32, 16);
        pose_u16(b + 36, 1); pose_u16(b + 38, 1);
        pose_u32(b + 40, 48000); pose_u32(b + 44, 96000);
        pose_u16(b + 48, 2); pose_u16(b + 50, 16);
        CHECK(wav_analyser(b, 52, &w) && w.offset == 20u && w.frames == 4u,
                "COUNTER-CASE: `data` before `fmt ` stays readable - the order is not imposed");
    }

    /* COUNTER-CASE - PADDING TO AN EVEN BYTE. A block of ODD size is followed
     * by a padding byte. Forgetting it shifts every following block by one byte:
     * `data` becomes unfindable in an otherwise valid file. */
    {
        uint8_t b[128];
        memcpy(b, "RIFF", 4); pose_u32(b + 4, 60); memcpy(b + 8, "WAVE", 4);
        /* An unknown 3-byte chunk (odd) + 1 byte of padding. */
        memcpy(b + 12, "junk", 4); pose_u32(b + 16, 3);
        b[20] = b[21] = b[22] = 0; b[23] = 0;        /* charge + bourrage */
        memcpy(b + 24, "fmt ", 4); pose_u32(b + 28, 16);
        pose_u16(b + 32, 1); pose_u16(b + 34, 1);
        pose_u32(b + 36, 48000); pose_u32(b + 40, 96000);
        pose_u16(b + 44, 2); pose_u16(b + 46, 16);
        memcpy(b + 48, "data", 4); pose_u32(b + 52, 8);
        for (int i = 0; i < 8; i++) b[56 + i] = (uint8_t)i;
        CHECK(wav_analyser(b, 64, &w) && w.offset == 56u,
                "COUNTER-CASE: an odd-sized block is followed by a padding "
                "byte - ignoring it makes `data` unfindable");
    }

    /* The null pointer and the null output, on the same principle as
     * everywhere. */
    CHECK(!wav_analyser(NULL, 64, &w), "a null pointer is refused");
    make_wav(buf, 1, 1, 16, 48000, 96);
    CHECK(!wav_analyser(buf, n, NULL), "a null output is refused");
}

/* Any truncation of a VALID file must return false or a coherent result, never
 * read past the end. We sweep every length: it is a property, not a case, and it
 * is what a file cut short while copying exercises. */
static void troncatures(void)
{
    uint8_t buf[512];
    const size_t n = make_wav(buf, 1, 1, 16, 48000, 128);
    int fautes = 0;

    for (size_t k = 0; k <= n; k++) {
        wav_info_t w;
        if (!wav_analyser(buf, k, &w)) continue;
        /* If it accepts, what it describes must fit INSIDE what it was given. */
        if (w.offset > k || w.bytes > k - w.offset) fautes++;
    }
    CHECK(fautes == 0,
            "on ANY accepted truncation, the described data fits inside the buffer");
}

int main(void)
{
    printf("== WAV header (blocks chained by their size, hostile inputs) ==\n");
    honnete();
    refusal();
    hostile();
    troncatures();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
