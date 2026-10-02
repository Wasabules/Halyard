/* clip_wire.c - CLIP 2026-10-02. See clip_wire.h for the why behind each rule.
 *
 * PURE: no global state, no I/O, no `getenv`. Even the byte order is done by
 * hand rather than through `<arpa/inet.h>` or `sockets_compat.h`: `ntohs` is
 * not a pure function on every target this repo builds for (on Windows it
 * lives in ws2_32 and needs WSAStartup), and a test that has to link winsock to
 * check a shift is a test that will one day be dropped from the offline suite.
 * `sockets_compat.h` belongs to the plumbing, not to the codec. */

#include "clip_wire.h"

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8)
         | ((uint32_t)p[3]);
}

static void write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v & 0xFFu);
}

bool clip_wire_parse_header(const uint8_t *p, size_t n, uint32_t cap,
                            bool strict, clip_wire_header_t *out)
{
    if (!p || !out || n < CLIP_WIRE_HEADER_LEN) return false;

    const uint16_t reserved = read_u16_be(p + 0);
    const uint16_t opcode   = read_u16_be(p + 2);
    const uint16_t format   = read_u16_be(p + 4);
    const uint32_t len      = read_u32_be(p + 6);

    if (strict) {
        /* === THE TWO GUARDS THE SERVER DOES NOT APPLY =======================
         *
         * `StfpClient::DealWithInput` @0x140c2ecb0 reads bytes 0..1 and 4..5
         * and then DISCARDS both results - the `ntohs` return values are never
         * used. The official client is the strict one: `bodySize()` @0xabd460
         * opens with `cmp WORD PTR [rsi],0x0 / jne -> return -1`, and the
         * receive handler @0x768080 refuses any format but TEXT with "Invalid
         * clipboard format".
         *
         * We take the CLIENT's strictness, not the server's laxity, and the
         * reason is the same one that cost K16a a session: on this channel a
         * header is ten bytes of which eight are normally zero, so almost
         * anything parses. The first ten bytes of a TLS alert
         * (`15 03 03 00 02 ...`) are a valid-looking header announcing a
         * 33 MB payload - unless `reserved` is checked, which is exactly why
         * the client checks it first and before the length. */
        if (reserved != 0u) return false;
        if (format != CLIP_FMT_TEXT) return false;
    }

    /* The cap BEFORE `msg_len`, on the 32-bit value: `10 + 0xFFFFFFFF` wraps
     * to 9 where `size_t` is 32 bits (the Vita), and the caller would then
     * believe it held a complete nine-byte message and resume in the middle of
     * the payload. Rejecting first makes that arithmetic unreachable. */
    if (len > cap) return false;

    out->reserved    = reserved;
    out->opcode      = opcode;
    out->format      = format;
    out->payload_len = len;
    out->msg_len     = (size_t)CLIP_WIRE_HEADER_LEN + (size_t)len;
    return true;
}

bool clip_wire_opcode_known(uint16_t opcode)
{
    return opcode <= CLIP_OP_REPLY_TEXT;
}

const char *clip_wire_opcode_name(uint16_t opcode)
{
    /* The server's own strings, copied from the table `StfpClient::DealWithInput`
     * builds before logging "process message 0x%x (%s with payloadSize=%u)". */
    switch (opcode) {
        case CLIP_OP_CONNECT:    return "MESSAGE_TYPE_CONNECT";
        case CLIP_OP_FLUSH:      return "MESSAGE_TYPE_FLUSH";
        case CLIP_OP_UPDATE:     return "MESSAGE_TYPE_UPDATE";
        case CLIP_OP_REQUEST:    return "MESSAGE_TYPE_REQUEST";
        case CLIP_OP_REPLY_TEXT: return "MESSAGE_TYPE_REPLY";
        default:                 return "unknown";
    }
}

size_t clip_wire_build_header(uint8_t *out, size_t out_cap,
                              uint16_t opcode, uint16_t format)
{
    if (!out || out_cap < CLIP_WIRE_HEADER_LEN) return 0;

    write_u16_be(out + 0, 0u);        /* reserved: always zero, both sides */
    write_u16_be(out + 2, opcode);
    write_u16_be(out + 4, format);
    write_u32_be(out + 6, 0u);        /* no payload */
    return CLIP_WIRE_HEADER_LEN;
}

size_t clip_wire_build_reply_text(uint8_t *out, size_t out_cap, uint32_t cap,
                                  const char *text, size_t text_len)
{
    if (!out) return 0;
    if (!text && text_len != 0) return 0;

    /* Refuse on the 32-bit value before any addition, for the reason given in
     * the parser. `text_len` is a `size_t`, so compare in the wider type and
     * only then narrow. */
    if (text_len > (size_t)cap) return 0;
    if (out_cap < (size_t)CLIP_WIRE_HEADER_LEN + text_len) return 0;

    write_u16_be(out + 0, 0u);
    write_u16_be(out + 2, (uint16_t)CLIP_OP_REPLY_TEXT);
    write_u16_be(out + 4, (uint16_t)CLIP_FMT_TEXT);
    write_u32_be(out + 6, (uint32_t)text_len);

    for (size_t i = 0; i < text_len; i++)
        out[CLIP_WIRE_HEADER_LEN + i] = (uint8_t)text[i];

    return (size_t)CLIP_WIRE_HEADER_LEN + text_len;
}

/* The length of the UTF-8 sequence a lead byte opens, or 0 if the byte cannot
 * be a lead byte. Continuation bytes (0x80..0xBF), 0xC0/0xC1 (overlong by
 * construction) and 0xF5..0xFF (above U+10FFFF) all return 0. */
static unsigned utf8_seq_len(uint8_t b)
{
    if (b < 0x80u) return 1;
    if (b < 0xC2u) return 0;
    if (b < 0xE0u) return 2;
    if (b < 0xF0u) return 3;
    if (b < 0xF5u) return 4;
    return 0;
}

bool clip_wire_utf8_valid(const uint8_t *p, size_t n)
{
    if (!p) return n == 0;

    size_t i = 0;
    while (i < n) {
        const unsigned need = utf8_seq_len(p[i]);
        if (need == 0) return false;
        if (n - i < need) return false;          /* truncated at the end */

        for (unsigned k = 1; k < need; k++)
            if ((p[i + k] & 0xC0u) != 0x80u) return false;

        /* The two ranges a length check alone cannot catch, and both of which a
         * naive validator lets through: the UTF-16 surrogates U+D800..U+DFFF,
         * which have no UTF-8 encoding at all, and the overlong three- and
         * four-byte forms. They matter here because the VM re-encodes our bytes
         * to UTF-16 and a surrogate half would become U+FFFD - our text would
         * come back changed through a path that reports nothing. */
        if (need == 3) {
            const uint32_t cp = ((uint32_t)(p[i] & 0x0Fu) << 12)
                              | ((uint32_t)(p[i + 1] & 0x3Fu) << 6)
                              |  (uint32_t)(p[i + 2] & 0x3Fu);
            if (cp < 0x800u) return false;                      /* overlong */
            if (cp >= 0xD800u && cp <= 0xDFFFu) return false;   /* surrogate */
        } else if (need == 4) {
            const uint32_t cp = ((uint32_t)(p[i] & 0x07u) << 18)
                              | ((uint32_t)(p[i + 1] & 0x3Fu) << 12)
                              | ((uint32_t)(p[i + 2] & 0x3Fu) << 6)
                              |  (uint32_t)(p[i + 3] & 0x3Fu);
            if (cp < 0x10000u) return false;                    /* overlong */
            if (cp > 0x10FFFFu) return false;
        }

        i += need;
    }
    return true;
}

size_t clip_wire_utf8_trim_len(const uint8_t *p, size_t n, size_t limit)
{
    if (!p) return 0;
    if (n <= limit) return n;

    /* Walk back from `limit` to the first byte that is not a continuation byte.
     * At most three steps: no UTF-8 sequence is longer than four bytes. If that
     * byte opens a sequence that would not fit, cut before it instead. */
    size_t cut = limit;
    size_t back = 0;
    while (cut > 0 && (p[cut] & 0xC0u) == 0x80u && back < 3) {
        cut--;
        back++;
    }

    const unsigned need = utf8_seq_len(p[cut]);
    if (need == 0) return cut;                 /* malformed: cut here, no guess */
    if (cut + need <= limit) return cut + need; /* the sequence fits whole */
    return cut;
}
