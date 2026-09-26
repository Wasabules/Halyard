/* cursor_wire.c - see cursor_wire.h for the why behind each rule. */

#include "cursor_wire.h"

#include <stdlib.h>   /* getenv, atoi - the K16a revert toggle */

/* K15c: `read_u16` went away together with the u16 read of the length. It had
 * no caller left, and `-Werror` was right to say so - that is in fact what
 * revealed that my first check of the tests was reading a stale binary. */

static uint32_t lire_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* K16a - the revert toggle. Read once, like the ~120 others in this repo.
 * `SHADOW_STFP_STRICT=0` gives the parser back its old credulity: any version,
 * any length. It exists only so an A/B can be run honestly - there is no
 * reason to use it in normal operation. */
static int strict_actif(void)
{
    static int g = -1;
    if (g < 0) {
        const char *e = getenv("SHADOW_STFP_STRICT");
        g = e ? atoi(e) : 1;
    }
    return g;
}

bool cursor_wire_parse_header(const uint8_t *p, size_t n, uint32_t cap,
                              cursor_wire_header_t *out)
{
    if (!p || !out || n < CURSOR_WIRE_HEADER_LEN) return false;

    /* === K16a - THE VERSION, BEFORE ANYTHING ELSE ===
     *
     * The LOW nibble is the framing version; the official client checks it
     * before it even reads the length. Without that guard, the first ten bytes
     * of a TLS alert (`15 03 03 00 02 ...`) passed for a valid header
     * announcing a 33 MB payload. See cursor_wire.h. */
    if (strict_actif()) {
        const uint8_t version = (uint8_t)(p[0] & 0x0Fu);
        if (version < CURSOR_WIRE_VERSION_MIN || version > CURSOR_WIRE_VERSION_MAX)
            return false;
    }

    out->type       = p[0];
    out->seq        = p[1];
    /* === K15c 2026-08-28 - THE LENGTH IS A u32, AND NO CAPTURE COULD HAVE
     * TOLD US SO ===
     *
     * The previous comment said: "p[4] and p[5] are always zero across the 140
     * frames analysed". That is true - and across ~3900 STFP frames pulled from
     * 6.9 GB of captures, without a single exception. It was still NOT proof of
     * the field's width: four frame sizes exist in the whole corpus (10, 46,
     * 302, 4142) and the largest one is structural - 10 of frame header + 36 of
     * image header + 4096 of 32x32 BGRA pixels. Our u16 read was therefore
     * never exercised beyond 6.3 % of its ceiling: u16 and u32 yield the SAME
     * value on 100 % of the bytes we possess.
     *
     * The disassembly settles it. `ShadowPCDisplay` @0xd61030:
     *
     *     movzbl (%rsi),%eax ; and $0xf,%eax ; cmp $0x2,%al ; jne reject
     *     mov 0x2(%rsi),%eax          <-- 32-BIT READ
     *
     * `8b 46 02` is a `mov r32, m32`. A u16 would have been encoded
     * `0f b7 46 02` (`movzwl`). Verified on two independent binaries, five
     * classes, same vtable quadruplet.
     *
     * Practical consequence: nothing changes today - the cursor tops out at
     * 4142 bytes. But a 1080p key frame on a TCP video channel runs 200 to
     * 400 KB: our u16 read would have truncated the length and **desynchronised
     * the parser for good, without a single error**. Same family as G4, where a
     * field was read crookedly and nothing ever protested.
     *
     * METHOD LESSON, worth keeping: "these bytes are always zero in the
     * captures" NEVER tells you the width of a field. It only tells you the
     * range that the captured scenario happened to exercise. */
    out->payload_len = lire_u32(p + 2);

    /* === K16a - THE CAP, BEFORE COMPUTING `trame_len` ===
     *
     * Order matters. Computing first and checking afterwards would let the
     * computation OVERFLOW on a target where `size_t` is 32 bits: there
     * `10 + 0xFFFFFFFF` yields 9, and the caller would believe it held a
     * complete nine-byte frame. So we reject BEFORE, on the 32-bit value,
     * where the overflow does not exist. */
    if (strict_actif() && out->payload_len > cap) return false;

    out->id         = lire_u32(p + 6);
    out->frame_len  = (size_t)CURSOR_WIRE_HEADER_LEN + (size_t)out->payload_len;
    return true;
}

bool cursor_wire_parse_image(const uint8_t *charge, size_t n,
                             cursor_wire_image_t *out)
{
    if (!charge || !out || n < CURSOR_WIRE_IMG_HEADER_LEN) return false;

    out->image_size  = lire_u32(charge + 8);
    out->format        = lire_u32(charge + 12);
    out->width       = lire_u32(charge + 16);
    out->height       = lire_u32(charge + 20);
    out->stride           = lire_u32(charge + 24);
    out->hot_x       = lire_u32(charge + 28);
    out->hot_y       = lire_u32(charge + 32);
    out->offset_pixels = CURSOR_WIRE_IMG_HEADER_LEN;
    out->hidden        = (out->image_size == 0);

    /* C2 - hidden cursor: the other fields are meaningless, we demand nothing
     * of them and return true. Dropping the frame would deprive the caller of
     * the only information it carries: the pointer must disappear. */
    if (out->hidden) return true;

    /* C3 - from here on, everything comes off the network. We validate before
     * letting the caller read a single pixel. */
    if (out->width == 0 || out->height == 0) return false;
    if (out->width > CURSOR_WIRE_DIM_MAX
        || out->height > CURSOR_WIRE_DIM_MAX) return false;

    /* The stride must cover a full row, otherwise computing a pixel address
     * would land outside the buffer. Both known formats can be checked
     * exactly. */
    uint32_t pas_mini;
    if (out->format == CURSOR_WIRE_FMT_BGRA) {
        pas_mini = out->width * 4u;
    } else if (out->format == CURSOR_WIRE_FMT_MASKS) {
        pas_mini = (out->width + 7u) / 8u;
    } else {
        return false;   /* unknown format: we do not guess the layout */
    }
    if (out->stride < pas_mini) return false;

    /* The announced size must match the announced geometry. The mask format
     * carries TWO of them (AND then XOR), hence the factor. */
    const uint32_t attendu = out->format == CURSOR_WIRE_FMT_MASKS
                           ? out->stride * out->height * 2u
                           : out->stride * out->height;
    if (out->image_size != attendu) return false;

    /* And above all: those pixels must fit in what we ACTUALLY received. */
    if (out->image_size > n - out->offset_pixels) return false;

    /* A hotspot outside the image would draw the cursor off to one side. */
    if (out->hot_x >= out->width || out->hot_y >= out->height) return false;

    return true;
}
