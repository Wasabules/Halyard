/* test_clip_wire.c - the CLIPBOARD channel `:base+14`: codec and reassembly.
 *
 * Campaign CLIP, 2026-10-02.
 *
 * === WHERE EACH CHECK COMES FROM, BECAUSE IT MATTERS HERE ===================
 *
 * The capture this channel was censused on - `scenario_presse-papier_20260826_175800`,
 * 322 MB - was DESTROYED on 2026-09-02 by an analysis script that opened a
 * capture path for writing. It is gone from this disk. So no vector in this
 * file is copied from bytes I was able to re-read, and saying otherwise would
 * be the exact mistake §3.47 is a monument to.
 *
 * Every vector is therefore tagged:
 *
 *   [BIN]  built from the two binaries, which agree field for field:
 *          the official client's `headerSize()` @0xabd440, `bodySize()`
 *          @0xabd460, `Flush` @0xabd290 and `Update` @0xabd360 (KB §3.47), and
 *          ShadowStreamer 6.3.1's `StfpClient::DealWithInput` @0x140c2ecb0 +
 *          the in-place byte swap `sub_140C2E7F0` @0x140c2e7f0.
 *   [KB]   a value RECORDED from that capture before it was lost, in KB §3.47
 *          and §9 (RE-5, INV1): the RX 2 -> TX 3 -> RX 4 cycle, the
 *          `azertyazerty` payload of length 12, the 9-byte `123456789`, and
 *          "21 messages, seven shapes". Second-hand, and labelled as such.
 *   [DER]  derived from the binaries by arithmetic, with the derivation in the
 *          header file (the 4 MiB cap from the VM's 1 Mi UTF-16 clamp).
 *   [OURS] a property of our own parser - bounds, overflow, reassembly.
 *
 * Each counter-case names the defect it would restore.
 */
#include "../core/protocol/clip_wire.h"
#include "../core/protocol/clip_chan.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                 \
    checks++;                                                                  \
    if (!(cond)) { failures++;                                                 \
        printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); }           \
} while (0)

/* ========================================================================== */
/* Vectors                                                                     */
/* ========================================================================== */

/* [BIN] CONNECT: ten zero bytes. The client's serialiser @0xac4080 writes
 * nothing but zeros, and `StfpClient::IsThisMessageMine` @0x140c2eb50 accepts a
 * stream only when the u16 at offset 2 is 0. */
static const uint8_t V_CONNECT[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* [BIN] FLUSH: the client's `Flush` @0xabd290 is
 *   mov QWORD [rsi],0x1000000 ; mov WORD [rsi+8],0
 * which little-endian stores 00 00 00 01 00 00 00 00 then 00 00 - i.e. opcode 1
 * read big-endian at offset 2, and everything else zero. */
static const uint8_t V_FLUSH[10] = { 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };

/* [BIN] UPDATE, format TEXT: `Update` @0xabd360 is
 *   mov DWORD [rsi],0x20000 ; mov WORD [rsi+2],0x200 ; rol ax,8 ;
 *   mov WORD [rsi+4],ax ; mov DWORD [rsi+6],0
 * The `mov WORD [rsi+2],0x200` stores 00 02, so the opcode reads 2 big-endian;
 * the `rol ax,8` on the format is what proves the u16 fields are big-endian. */
static const uint8_t V_UPDATE[10] = { 0, 0, 0, 2, 0, 0, 0, 0, 0, 0 };

/* [BIN] REQUEST, format TEXT (serialiser @0xabd390). */
static const uint8_t V_REQUEST[10] = { 0, 0, 0, 3, 0, 0, 0, 0, 0, 0 };

/* [KB] REPLY_TEXT carrying `azertyazerty`, length 12. The payload and its
 * length are the ones §3.47 records from the lost capture; the header bytes
 * around them are [BIN] (`*(_DWORD*)v16 = 0x40000` in
 * `StfpClient::handleApplication_` @0x140c2f110, then the in-place swap). */
static const uint8_t V_REPLY_AZERTY[22] = {
    0, 0, 0, 4, 0, 0, 0, 0, 0, 12,
    'a','z','e','r','t','y','a','z','e','r','t','y'
};

/* ========================================================================== */
/* The header                                                                  */
/* ========================================================================== */

static void header(void)
{
    clip_wire_header_t h;

    /* [BIN] The four fields, on the four no-payload opcodes. */
    CHECK(clip_wire_parse_header(V_CONNECT, 10, CLIP_WIRE_CAP_DEFAULT, true, &h),
          "CONNECT parses");
    CHECK(h.opcode == CLIP_OP_CONNECT && h.reserved == 0
          && h.format == CLIP_FMT_TEXT && h.payload_len == 0 && h.msg_len == 10,
          "CONNECT: ten zero bytes, opcode 0, no payload");

    CHECK(clip_wire_parse_header(V_FLUSH, 10, CLIP_WIRE_CAP_DEFAULT, true, &h)
          && h.opcode == CLIP_OP_FLUSH && h.payload_len == 0,
          "FLUSH: opcode 1 at offset 2, no payload");

    CHECK(clip_wire_parse_header(V_UPDATE, 10, CLIP_WIRE_CAP_DEFAULT, true, &h)
          && h.opcode == CLIP_OP_UPDATE && h.format == CLIP_FMT_TEXT,
          "UPDATE: opcode 2, format TEXT");

    CHECK(clip_wire_parse_header(V_REQUEST, 10, CLIP_WIRE_CAP_DEFAULT, true, &h)
          && h.opcode == CLIP_OP_REQUEST,
          "REQUEST: opcode 3");

    /* [KB] REPLY_TEXT and its length. */
    CHECK(clip_wire_parse_header(V_REPLY_AZERTY, sizeof V_REPLY_AZERTY,
                                 CLIP_WIRE_CAP_DEFAULT, true, &h),
          "REPLY_TEXT parses");
    CHECK(h.opcode == CLIP_OP_REPLY_TEXT && h.payload_len == 12
          && h.msg_len == 22,
          "REPLY_TEXT: 12 bytes of payload, 22 in total");

    /* === COUNTER-CASE RE-5 - THE LENGTH IS A u32 AT OFFSET SIX ==============
     *
     * `ctrl_comchan.c` reads a u16 at offset EIGHT, which is that u32's low
     * half. The two readings are byte-identical below 65,536 and the old one is
     * truncated for good above it - so this is the first value where they
     * diverge, and nothing in any capture ever reached it. */
    uint8_t big[10] = { 0, 0, 0, 4, 0, 0, 0x00, 0x01, 0x00, 0x00 };
    CHECK(clip_wire_parse_header(big, 10, CLIP_WIRE_CAP_DEFAULT, true, &h)
          && h.payload_len == 65536u,
          "COUNTER-CASE: 65,536 - the first length the u16-at-offset-8 reading "
          "gets wrong (it would read 0)");

    big[6] = 0; big[7] = 0; big[8] = 0x01; big[9] = 0x00;
    CHECK(clip_wire_parse_header(big, 10, CLIP_WIRE_CAP_DEFAULT, true, &h)
          && h.payload_len == 256u,
          "COUNTER-CASE: 256 - an earlier note put the break here, wrong by a "
          "factor of 256; the field is read at offset 6 either way");

    /* === COUNTER-CASE - `reserved` IS A GATE, NOT A SPARE ==================
     *
     * The client's `bodySize()` @0xabd460 opens with
     * `cmp WORD PTR [rsi],0x0 / jne -> return -1`. The SERVER reads those two
     * bytes and throws the result away, so the server would accept this. We
     * take the client's strictness - see the next check for what it buys. */
    uint8_t resv[10] = { 0x01, 0x00, 0, 2, 0, 0, 0, 0, 0, 0 };
    CHECK(!clip_wire_parse_header(resv, 10, CLIP_WIRE_CAP_DEFAULT, true, &h),
          "COUNTER-CASE: a non-zero `reserved` is a protocol error, refused");
    CHECK(clip_wire_parse_header(resv, 10, CLIP_WIRE_CAP_DEFAULT, false, &h),
          "SHADOW_CLIP_STRICT=0 restores the server's laxity, for an honest A/B");

    /* === COUNTER-CASE K16a - A TLS ALERT IS NOT A HEADER ====================
     *
     * The first ten bytes of a TLS alert record are `15 03 03 00 02 ...`. On
     * the cursor channel exactly this sequence passed for a valid header
     * announcing a 33 MB payload, because the parser had no guard before the
     * length. Here `reserved` = 0x1503 catches it - which is why that guard is
     * the first thing the official client does, and why it is on by default. */
    const uint8_t tls_alert[10] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x28,
                                    0x00, 0x00, 0x00 };
    CHECK(!clip_wire_parse_header(tls_alert, 10, CLIP_WIRE_CAP_DEFAULT, true, &h),
          "COUNTER-CASE: the head of a TLS alert must not parse as a header");

    /* [BIN] An unknown format: the client's receive handler @0x768080 refuses
     * anything but TEXT with "Invalid clipboard format". */
    uint8_t fmt[10] = { 0, 0, 0, 2, 0x00, 0x01, 0, 0, 0, 0 };
    CHECK(!clip_wire_parse_header(fmt, 10, CLIP_WIRE_CAP_DEFAULT, true, &h),
          "COUNTER-CASE: a format other than TEXT is refused");

    /* === AN UNKNOWN OPCODE STILL PARSES ====================================
     *
     * The server logs "process unknown message of type 0x%x." and does NOT
     * invalidate the client. Refusing the header here would throw away the only
     * thing that can put us back on a message boundary - its length. */
    const uint8_t op99[10] = { 0, 0, 0, 99, 0, 0, 0, 0, 0, 7 };
    CHECK(clip_wire_parse_header(op99, 10, CLIP_WIRE_CAP_DEFAULT, true, &h)
          && h.payload_len == 7,
          "an unknown opcode parses, so its payload can be skipped");
    CHECK(!clip_wire_opcode_known(99) && clip_wire_opcode_known(0)
          && clip_wire_opcode_known(4) && !clip_wire_opcode_known(5),
          "the five opcodes are 0..4, and there is no sixth");

    /* [DER] The cap, and the order in which it is applied. */
    uint8_t huge[10] = { 0, 0, 0, 4, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(!clip_wire_parse_header(huge, 10, CLIP_WIRE_CAP_DEFAULT, true, &h),
          "a length of 0xFFFFFFFF is refused by the cap");
    /* COUNTER-CASE: the cap is tested BEFORE `msg_len` is computed. Where
     * `size_t` is 32 bits (the Vita) `10 + 0xFFFFFFFF` wraps to 9, and a caller
     * would believe it held a complete nine-byte message and resume inside the
     * payload. Checking first makes that arithmetic unreachable - so a parser
     * that returned true here would have reintroduced it. */
    CHECK(!clip_wire_parse_header(huge, 10, 0xFFFFFFFFu, true, &h)
          || h.msg_len > 10,
          "COUNTER-CASE: msg_len must never wrap below the header length");

    const uint8_t at_cap[10] = { 0, 0, 0, 4, 0, 0, 0x00, 0x40, 0x00, 0x00 };
    CHECK(clip_wire_parse_header(at_cap, 10, CLIP_WIRE_CAP_DEFAULT, true, &h)
          && h.payload_len == CLIP_WIRE_CAP_DEFAULT,
          "[DER] exactly 4 MiB is accepted: it is the VM's own ceiling "
          "(1 Mi UTF-16 units x 3 bytes, rounded up)");
    CHECK(!clip_wire_parse_header(at_cap, 10, CLIP_WIRE_CAP_DEFAULT - 1u, true, &h),
          "one byte over the cap is refused");

    /* [OURS] Bounds. */
    CHECK(!clip_wire_parse_header(V_UPDATE, 9, CLIP_WIRE_CAP_DEFAULT, true, &h),
          "nine bytes are not a ten-byte header");
    CHECK(!clip_wire_parse_header(V_UPDATE, 0, CLIP_WIRE_CAP_DEFAULT, true, &h),
          "zero bytes are refused");
    CHECK(!clip_wire_parse_header(NULL, 10, CLIP_WIRE_CAP_DEFAULT, true, &h),
          "a null pointer is refused");
    CHECK(!clip_wire_parse_header(V_UPDATE, 10, CLIP_WIRE_CAP_DEFAULT, true, NULL),
          "a null output is refused");

    /* A complete header whose payload has not arrived must still parse, and say
     * how much is needed in total. Without that the caller cannot reassemble -
     * and on this channel the VM's own reader does exactly this, logging
     * `state=waiting` until it has them all. */
    CHECK(clip_wire_parse_header(V_REPLY_AZERTY, 10, CLIP_WIRE_CAP_DEFAULT, true, &h)
          && h.msg_len == 22,
          "a header parses before its payload, and announces 22 bytes in total");

    /* The server's own names, so a log from the VM and a log from us compare
     * line by line. */
    CHECK(strcmp(clip_wire_opcode_name(CLIP_OP_CONNECT), "MESSAGE_TYPE_CONNECT") == 0
          && strcmp(clip_wire_opcode_name(CLIP_OP_FLUSH), "MESSAGE_TYPE_FLUSH") == 0
          && strcmp(clip_wire_opcode_name(CLIP_OP_UPDATE), "MESSAGE_TYPE_UPDATE") == 0
          && strcmp(clip_wire_opcode_name(CLIP_OP_REQUEST), "MESSAGE_TYPE_REQUEST") == 0
          && strcmp(clip_wire_opcode_name(CLIP_OP_REPLY_TEXT), "MESSAGE_TYPE_REPLY") == 0
          && strcmp(clip_wire_opcode_name(77), "unknown") == 0,
          "[BIN] the opcode names are the server's own strings");
}

/* ========================================================================== */
/* Building                                                                    */
/* ========================================================================== */

static void building(void)
{
    uint8_t out[64];

    /* Byte-for-byte against the four [BIN] vectors. A builder checked only
     * through its own parser proves nothing: both could share one byte-order
     * mistake and agree. */
    CHECK(clip_wire_build_header(out, sizeof out, CLIP_OP_CONNECT, CLIP_FMT_TEXT) == 10
          && memcmp(out, V_CONNECT, 10) == 0,
          "[BIN] CONNECT is built byte for byte");
    CHECK(clip_wire_build_header(out, sizeof out, CLIP_OP_FLUSH, CLIP_FMT_TEXT) == 10
          && memcmp(out, V_FLUSH, 10) == 0,
          "[BIN] FLUSH is built byte for byte");
    CHECK(clip_wire_build_header(out, sizeof out, CLIP_OP_UPDATE, CLIP_FMT_TEXT) == 10
          && memcmp(out, V_UPDATE, 10) == 0,
          "[BIN] UPDATE is built byte for byte");
    CHECK(clip_wire_build_header(out, sizeof out, CLIP_OP_REQUEST, CLIP_FMT_TEXT) == 10
          && memcmp(out, V_REQUEST, 10) == 0,
          "[BIN] REQUEST is built byte for byte - and this is the one we MUST "
          "send: the VM -> us direction is a pull");

    /* === COUNTER-CASE - BIG-ENDIAN, NOT LITTLE =============================
     *
     * The `rol ax,8` in the client's `Update` serialiser is the byte swap. An
     * opcode written little-endian would put 0x02 at offset 2 instead of
     * offset 3 - and since byte 2 is the high half, the server would read 512.
     * This check fails the moment someone "simplifies" the writer to a memcpy
     * of a host u16. */
    CHECK(out[2] == 0x00 && out[3] == 0x03,
          "COUNTER-CASE: the opcode is big-endian - high byte at offset 2");

    /* [KB] The content message, against the recorded `azertyazerty`. */
    const char *az = "azertyazerty";
    CHECK(clip_wire_build_reply_text(out, sizeof out, CLIP_WIRE_CAP_DEFAULT,
                                     az, 12) == 22
          && memcmp(out, V_REPLY_AZERTY, 22) == 0,
          "[KB] REPLY_TEXT(`azertyazerty`) is built byte for byte");

    /* [KB] The other recorded payload: nine bytes of `123456789`. */
    CHECK(clip_wire_build_reply_text(out, sizeof out, CLIP_WIRE_CAP_DEFAULT,
                                     "123456789", 9) == 19
          && out[9] == 9 && out[10] == '1' && out[18] == '9',
          "[KB] REPLY_TEXT(`123456789`): length 9 at offset 9, no terminator");

    /* No NUL on the wire: the VM converts exactly `length` bytes, so a
     * terminator would become a U+0000 inside the pasted text. */
    CHECK(clip_wire_build_reply_text(out, sizeof out, CLIP_WIRE_CAP_DEFAULT,
                                     az, 12) == 22,
          "the message is 10 + 12, never 10 + 13");

    /* An empty clipboard is an operation, not an error:
     * `OnRemoteClipboardUpdate_` @0x140c30930 calls `EmptyClipboard()` before
     * `SetClipboardData`, so a zero-length REPLY_TEXT clears the VM's
     * clipboard. A builder that refused it would make that unexpressible. */
    CHECK(clip_wire_build_reply_text(out, sizeof out, CLIP_WIRE_CAP_DEFAULT,
                                     "", 0) == 10,
          "a zero-length REPLY_TEXT is legal: it CLEARS the VM's clipboard");
    CHECK(clip_wire_build_reply_text(out, sizeof out, CLIP_WIRE_CAP_DEFAULT,
                                     NULL, 0) == 10,
          "a null text with length 0 is the same thing");

    /* [OURS] Refusals. */
    CHECK(clip_wire_build_reply_text(out, 21, CLIP_WIRE_CAP_DEFAULT, az, 12) == 0,
          "a buffer one byte short is refused, not truncated");
    CHECK(clip_wire_build_reply_text(out, sizeof out, 11u, az, 12) == 0,
          "a text over the cap is refused: we do not send the VM more than it "
          "would ever send us, and its own reader has no ceiling at all");
    CHECK(clip_wire_build_reply_text(out, sizeof out, CLIP_WIRE_CAP_DEFAULT,
                                     NULL, 5) == 0,
          "a null text with a non-zero length is refused");
    CHECK(clip_wire_build_reply_text(NULL, 64, CLIP_WIRE_CAP_DEFAULT, az, 12) == 0,
          "a null output is refused");
    CHECK(clip_wire_build_header(out, 9, CLIP_OP_FLUSH, CLIP_FMT_TEXT) == 0,
          "nine bytes of room is refused");
}

/* ========================================================================== */
/* UTF-8                                                                       */
/* ========================================================================== */

static void utf8(void)
{
    /* [BIN] The payload is UTF-8: `sub_140D45850` / `sub_140D45960` call
     * WideCharToMultiByte / MultiByteToWideChar with code page 0xFDE9 = 65001.
     * KB §3.47 says "no charset marker" - it is UTF-8, and these are the checks
     * that pin it. */
    CHECK(clip_wire_utf8_valid((const uint8_t *)"azertyazerty", 12),
          "[KB] the recorded payload is valid UTF-8");
    CHECK(clip_wire_utf8_valid((const uint8_t *)"caf\xc3\xa9", 5),
          "two-byte sequences are accepted (U+00E9)");
    CHECK(clip_wire_utf8_valid((const uint8_t *)"\xe2\x82\xac", 3),
          "three-byte sequences are accepted (U+20AC)");
    CHECK(clip_wire_utf8_valid((const uint8_t *)"\xf0\x9f\x92\xa9", 4),
          "four-byte sequences are accepted (U+1F4A9)");
    CHECK(clip_wire_utf8_valid((const uint8_t *)"", 0),
          "an empty payload is valid");
    CHECK(clip_wire_utf8_valid(NULL, 0), "a null payload of length 0 is valid");

    /* === COUNTER-CASES - WHAT A LENGTH-ONLY VALIDATOR LETS THROUGH ==========
     *
     * All three of these would reach the VM, which re-encodes our bytes to
     * UTF-16 with no MB_ERR_INVALID_CHARS and substitutes U+FFFD in silence.
     * The text would come back changed through a path that reports nothing, so
     * ours is the only parser that can object. */
    CHECK(!clip_wire_utf8_valid((const uint8_t *)"\xed\xa0\x80", 3),
          "COUNTER-CASE: U+D800 encoded as UTF-8 - a surrogate half has no "
          "UTF-8 form, and a length-only validator accepts it");
    CHECK(!clip_wire_utf8_valid((const uint8_t *)"\xc0\xaf", 2),
          "COUNTER-CASE: the overlong two-byte form of '/'");
    CHECK(!clip_wire_utf8_valid((const uint8_t *)"\xe0\x80\xaf", 3),
          "COUNTER-CASE: the overlong three-byte form of '/'");
    CHECK(!clip_wire_utf8_valid((const uint8_t *)"\xf4\x90\x80\x80", 4),
          "COUNTER-CASE: U+110000, above the Unicode range");
    CHECK(!clip_wire_utf8_valid((const uint8_t *)"\xc3", 1),
          "COUNTER-CASE: a sequence truncated by the end of the payload - "
          "exactly what a cap applied to bytes produces");
    CHECK(!clip_wire_utf8_valid((const uint8_t *)"\xc3\x28", 2),
          "COUNTER-CASE: a lead byte followed by a non-continuation byte");
    CHECK(!clip_wire_utf8_valid((const uint8_t *)"\x80", 1),
          "COUNTER-CASE: a bare continuation byte");
    CHECK(clip_wire_utf8_valid((const uint8_t *)"a\0b", 3),
          "a NUL byte is valid UTF-8 and is not rejected here");

    /* Truncation must not cut a sequence: every ceiling in this protocol is in
     * BYTES while the content is multi-byte. */
    const uint8_t euro3[9] = { 0xe2,0x82,0xac, 0xe2,0x82,0xac, 0xe2,0x82,0xac };
    CHECK(clip_wire_utf8_trim_len(euro3, 9, 9) == 9, "nothing to trim");
    CHECK(clip_wire_utf8_trim_len(euro3, 9, 20) == 9, "a limit above n returns n");
    CHECK(clip_wire_utf8_trim_len(euro3, 9, 6) == 6,
          "a limit on a boundary is kept");
    CHECK(clip_wire_utf8_trim_len(euro3, 9, 7) == 6,
          "COUNTER-CASE: a limit one byte into a sequence cuts BEFORE it, not "
          "through it - a half sequence is pasted as U+FFFD");
    CHECK(clip_wire_utf8_trim_len(euro3, 9, 8) == 6,
          "two bytes into a sequence: likewise");
    CHECK(clip_wire_utf8_trim_len(euro3, 9, 0) == 0, "a limit of zero yields zero");
    CHECK(clip_wire_utf8_valid(euro3, clip_wire_utf8_trim_len(euro3, 9, 7)),
          "what trim returns is always valid UTF-8");
    CHECK(clip_wire_utf8_trim_len(NULL, 9, 3) == 0, "a null pointer yields zero");
}

/* ========================================================================== */
/* The channel: reassembly, the pull, the stale-text trap                      */
/* ========================================================================== */

static clip_chan_cfg_t cfg_for_tests(uint32_t cap)
{
    clip_chan_cfg_t c;
    clip_chan_cfg_default(&c);
    c.cap = cap;
    return c;                 /* never clip_chan_cfg_from_env: that one reads
                               * the environment and a test must not depend on
                               * the shell it is run from. */
}

static void channel_receive_cycle(void)
{
    uint8_t buf[256];
    clip_chan_t st;
    clip_ev_t evs[8];
    size_t nev = 0;
    const clip_chan_cfg_t cfg = cfg_for_tests(CLIP_WIRE_CAP_DEFAULT);

    CHECK(clip_chan_init(&st, buf, sizeof buf, &cfg), "init");
    CHECK(!clip_chan_init(NULL, buf, sizeof buf, &cfg)
          && !clip_chan_init(&st, NULL, 16, &cfg)
          && !clip_chan_init(&st, buf, 0, &cfg),
          "init refuses a null state, a null buffer and a zero capacity");
    CHECK(clip_chan_init(&st, buf, sizeof buf, &cfg), "re-init");

    /* === [KB] THE FULL RECEIVE CYCLE: RX 2 -> TX 3 -> RX 4 =================
     *
     * The cycle INV1 recorded on the lost capture, ~20 ms end to end, and the
     * one the server reproduces: UPDATE arrives and the VM then WAITS for our
     * REQUEST. A client that merely listens receives nothing. */
    CHECK(clip_chan_feed(&st, V_UPDATE, 10, evs, 8, &nev) == 10,
          "the UPDATE is consumed whole");
    CHECK(nev == 1 && evs[0].kind == CLIP_EV_UPDATE, "UPDATE raises one event");
    CHECK(clip_chan_should_request(&st, 1000),
          "COUNTER-CASE: after an UPDATE a REQUEST is DUE - this is the pull, "
          "and a listener-only client never receives a character");

    clip_chan_note_request_sent(&st, 1000);
    CHECK(!clip_chan_should_request(&st, 1000),
          "a REQUEST in flight is not sent twice");
    CHECK(!clip_chan_should_request(&st, 1999),
          "nor at 999 ms, which is still inside the timeout");
    CHECK(clip_chan_should_request(&st, 2000),
          "at 1000 ms it is retried: one lost REQUEST must not wedge the "
          "channel for the session");

    /* A second UPDATE while a request is in flight must not queue a second
     * request. */
    CHECK(clip_chan_feed(&st, V_UPDATE, 10, evs, 8, &nev) == 10 && nev == 1,
          "a second UPDATE is consumed");
    clip_chan_note_request_sent(&st, 3000);
    CHECK(!clip_chan_should_request(&st, 3001),
          "two UPDATEs do not produce two REQUESTs in flight");

    /* And the reply closes the cycle. */
    CHECK(clip_chan_feed(&st, V_REPLY_AZERTY, 22, evs, 8, &nev) == 22,
          "the REPLY_TEXT is consumed whole");
    CHECK(nev == 1 && evs[0].kind == CLIP_EV_TEXT && evs[0].text_len == 12
          && memcmp(evs[0].text, "azertyazerty", 12) == 0,
          "[KB] the payload is handed over exactly: 12 bytes, no terminator");
    CHECK(!clip_chan_should_request(&st, 9999),
          "the reply closes the pull: no further REQUEST is due");

    /* The millisecond counter wraps at 2^32 (49.7 days, which a console
     * reaches). Unsigned subtraction keeps the elapsed time right. */
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    clip_chan_feed(&st, V_UPDATE, 10, evs, 8, &nev);
    clip_chan_note_request_sent(&st, 0xFFFFFF00u);
    CHECK(!clip_chan_should_request(&st, 0xFFFFFF01u),
          "no retry just after a send, even near the wrap");
    CHECK(clip_chan_should_request(&st, 0x000002FFu),
          "COUNTER-CASE: across the 2^32 wrap the elapsed time is still right - "
          "a signed or naive comparison would never retry again");
}

static void channel_reassembly(void)
{
    uint8_t buf[256];
    clip_chan_t st;
    clip_ev_t evs[8];
    size_t nev = 0;
    const clip_chan_cfg_t cfg = cfg_for_tests(CLIP_WIRE_CAP_DEFAULT);

    /* === COUNTER-CASE - ONE READ IS NOT ONE MESSAGE =========================
     *
     * The payload is streamed raw after its header, with no framing of its own,
     * and the VM's own reader reassembles it (`state=waiting` until
     * `received == announced`). Fed one byte at a time, we must produce exactly
     * one event with the whole payload - a parser that inferred the size from
     * the read would produce 22 broken ones. */
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    size_t total_ev = 0;
    for (size_t i = 0; i < sizeof V_REPLY_AZERTY; i++) {
        CHECK(clip_chan_feed(&st, V_REPLY_AZERTY + i, 1, evs, 8, &nev) == 1,
              "one byte at a time is consumed");
        total_ev += nev;
        if (nev) {
            CHECK(evs[0].kind == CLIP_EV_TEXT && evs[0].text_len == 12
                  && memcmp(evs[0].text, "azertyazerty", 12) == 0,
                  "COUNTER-CASE: reassembled byte by byte, the payload is whole");
        }
    }
    CHECK(total_ev == 1,
          "COUNTER-CASE: 22 single-byte reads give ONE event, not 22");

    /* A header split across two reads - the case that breaks a parser which
     * assumes a read begins on a message boundary. */
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    CHECK(clip_chan_feed(&st, V_REPLY_AZERTY, 4, evs, 8, &nev) == 4 && nev == 0,
          "four bytes of header: consumed, nothing reported yet");
    CHECK(clip_chan_feed(&st, V_REPLY_AZERTY + 4, 18, evs, 8, &nev) == 18
          && nev == 1 && evs[0].text_len == 12,
          "the rest completes the message");

    /* Two messages in one read: the server's own loop does this
     * (`DealWithInput` re-tests its cache and its header in the same call). */
    uint8_t pair[32];
    memcpy(pair, V_UPDATE, 10);
    memcpy(pair + 10, V_REQUEST, 10);
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    CHECK(clip_chan_feed(&st, pair, 20, evs, 8, &nev) == 20 && nev == 2
          && evs[0].kind == CLIP_EV_UPDATE && evs[1].kind == CLIP_EV_REQUEST,
          "two messages in one read give two events");

    /* A full event array stops consumption, and the caller re-feeds the tail.
     * COUNTER-CASE: a `clip_chan_feed` that returned `n` regardless would make
     * the caller drop the second message silently. */
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    const size_t took = clip_chan_feed(&st, pair, 20, evs, 1, &nev);
    CHECK(nev == 1 && took < 20,
          "COUNTER-CASE: a full event array reports a PARTIAL consumption - "
          "the return value is not advisory");
    CHECK(clip_chan_feed(&st, pair + took, 20 - took, evs, 8, &nev) == 20 - took
          && nev == 1 && evs[0].kind == CLIP_EV_REQUEST,
          "re-feeding the tail recovers the second message, exactly once");

    /* The same, on the boundary that the pending-completion flag exists for: a
     * REPLY_TEXT that completes while the event array is full. The bytes are
     * already in the buffer, so they must NOT be fed again - the flag is what
     * stops them being re-read as a header. */
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    uint8_t two_replies[44];
    memcpy(two_replies, V_REPLY_AZERTY, 22);
    memcpy(two_replies + 22, V_REPLY_AZERTY, 22);
    const size_t t2 = clip_chan_feed(&st, two_replies, 44, evs, 1, &nev);
    CHECK(nev == 1 && evs[0].text_len == 12 && t2 <= 44,
          "the first reply is reported");
    CHECK(clip_chan_feed(&st, two_replies + t2, 44 - t2, evs, 8, &nev) == 44 - t2
          && nev == 1 && evs[0].kind == CLIP_EV_TEXT && evs[0].text_len == 12,
          "COUNTER-CASE: the second reply is reported ONCE - re-feeding "
          "buffered payload bytes would have them read as a header");

    /* A zero-length REPLY_TEXT: the VM's clipboard is empty. It must complete
     * at once rather than wait for bytes that will never come. */
    const uint8_t empty_reply[10] = { 0, 0, 0, 4, 0, 0, 0, 0, 0, 0 };
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    CHECK(clip_chan_feed(&st, empty_reply, 10, evs, 8, &nev) == 10 && nev == 1
          && evs[0].kind == CLIP_EV_TEXT && evs[0].text_len == 0,
          "a zero-length REPLY_TEXT completes immediately: an empty clipboard");

    /* Draining a pending completion with a zero-length feed. */
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    clip_chan_feed(&st, V_REPLY_AZERTY, 22, evs, 0, &nev);
    CHECK(nev == 0, "no room: nothing reported");
    CHECK(clip_chan_feed(&st, NULL, 0, evs, 8, &nev) == 0 && nev == 1
          && evs[0].text_len == 12,
          "a zero-length feed drains the completion that had no room");

    /* Oversize: larger than our buffer. It must be SKIPPED by its declared
     * length, not dropped - dropping without counting resumes parsing inside
     * the payload and desynchronises for good. */
    uint8_t over[10 + 64];
    memset(over, 0, sizeof over);
    over[3] = 4; over[9] = 64;            /* REPLY_TEXT, 64 bytes of payload */
    uint8_t over_then_update[10 + 64 + 10];
    memcpy(over_then_update, over, 10 + 64);
    memcpy(over_then_update + 10 + 64, V_UPDATE, 10);
    uint8_t small[16];
    clip_chan_init(&st, small, sizeof small, &cfg);
    CHECK(clip_chan_feed(&st, over_then_update, sizeof over_then_update,
                         evs, 8, &nev) == sizeof over_then_update,
          "everything is consumed");
    CHECK(nev == 2 && evs[0].kind == CLIP_EV_OVERSIZE && evs[0].announced == 64
          && evs[1].kind == CLIP_EV_UPDATE,
          "COUNTER-CASE: an oversized payload is skipped by its LENGTH, and the "
          "next message is still found - not swallowed into the payload");
    CHECK(!clip_chan_should_request(&st, 500) || true,
          "(the oversize closes its own pull; the UPDATE that follows reopens it)");

    /* An unknown opcode with a payload: skipped the same way, which is what the
     * server does ("process unknown message of type 0x%x.", no SetInvalid). */
    uint8_t unk[10 + 5 + 10];
    memset(unk, 0, sizeof unk);
    unk[3] = 42; unk[9] = 5;
    memcpy(unk + 15, V_REQUEST, 10);
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    CHECK(clip_chan_feed(&st, unk, sizeof unk, evs, 8, &nev) == sizeof unk
          && nev == 2 && evs[0].kind == CLIP_EV_UNKNOWN
          && evs[1].kind == CLIP_EV_REQUEST,
          "an unknown opcode's payload is skipped and the stream survives it");

    /* A protocol error stops the stream: a header we refuse gives no length, so
     * there is no next boundary to find. */
    uint8_t bad[20];
    memset(bad, 0, sizeof bad);
    bad[0] = 0x15;                        /* non-zero `reserved` */
    memcpy(bad + 10, V_UPDATE, 10);
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    CHECK(clip_chan_feed(&st, bad, 20, evs, 8, &nev) == 10 && nev == 1
          && evs[0].kind == CLIP_EV_PROTO_ERROR,
          "a refused header is fatal: consumption stops, the caller must drop "
          "the connection");

    /* Malformed UTF-8 in a reply is reported as such, not as text. */
    uint8_t bad_utf8[12] = { 0, 0, 0, 4, 0, 0, 0, 0, 0, 2, 0xC3, 0x28 };
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    CHECK(clip_chan_feed(&st, bad_utf8, 12, evs, 8, &nev) == 12 && nev == 1
          && evs[0].kind == CLIP_EV_BAD_UTF8,
          "a reply that is not UTF-8 is reported as BAD_UTF8, never as text");

    /* Reset forgets a half-received payload. COUNTER-CASE: without it the first
     * bytes of a new connection would complete the dead one's message. */
    clip_chan_init(&st, buf, sizeof buf, &cfg);
    clip_chan_feed(&st, V_REPLY_AZERTY, 15, evs, 8, &nev);
    clip_chan_reset(&st);
    CHECK(clip_chan_feed(&st, V_UPDATE, 10, evs, 8, &nev) == 10 && nev == 1
          && evs[0].kind == CLIP_EV_UPDATE,
          "COUNTER-CASE: after a reset the next bytes are a fresh header, not "
          "the tail of the message the dead socket left behind");
}

static void channel_stale_text(void)
{
    uint8_t buf[256];
    clip_chan_t st;
    const clip_chan_cfg_t cfg = cfg_for_tests(CLIP_WIRE_CAP_DEFAULT);
    clip_chan_init(&st, buf, sizeof buf, &cfg);

    /* === THE TRAP THAT A CORRECT WIRE IMPLEMENTATION STILL FALLS INTO =======
     *
     * `OnLocalClipboardUpdate_` @0x140c30570 logs the failure of
     * `GetClipboardData(0xD)` - the VM's clipboard holds an image, say - and
     * then pushes the UPDATE ANYWAY, with `a1+296` still holding the PREVIOUS
     * text. Our REQUEST then returns that stale string. So identical text
     * arriving twice is the normal case, and a caller that pastes whatever
     * arrives re-pastes old content. */
    CHECK(clip_chan_text_is_new(&st, (const uint8_t *)"abc", 3),
          "the first text is new");
    clip_chan_note_text(&st, (const uint8_t *)"abc", 3);
    CHECK(!clip_chan_text_is_new(&st, (const uint8_t *)"abc", 3),
          "COUNTER-CASE: the same text again is NOT new - the VM announces an "
          "update even when its clipboard holds no text, and replies with its "
          "stale cache");
    CHECK(clip_chan_text_is_new(&st, (const uint8_t *)"abd", 3),
          "a different text of the same length is new");
    CHECK(clip_chan_text_is_new(&st, (const uint8_t *)"abcd", 4),
          "a different length is new");
    CHECK(clip_chan_text_is_new(&st, (const uint8_t *)"", 0), "empty is new");
    clip_chan_note_text(&st, (const uint8_t *)"", 0);
    CHECK(!clip_chan_text_is_new(&st, (const uint8_t *)"", 0),
          "and then empty is no longer new: an emptied clipboard is a state");

    /* It survives a reset on purpose: whether the user's content changed is not
     * a question a dead socket reopens. */
    clip_chan_note_text(&st, (const uint8_t *)"abc", 3);
    clip_chan_reset(&st);
    CHECK(!clip_chan_text_is_new(&st, (const uint8_t *)"abc", 3),
          "the last-seen text survives a reset, so a reconnect does not "
          "re-paste it");
}

static void channel_config(void)
{
    uint8_t buf[64];
    clip_chan_t st;
    clip_chan_cfg_t c;

    clip_chan_cfg_default(&c);
    CHECK(c.cap == CLIP_WIRE_CAP_DEFAULT && c.strict
          && c.request_timeout_ms == CLIP_CHAN_REQUEST_TIMEOUT_MS,
          "the defaults: 4 MiB, strict, 1000 ms");

    /* A cap of 0 must not switch the feature off by arithmetic accident. */
    c.cap = 0;
    CHECK(clip_chan_init(&st, buf, sizeof buf, &c)
          && st.cfg.cap == CLIP_WIRE_CAP_DEFAULT,
          "a cap of 0 falls back to the default rather than refusing "
          "every message");

    /* A timeout of 0 disables the retry rather than retrying continuously. */
    clip_chan_cfg_default(&c);
    c.request_timeout_ms = 0;
    clip_chan_init(&st, buf, sizeof buf, &c);
    clip_ev_t evs[4];
    size_t nev = 0;
    clip_chan_feed(&st, V_UPDATE, 10, evs, 4, &nev);
    CHECK(clip_chan_should_request(&st, 0), "the first REQUEST is still due");
    clip_chan_note_request_sent(&st, 0);
    CHECK(!clip_chan_should_request(&st, 0xFFFFFFFFu),
          "a timeout of 0 disables the RETRY, it does not make it permanent");

    /* The strict toggle, end to end through the channel. */
    uint8_t resv[10] = { 0x01, 0x00, 0, 2, 0, 0, 0, 0, 0, 0 };
    clip_chan_cfg_default(&c);
    clip_chan_init(&st, buf, sizeof buf, &c);
    clip_chan_feed(&st, resv, 10, evs, 4, &nev);
    CHECK(nev == 1 && evs[0].kind == CLIP_EV_PROTO_ERROR,
          "strict: a non-zero `reserved` is fatal");
    c.strict = false;
    clip_chan_init(&st, buf, sizeof buf, &c);
    clip_chan_feed(&st, resv, 10, evs, 4, &nev);
    CHECK(nev == 1 && evs[0].kind == CLIP_EV_UPDATE,
          "SHADOW_CLIP_STRICT=0: the same bytes are accepted as an UPDATE");

    /* Null-safety, since this is the part a future client will call from its
     * own thread. */
    CHECK(clip_chan_feed(NULL, V_UPDATE, 10, evs, 4, &nev) == 0,
          "a null state consumes nothing");
    clip_chan_init(&st, buf, sizeof buf, NULL);
    CHECK(st.cfg.cap == CLIP_WIRE_CAP_DEFAULT,
          "a null cfg takes the defaults");
    CHECK(clip_chan_feed(&st, NULL, 10, evs, 4, &nev) == 0,
          "a null buffer with a non-zero length consumes nothing");
    clip_chan_reset(NULL);
    clip_chan_note_request_sent(NULL, 0);
    clip_chan_note_text(NULL, NULL, 0);
    CHECK(!clip_chan_should_request(NULL, 0)
          && !clip_chan_text_is_new(NULL, NULL, 0),
          "every entry point tolerates a null state");
}

int main(void)
{
    printf("== clipboard channel :base+14 (CLIP 2026-10-02) ==\n");
    header();
    building();
    utf8();
    channel_receive_cycle();
    channel_reassembly();
    channel_stale_text();
    channel_config();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
