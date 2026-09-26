/* wav - see wav.h. Pure function: no I/O, no allocation, no state. */
#include "wav.h"

#include <string.h>

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Values of the `wFormatTag` field. Only integer PCM is accepted: float (0x0003)
 * and extensible (0xFFFE) exist too, and feeding them to a 16-bit sample reader
 * would produce values unrelated to samples - that is, white noise at full
 * volume. */
#define WAV_FMT_PCM 0x0001

bool wav_analyser(const uint8_t *p, size_t n, wav_info_t *out)
{
    if (!p || !out) return false;
    /* 12 bytes of RIFF container at minimum, before even looking for a chunk. */
    if (n < 12) return false;
    if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) return false;

    bool     fmt_seen   = false;
    uint16_t channels   = 0;
    uint32_t rate       = 0;
    uint16_t bits       = 0;
    size_t   data_off   = 0;
    size_t   data_len   = 0;

    /* === THE CHUNK LOOP, AND THE TWO WAYS TO GET LOST IN IT ===
     *
     * We walk from chunk to chunk, each one declaring its size. Two traps, both
     * of them present in real files:
     *
     *   1. A SIZE THAT OVERFLOWS. `size` is a u32 that came from the file; on a
     *      target where `size_t` is 32 bits, `pos + 8 + size` OVERFLOWS and
     *      yields a position SMALLER than `pos` - the loop walks backwards and
     *      never ends. So we compare against the REMAINING space, never by
     *      adding.
     *   2. A ZERO SIZE. A zero-byte chunk is legal; it still advances us by its
     *      eight header bytes, so the loop makes progress. That is guaranteed by
     *      the `pos += 8 + ...`, not by any special case.
     *
     * The pad-to-even-byte rule comes from the RIFF spec: a chunk of odd size is
     * followed by one padding byte. Forgetting it shifts every following chunk
     * by one byte - so `data` goes missing on a file that is perfectly valid. */
    size_t pos = 12;
    while (pos + 8 <= n) {
        const uint8_t *id = p + pos;
        const uint32_t size = read_u32(p + pos + 4);

        const size_t remaining = n - pos - 8;
        if ((size_t)size > remaining) return false;  /* chunk runs past the buffer */

        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16) return false;
            const uint8_t *f = p + pos + 8;
            if (read_u16(f) != WAV_FMT_PCM) return false;
            channels = read_u16(f + 2);
            rate     = read_u32(f + 4);
            bits     = read_u16(f + 14);
            fmt_seen = true;
        } else if (memcmp(id, "data", 4) == 0) {
            data_off = pos + 8;
            data_len = (size_t)size;
            /* We do NOT break out of the loop here. The `fmt ` chunk comes before
             * `data` in every sane file, but the spec does not require it, and
             * stopping at the first `data` would make a valid file unreadable. */
        }

        size_t advance = 8u + (size_t)size;
        if (size & 1u) advance++;                /* pad to an even byte */
        if (advance > n - pos) break;            /* the padding runs past: stop here */
        pos += advance;
    }

    if (!fmt_seen) return false;
    if (data_len == 0) return false;             /* an empty sound is not a sound */
    if (bits != 16) return false;
    if (channels != 1 && channels != 2) return false;
    /* Sanity bounds on the sample rate: they reject a corrupted header before it
     * is used to compute a duration or a resampling step. 8 kHz and 192 kHz
     * bracket everything a production tool emits. */
    if (rate < 8000u || rate > 192000u) return false;

    const size_t bytes_per_frame = (size_t)channels * 2u;
    /* A trailing incomplete frame is TRUNCATED, not rejected: a file cut short
     * during a copy should still play the sound it does contain, not silence.
     * But it is never read half-way - the division is what drops it. */
    const size_t frames = data_len / bytes_per_frame;
    if (frames == 0) return false;

    out->frequency = rate;
    out->channels    = channels;
    out->offset    = data_off;
    out->bytes    = frames * bytes_per_frame;
    out->frames    = frames;
    return true;
}
