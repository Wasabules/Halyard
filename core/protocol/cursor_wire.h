/* === THIS MODULE IS THE *STFP* FRAMING, NOT THE CURSOR'S ALONE ===
 *
 * K15d, 2026-08-29. The name dates from when `:base+20` was the only STFP
 * channel we read. The repo's first capture taken in the `reliability` profile
 * shows the SAME framing carrying VIDEO over TCP:
 *
 *   0x22, 1372 frames: 02 ff 24 d8 3c 00 | 00 00 01 61 ...   <- video
 *   0x12,   98 frames: 04 00 01 00 ...                       <- cursor
 *
 * and that out of 14,043 reads, 1470 satisfy `charge_len + 10 == block size`
 * EXACTLY - which establishes the 10-byte header and the u32 length on the
 * WIRE, no longer only in the disassembly.
 *
 * The module is not renamed: thirty call sites depend on it, and the churn
 * would carry a regression risk for no functional gain. But reading it as
 * "cursor-specific" would be wrong, and this repo has already paid for that
 * kind of misleading name (`ctrl_video_tcp.c` serves the cursor, a leftover of
 * a refuted hypothesis). This is written here so nobody gets caught out.
 *
 * ---
 *
 * cursor_wire.h - the semantics of the cursor channel `:base+20`, as PURE
 * functions.
 *
 * This channel is named `Cursor`: the official client NAMES its sockets with
 * their port in its telemetry, and that map was verified on two different port
 * bases (KB.md §3.37). It carries the IMAGE of the remote machine's pointer - a
 * bitmap, not video.
 *
 * For months the repo took it for a fallback video channel
 * (`VideoSslTcpChannel`). Consequence: our frames were arriving perfectly well,
 * and the "VST frame missing Annex-B start code" guard threw them away every
 * session. We had been receiving the pointer all along and never displaying it.
 *
 * Each of the three rules below is backed by a measurement on a capture of the
 * official client (`captures/scenario_*`, 140 frames analysed):
 *
 *   C1 - a frame's size is read from its header; it is NOT inferred from the
 *        size of the read. A 4142-byte frame was observed split into
 *        1024 + 3118: "one read = one frame" truncates it.
 *   C2 - `image_size == 0` means the cursor is HIDDEN; it is not an error.
 *        21 frames out of 140.
 *   C3 - the image header announces dimensions; they must be VALIDATED against
 *        the size actually received before reading a single pixel. Those bytes
 *        come from the network.
 *
 * They are pure - no state, no I/O, no logging, no getenv - so that
 * tests/test_cursor_wire.c can check them offline, with no console and no
 * virtual machine. The SHADOW_* toggles stay with the callers: this module says
 * what the protocol MEANS, not what we choose to do about it.
 */
#ifndef CURSOR_WIRE_H
#define CURSOR_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame header, at the head of every message on the channel. */
#define CURSOR_WIRE_HEADER_LEN 10

/* Types observed at the head of a frame. */
#define CURSOR_WIRE_TYPE_IMAGE 0x12  /* carries a cursor image */
#define CURSOR_WIRE_TYPE_EMPTY  0x02  /* longueur nulle, charge nulle ; role indetermine */

/* Image header, at the head of the payload of an IMAGE frame. */
#define CURSOR_WIRE_IMG_HEADER_LEN 36

/* Values of the `format` field. */
#define CURSOR_WIRE_FMT_BGRA    2  /* 32 bits per pixel, stride = width * 4 */
#define CURSOR_WIRE_FMT_MASKS 1  /* monochrome AND/XOR masks, stride = width / 8 */

/* No cursor larger than this has ever been observed (32x32); the bound protects
 * the caller from an allocation dictated by the network. */
#define CURSOR_WIRE_DIM_MAX 256

/* === K16a 2026-08-29 - VERSION AND CAP ===
 *
 * The LOW nibble of byte 0 is the framing VERSION, the high nibble is the
 * class. The official client refuses any version outside {1, 2} before it even
 * reads the length (`tcp_frame.cpp`, `and $0xf,%eax ; cmp $0x2,%al ; jne
 * reject`). We were accepting any byte 0 at all.
 *
 * This is not theoretical. The first ten bytes of a TLS ALERT begin
 * `15 03 03 00 02 ...`: read by the old parser they returned `true` with a
 * payload of 33,685,507 bytes. This module was the only rampart, and it was not
 * one.
 *
 * The cap answers the same problem from the other end. Since K15c the length is
 * a u32, and since K15g the caller GROWS its buffer to accommodate it: a length
 * coming off the network therefore dictates an allocation. It has to be bounded
 * by the caller, who alone knows what its channel can legitimately carry.
 *
 * And a quieter reason: on a target where `size_t` is 32 bits,
 * `10 + 0xFFFFFFFF` OVERFLOWS and yields 9. The caller would then believe it
 * held a complete nine-byte frame and would restart in the middle of the data.
 * The cap is what makes that arithmetic impossible, not a comfort measure. */
#define CURSOR_WIRE_VERSION_MIN 1
#define CURSOR_WIRE_VERSION_MAX 2

/* Default cap: the one `ctrl_video_tcp.c` already applies to its buffer. A
 * measured 1080p key frame is 213,598 bytes; 8 MB leaves headroom for a higher
 * definition without ever allowing an absurd allocation. */
#define CURSOR_WIRE_CAP_DEFAULT (8u * 1024u * 1024u)

typedef struct {
    uint8_t  type;        /* CURSOR_WIRE_TYPE_* */
    uint8_t  seq;         /* incremented from one frame to the next */
    uint32_t payload_len;  /* payload bytes that FOLLOW the header.
                           * u32 LE at bytes 2..5 (K15c, read off the objdump:
                           * `mov 0x2(%rsi),%eax`). It used to be read as a u16:
                           * harmless on a cursor, fatal on a key frame. */
    uint32_t id;          /* the frame identifier, increasing */
    size_t   frame_len;   /* = CURSOR_WIRE_HEADER_LEN + charge_len */
} cursor_wire_header_t;

typedef struct {
    uint32_t image_size;    /* pixel bytes; 0 = the cursor is HIDDEN (C2) */
    uint32_t format;          /* CURSOR_WIRE_FMT_* */
    uint32_t width, height;
    uint32_t stride;             /* bytes per row */
    uint32_t hot_x, hot_y;/* hotspot, in pixels from the top-left corner */
    size_t   offset_pixels;   /* where the pixels start WITHIN THE PAYLOAD */
    bool     hidden;          /* raccourci : image_size == 0 */
} cursor_wire_image_t;

/* Reads the frame header from `p` (`n` bytes available).
 *
 * Returns false if `n` is not enough. Returns true even when the frame is
 * INCOMPLETE: `out->frame_len` then says how many bytes must be waited for in
 * total. That is what lets the caller reassemble.
 *
 * C1 - NEVER infer the frame size from the size of the read. The previous
 * parser computed `length = (bytes_read - position) - 10`, that is to say "one
 * read = one frame": a 4142-byte frame arriving as 1024 + 3118 was then read as
 * two frames, both invalid. */
/* K16a - `cap` is the largest `charge_len` the caller will accept. A greater
 * length returns `false`: that is a REFUSAL, not a truncation. Pass
 * `CURSOR_WIRE_CAP_DEFAULT` when there is no reason to be narrower.
 *
 * `SHADOW_STFP_STRICT=0` restores the previous acceptance - any version, any
 * length - for an honest A/B. The direction of the toggle follows the repo's
 * rule: the revert value RESTORES the old path, it does not switch off the new
 * one. */
bool cursor_wire_parse_header(const uint8_t *p, size_t n, uint32_t cap,
                              cursor_wire_header_t *out);

/* Reads the image header from the PAYLOAD of an IMAGE frame
 * (`charge` = p + CURSOR_WIRE_HEADER_LEN, `n` = charge_len).
 *
 * Returns false if the payload is too short, if the dimensions are absurd, or
 * if the announced pixels do not fit in what was actually received (C3).
 * Returns true with `out->hidden = true` when `image_size == 0`: that is a
 * hidden cursor, not an error (C2). */
bool cursor_wire_parse_image(const uint8_t *charge, size_t n,
                             cursor_wire_image_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CURSOR_WIRE_H */
