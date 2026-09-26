/* wav - parsing a PCM WAV file, as a PURE function.
 *
 * === WHY THIS IS A MODULE OF ITS OWN (S88, 2026-08-29) ===
 *
 * UI sounds arrive as WAV files, produced outside this repo. That makes this the
 * SECOND place - after `streaming/proto.c` - where our code reads bytes it did
 * not produce, and the first where those bytes come from a file anyone can drop
 * onto the SD card.
 *
 * A RIFF header is a format whose chunks are CHAINED BY THEIR OWN SIZE FIELD:
 * each chunk declares its length, and you walk from one to the next. That is
 * exactly the shape that turns one wrong size into an infinite loop or an
 * out-of-bounds read - the same family as the integer overflow in
 * `pb_skip_field`, where a twelve-byte message hung the control thread forever
 * (KB §9, 2026-08-25).
 *
 * Hence a module of its own, PURE (no I/O, no allocation, no state), so it can
 * be checked offline against MALFORMED headers and not only against honest
 * files. See tests/test_wav.c.
 *
 * What it accepts, and nothing else: RIFF/WAVE, 16-bit integer PCM, 1 or 2
 * channels. Everything else is refused - an unexpected format must come out
 * silent, never as full-volume noise in someone's ears.
 */
#ifndef UI_WAV_H
#define UI_WAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t    frequency;     /* samples per second */
    uint16_t    channels;        /* 1 or 2 */
    /* Offset and size of the DATA inside the input buffer. Nothing is copied:
     * the caller already owns the bytes, and a pure module does not allocate. */
    size_t      offset;
    size_t      bytes;
    /* Frame count (one sample per channel). Derived, but returned anyway: this
     * is the value the mixer works from, and recomputing it in every caller is
     * the kind of duplication that ends up diverging. */
    size_t      frames;
} wav_info_t;

/* Parse the header of `p[0..n-1]`.
 *
 * Returns false - leaving `out` untouched - if this is not a 16-bit mono/stereo
 * PCM WAV, if a chunk declares a size that runs past the buffer, or if the data
 * is empty. NEVER reads outside `p[0..n-1]`, whatever sizes the file declares.
 *
 * `n` is the REAL size of the buffer, not the one the file claims: that
 * distinction is what stops a `RIFF` lying about its length from making us read
 * past the end. */
bool wav_analyser(const uint8_t *p, size_t n, wav_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* UI_WAV_H */
