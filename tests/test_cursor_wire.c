/* test_cursor_wire.c - the three rules of the cursor channel `:base+20`.
 *
 * Each check names its COUNTER-CASE: the exact input that cost something for
 * real. A failure here therefore says what has just been undone.
 *
 * The vectors marked REAL are copied byte for byte from a capture of the
 * official client (`captures/scenario_presse-papier_*`, a 4142 B frame whose
 * alpha channel draws the Windows arrow).
 */
#include "../core/protocol/cursor_wire.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* REAL — the header of a cursor frame, from an official capture. */
static const uint8_t ENTETE_REELLE[10] = {
    0x12, 0x01, 0x24, 0x10, 0x00, 0x00, 0x60, 0xf6, 0xa2, 0x01
};

/* REAL - the picture header of the same frame: 4096 bytes, BGRA, 32x32, stride
 * 128. */
static const uint8_t REAL_IMAGE[36] = {
    0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

static void header(void)
{
    cursor_wire_header_t e;

    CHECK(cursor_wire_parse_header(ENTETE_REELLE, sizeof ENTETE_REELLE, CURSOR_WIRE_CAP_DEFAULT, &e),
            "the real header must parse");
    CHECK(e.type == CURSOR_WIRE_TYPE_IMAGE, "type = 0x12 (picture)");
    CHECK(e.seq == 1, "sequence number");
    CHECK(e.payload_len == 4132, "announced payload length = 4132");
    CHECK(e.id == 27457120u, "frame identifier");

    /* COUNTER-CASE C1 - THE CUT FRAME.
     * The previous parser computed `length = (read - position) - 10`, hence "one
     * read = one frame". A 4142-byte frame was observed arriving in TWO pieces
     * (1024 then 3118): it was then read as two frames, one truncated, the other
     * starting in the middle of the pixels. The size is read from the header,
     * never from the read. */
    CHECK(e.frame_len == 4142,
            "COUNTER-CASE: the total size comes from the header (4132 + 10), "
            "not from the size of the read");

    /* The same header, received with only 1024 bytes available: it must still
     * parse, and say that 4142 are needed in total. */
    cursor_wire_header_t partiel;
    CHECK(cursor_wire_parse_header(ENTETE_REELLE, 1024, CURSOR_WIRE_CAP_DEFAULT, &partiel),
            "COUNTER-CASE: a complete header parses even when the frame is not");
    CHECK(partiel.frame_len == 4142,
            "COUNTER-CASE: it then says how many bytes to expect in total");

    /* Bounds: an incomplete header must return nothing. */
    CHECK(!cursor_wire_parse_header(ENTETE_REELLE, 9, CURSOR_WIRE_CAP_DEFAULT, &e),
            "9 bytes are not enough for a 10-byte header");
    CHECK(!cursor_wire_parse_header(ENTETE_REELLE, 0, CURSOR_WIRE_CAP_DEFAULT, &e),
            "zero bytes are not enough");
    CHECK(!cursor_wire_parse_header(NULL, 10, CURSOR_WIRE_CAP_DEFAULT, &e), "a null pointer is refused");
    CHECK(!cursor_wire_parse_header(ENTETE_REELLE, 10, CURSOR_WIRE_CAP_DEFAULT, NULL), "a null output is refused");

    /* Type 0x02, observed with a zero length. */
    const uint8_t empty[10] = { 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 1, 0, 0, 0 };
    CHECK(cursor_wire_parse_header(empty, sizeof empty, CURSOR_WIRE_CAP_DEFAULT, &e), "type 0x02 parses");
    CHECK(e.type == CURSOR_WIRE_TYPE_EMPTY && e.payload_len == 0
            && e.frame_len == 10,
            "type 0x02: zero length, the frame is its header alone");

    /* COUNTER-CASE K15c - THE LENGTH IS A u32, AND NO CAPTURE COULD SAY SO.
     *
     * Across ~3900 STFP frames drawn from 6.9 GB of captures, bytes 4-5 are zero
     * WITHOUT EXCEPTION, and only four sizes exist: 10, 46, 302, 4142. The
     * largest is structural (10 + 36 + 4096 pixels of 32x32 BGRA), i.e. 6.3 % of
     * a u16's ceiling. The u16 reading and the u32 reading therefore return the
     * SAME value on 100 % of the bytes we own: the corpus could not settle it,
     * and its silence was taken for proof.
     *
     * The disassembly settles it: `mov 0x2(%rsi),%eax` is a 32-bit read; a u16
     * would have been encoded `movzwl`.
     *
     * So this case builds what the wire never showed and what a 1080p key frame
     * on a TCP video channel will produce immediately. Under the old u16
     * reading, `payload_len` was 0 and the parser desynchronised permanently -
     * without an error, as always here. */
    const uint8_t grande[10] = { 0x12, 0x01, 0x00, 0x00, 0x03, 0x00, 1, 0, 0, 0 };
    CHECK(cursor_wire_parse_header(grande, sizeof grande, CURSOR_WIRE_CAP_DEFAULT, &e),
            "K15c: a header with a 196,608-byte payload");
    CHECK(e.payload_len == 196608u,
            "K15c: the length is read as u32 (the u16 reading returned 0)");
    CHECK(e.frame_len == 10u + 196608u,
            "K15c: frame = header + payload");

    /* === K15d 2026-08-29 - THE TCP VIDEO FRAME, TAKEN OFF THE WIRE ===
     * The first bytes of a `0x22` frame from the repo's first capture in the
     * `reliability` profile. This case pins two things nothing proved before:
     *
     *   1. the SAME framing carries the video and the cursor - the module is not
     *      "cursor-specific", despite its name;
     *   2. the TCP video payload starts with the SAME `VideoFrame` header as the
     *      UDP one (`[0x02][timestamp u32][flags]`, six bytes) followed by the
     *      Annex-B start code. The STFP header STACKS on top of it, it does not
     *      replace it.
     *
     * That second point is what unlocked the port: we feared a variable-size
     * sub-header, and there is none. If this test ever fails, the layout has
     * changed and the emission path can no longer be shared with the UDP one. */
    const uint8_t video_tcp[10] = { 0x22, 0x00, 0xe3, 0x1c, 0x00, 0x00, 1, 0, 0, 0 };
    CHECK(cursor_wire_parse_header(video_tcp, sizeof video_tcp, CURSOR_WIRE_CAP_DEFAULT, &e),
            "K15d: the header of a TCP video frame");
    CHECK(e.type == 0x22, "K15d: type 0x22 = video data");
    CHECK(e.payload_len == 7395u && e.frame_len == 7405u,
            "K15d: payload 7395 B, frame 7405 - the measured block size");

    /* The first exact value where the two readings diverge. */
    const uint8_t limite[10] = { 0x12, 0x01, 0x00, 0x00, 0x01, 0x00, 1, 0, 0, 0 };
    CHECK(cursor_wire_parse_header(limite, sizeof limite, CURSOR_WIRE_CAP_DEFAULT, &e)
            && e.payload_len == 65536u,
            "K15c: 65,536, the first length a u16 cannot carry");

    /* === K16a 2026-08-29 - THE VERSION AND THE CEILING ===
     *
     * COUNTER-CASE - A TLS ALERT READ AS A HEADER. These are the first ten bytes
     * of a TLS 1.2 alert record. With no version guard, the old parser accepted
     * them and announced a payload of 33,685,507 bytes - which the caller, since
     * K15g, tries to ACCOMMODATE by growing its buffer. The module was the only
     * rampart and was not one.
     *
     * The guard is the official client's: the LOW nibble of byte 0 in {1, 2}.
     * Here 0x15 & 0x0F = 5. */
    const uint8_t alerte_tls[10] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x0a, 0, 0, 0 };
    CHECK(!cursor_wire_parse_header(alerte_tls, sizeof alerte_tls,
                                      CURSOR_WIRE_CAP_DEFAULT, &e),
            "COUNTER-CASE: the start of a TLS alert is not an STFP header");

    /* Every version outside {1,2} is refused, and both valid ones accepted -
     * checked over all sixteen nibbles rather than on a chosen sample, because
     * it is a property, not a case. */
    {
        int refusal = 0, accepted = 0;
        for (unsigned v = 0; v < 16; v++) {
            uint8_t t[10] = { 0 };
            t[0] = (uint8_t)(0x20u | v);          /* classe 2, version v */
            t[2] = 0x10;                          /* charge 16 octets */
            if (cursor_wire_parse_header(t, sizeof t, CURSOR_WIRE_CAP_DEFAULT, &e))
                accepted++;
            else
                refusal++;
        }
        CHECK(accepted == 2 && refusal == 14,
                "K16a: exactly two framing versions are accepted");
    }

    /* COUNTER-CASE - THE MAXIMUM LENGTH. `0xFFFFFFFF` must be REFUSED, and
     * refused BEFORE computing `frame_len`: on a target where `size_t` is 32
     * bits, `10 + 0xFFFFFFFF` overflows and returns 9. The caller would believe
     * it held a complete nine-byte frame and would restart in the middle of the
     * data. */
    const uint8_t enorme[10] = { 0x12, 0x01, 0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0 };
    CHECK(!cursor_wire_parse_header(enorme, sizeof enorme,
                                      CURSOR_WIRE_CAP_DEFAULT, &e),
            "COUNTER-CASE: a length of 4 GB is refused, never truncated");

    /* The ceiling is THE CALLER'S, and it is exact to the byte: the caller is
     * the one that knows what its channel can carry. The bound is <=, not <: a
     * frame of exactly the ceiling's size is legitimate. */
    {
        const uint8_t t[10] = { 0x12, 0x01, 0x00, 0x00, 0x01, 0x00, 1, 0, 0, 0 };
        CHECK(cursor_wire_parse_header(t, sizeof t, 65536u, &e),
                "K16a: a payload equal to the ceiling goes through");
        CHECK(!cursor_wire_parse_header(t, sizeof t, 65535u, &e),
                "K16a: one byte more than the ceiling is refused");
    }

    /* THE MEASURED KEY FRAME must fit under the default ceiling. That is the
     * case that matters: 213,598 bytes taken off the wire (the reliability
     * capture). A ceiling chosen too low would throw away the session's only key
     * frame - hence exactly the failure this port has just fixed. */
    {
        const uint8_t t[10] = { 0x12, 0x00, 0x5e, 0x42, 0x03, 0x00, 1, 0, 0, 0 };
        CHECK(cursor_wire_parse_header(t, sizeof t, CURSOR_WIRE_CAP_DEFAULT, &e)
                && e.payload_len == 213598u,
                "K16a: the measured key frame (213,598 B) fits under the default ceiling");
    }
}

static void picture(void)
{
    /* A complete payload: the picture header plus the announced pixels. */
    static uint8_t charge[36 + 4096];
    memcpy(charge, REAL_IMAGE, sizeof REAL_IMAGE);

    cursor_wire_image_t im;
    CHECK(cursor_wire_parse_image(charge, sizeof charge, &im),
            "the real picture header must parse");
    CHECK(im.image_size == 4096, "4096 bytes of pixels");
    CHECK(im.format == CURSOR_WIRE_FMT_BGRA, "format 2 = BGRA 32 bits");
    CHECK(im.width == 32 && im.height == 32, "32 x 32");
    CHECK(im.stride == 128, "stride = 128 = 32 pixels x 4 bytes");
    CHECK(im.hot_x == 0 && im.hot_y == 0, "hot spot at the corner");
    CHECK(im.offset_pixels == 36, "the pixels start after the 36-byte header");
    CHECK(!im.hidden, "a 4096-byte picture is not a hidden cursor");

    /* COUNTER-CASE C2 - THE HIDDEN CURSOR.
     * 21 frames out of 140 carry `image_size == 0`. That is not a corrupt frame:
     * it is the server saying "hide the pointer" (a text field, a game capturing
     * the mouse). Rejecting it would deprive the caller of the only information
     * it carries. */
    uint8_t cache[36];
    memcpy(cache, REAL_IMAGE, sizeof cache);
    memset(cache + 8, 0, 4);                 /* image_size = 0 */
    CHECK(cursor_wire_parse_image(cache, sizeof cache, &im),
            "COUNTER-CASE: image_size = 0 must be ACCEPTED");
    CHECK(im.hidden,
            "COUNTER-CASE: and reported as a hidden cursor, not as an error");

    /* COUNTER-CASE C3 - THE DIMENSIONS COME FROM THE NETWORK.
     * Each of these frames is well formed as far as the framing goes: it is its
     * picture header that lies. Accepting them would read pixels outside the
     * received buffer. */
    uint8_t bad[36 + 4096];

    memcpy(bad, REAL_IMAGE, 36);
    CHECK(!cursor_wire_parse_image(bad, 36 + 4095, &im),
            "COUNTER-CASE: 4096 bytes announced, 4095 received - refused");

    memcpy(bad, REAL_IMAGE, 36);
    bad[16] = 0x00; bad[17] = 0x10;        /* width = 4096 */
    CHECK(!cursor_wire_parse_image(bad, sizeof bad, &im),
            "CONTRE-CAS : width aberrante (4096) — refus");

    memcpy(bad, REAL_IMAGE, 36);
    bad[16] = 0; bad[17] = 0; bad[18] = 0; bad[19] = 0;   /* width = 0 */
    CHECK(!cursor_wire_parse_image(bad, sizeof bad, &im),
            "CONTRE-CAS : width nulle — refus");

    memcpy(bad, REAL_IMAGE, 36);
    bad[24] = 4;                            /* stride = 4, too short for 32 BGRA */
    CHECK(!cursor_wire_parse_image(bad, sizeof bad, &im),
            "COUNTER-CASE: a stride shorter than a row would read outside the buffer");

    memcpy(bad, REAL_IMAGE, 36);
    bad[12] = 9;                            /* format inconnu */
    CHECK(!cursor_wire_parse_image(bad, sizeof bad, &im),
            "COUNTER-CASE: an unknown format - we do not guess the layout");

    memcpy(bad, REAL_IMAGE, 36);
    bad[28] = 32;                           /* point chaud x = 32, hors picture */
    CHECK(!cursor_wire_parse_image(bad, sizeof bad, &im),
            "COUNTER-CASE: a hot spot outside the picture - the cursor would be offset");

    memcpy(bad, REAL_IMAGE, 36);
    bad[8] = 0x00; bad[9] = 0x20;          /* image_size = 8192, geometry unchanged */
    CHECK(!cursor_wire_parse_image(bad, sizeof bad, &im),
            "COUNTER-CASE: an announced size inconsistent with the geometry - refused");

    /* The monochrome-mask format: two planes (AND then XOR). */
    uint8_t mono[36 + 256];
    memcpy(mono, REAL_IMAGE, 36);
    mono[12] = CURSOR_WIRE_FMT_MASKS;
    mono[24] = 4; mono[25] = 0; mono[26] = 0; mono[27] = 0;   /* pas = 32/8 */
    mono[8] = 0; mono[9] = 1; mono[10] = 0; mono[11] = 0;     /* 4 * 32 * 2 = 256 */
    CHECK(cursor_wire_parse_image(mono, sizeof mono, &im),
            "the mask format: 4 bytes per row, two planes");
    CHECK(im.format == CURSOR_WIRE_FMT_MASKS && im.image_size == 256,
            "the mask format: size = stride x height x 2");

    /* Bornes elementaires. */
    CHECK(!cursor_wire_parse_image(charge, 35, &im),
            "35 bytes are not enough for a 36-byte picture header");
    CHECK(!cursor_wire_parse_image(NULL, 100, &im), "a null pointer is refused");
    CHECK(!cursor_wire_parse_image(charge, 100, NULL), "a null output is refused");
}

int main(void)
{
    printf("== cursor channel :base+20 (framing and picture header) ==\n");
    header();
    picture();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
