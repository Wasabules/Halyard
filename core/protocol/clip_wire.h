/* clip_wire.h - the CLIPBOARD channel `:base+14`, as PURE functions.
 *
 * === CLIP 2026-10-02 - WHAT THIS MODULE IS FOR ===============================
 *
 * `ctrl_comchan.{c,h}` already describes this wire correctly (RE-5, from the
 * official CLIENT binary) and already WRITES the four fields correctly. What it
 * has is no reader, no REQUEST and no REPLY: it sends CONNECT, FLUSH and UPDATE,
 * all zero-length, and the server ignores the last two outright - so the channel
 * opens and nothing crosses it in either direction.
 *
 * One correction to carry over, because it decided the shape of this work.
 * KB §3.37's live-risk line says `SHADOW_COMCHAN=1` overwrites the VM's
 * clipboard every 7 s. That was true until S52 (2026-08-26) and is not true
 * now: S52 removed opcode 4 from `ctrl_comchan.c` entirely, the heartbeat is
 * behind its own `SHADOW_COMCHAN_HEARTBEAT` (default 0), and what the heartbeat
 * would send if forced is FLUSH + UPDATE with no payload, which the server
 * discards. So this module is purely ADDITIVE: there was no destructive write
 * left to remove. `SHADOW_COMCHAN` still stays off, because the channel it
 * opens now does nothing at all.
 *
 * This module is the codec, separate and pure, so the thing whose bug costs an
 * RE campaign is the thing that is tested - the same split as `cursor_wire.c` /
 * `ctrl_video_tcp.c`. And the separation is itself the guard S52 asked for: the
 * day a REPLY(4) is sent, it is sent by a function whose name says it carries a
 * clipboard, not by a heartbeat.
 *
 * It is PURE: no global state, no I/O, no logging, no `getenv`. The strictness
 * and the size ceiling are PARAMETERS, exactly as `cap` is for
 * `cursor_wire_parse_header`, because this module says what the protocol MEANS
 * and not what we choose to do about it. `clip_chan.{c,h}` holds the state and
 * resolves the `SHADOW_CLIP_*` toggles.
 *
 * === THE WIRE ================================================================
 *
 * Ten bytes of header, FOUR fields, all integers BIG-ENDIAN:
 *
 *   [u16_be reserved][u16_be opcode][u16_be format][u32_be length][payload]
 *    0..1             2..3           4..5           6..9
 *
 * Established TWICE, independently, and the two agree byte for byte:
 *
 *  - from the official CLIENT (RE-5, 2026-09-03, KB §3.47): `headerSize()`
 *    @0xabd440 returns 10; `bodySize()` @0xabd460 begins
 *    `cmp WORD PTR [rsi],0x0 / jne -> return -1` then `mov eax,[rsi+0x6]`;
 *    the `Update` serialiser @0xabd360 byte-swaps the format with `rol ax,8`,
 *    which is what proves the u16 fields are big-endian.
 *
 *  - from the SERVER, ShadowStreamer 6.3.1 (CLIP, 2026-10-02):
 *    `Engines::Clipboard::Clients::StfpClient::DealWithInput` @0x140c2ecb0
 *    reads `ntohs` / `ntohs` / `ntohs` / `ntohl` over a ten-byte struct, and
 *    `sub_140C2E7F0` @0x140c2e7f0 is the symmetric `htons`x3 + `htonl` applied
 *    in place before every send.
 *
 * The length is a u32 at offset SIX. `ctrl_comchan.c` reads a u16 at offset
 * EIGHT, which is that u32's LOW HALF - identical below 65,536 and truncated
 * for good above it. Measured: the two encodings differ at exactly 64 KiB, and
 * a clipboard that size is nothing exotic.
 *
 * === THE FIVE OPCODES ========================================================
 *
 * The server logs its own names for them (`DealWithInput:93`, the string table
 * at `MESSAGE_TYPE_*`), and they match the client's RTTI classes:
 *
 *   0 CONNECT     ten zero bytes, the FIRST application bytes after the TLS
 *                 handshake. Mandatory: see below.
 *   1 FLUSH       "drop whatever you hold". No payload.
 *   2 UPDATE      "my clipboard now holds this FORMAT". No payload.
 *   3 REQUEST     "send me that format". No payload.
 *   4 REPLY_TEXT  THE CONTENT. `length` = exact payload size, no terminator,
 *                 no charset marker, no padding.
 *
 * === THE TWO DIRECTIONS ARE NOT SYMMETRIC, AND THE SERVER DECIDES WHICH ======
 *
 *   VM -> us is a PULL. The VM sends UPDATE(2) and then WAITS. We must send
 *   REQUEST(3); only then does the VM send REPLY_TEXT(4). A client that merely
 *   listens never receives a single character. Measured 3/3 on a capture
 *   (INV1, RX 2 -> TX 3 -> RX 4, ~20 ms) and reproduced in the server:
 *   `DealWithInput` type 3 -> `sub_140C2F5B0` -> `FakeWindow::push_` subtype 3
 *   -> `PostMessage(WM_CLIPBOARD_REQUEST)` -> `OnClipboardRequest_` @0x140c30bc0
 *   -> subtype 4 -> `handleApplication_` @0x140c2f110 sends REPLY_TEXT.
 *
 *   us -> VM is a PUSH, and ONLY REPLY_TEXT(4) does anything. This is the
 *   finding that decides our send path: `StfpClient::DealWithInput` dispatches
 *   type 3 and type 4 and *silently ignores types 1 and 2* -
 *   `if (v10 != 1 && v10 != 2)` guards the whole dispatch. So the official
 *   client's `FLUSH, UPDATE, REPLY_TEXT` triplet is decorative in its first two
 *   thirds: the VM's clipboard is set by REPLY_TEXT alone. The mirror-image
 *   trap of the PULL direction: a client that sends UPDATE(2) and waits for a
 *   REQUEST(3) from the VM waits for ever.
 *
 * === THE PAYLOAD IS UTF-8 ====================================================
 *
 * TEXT is the only format, and the VM's clipboard handle is `CF_UNICODETEXT`
 * (13) in both directions - `GetClipboardData(0xD)` in
 * `OnLocalClipboardUpdate_` @0x140c30570, `SetClipboardData(0xD, ...)` in
 * `OnRemoteClipboardUpdate_` @0x140c30930. The wire carries the NARROW
 * conversion of that UTF-16, and the code page is named in the binary:
 * `sub_140D45850` / `sub_140D45960` call `WideCharToMultiByte` /
 * `MultiByteToWideChar` with `0xFDE9` = 65001 = **CP_UTF8**. The length passed
 * is the exact string size, so there is no NUL on the wire.
 *
 * KB §3.47 says "no charset marker"; it is UTF-8, and this is where that is
 * written down.
 *
 * Neither conversion passes `MB_ERR_INVALID_CHARS`, so the VM will not reject
 * malformed UTF-8 - it will substitute U+FFFD, silently. That is why
 * `clip_wire_utf8_valid()` exists on OUR side: the only parser that will ever
 * complain is ours.
 */
#ifndef CLIP_WIRE_H
#define CLIP_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed, from `headerSize()` @0xabd440 and from the `v23 = 10` minimum that
 * `StfpClient::DealWithInput` @0x140c2ecb0 hands to `CheckMessageSize_`. */
#define CLIP_WIRE_HEADER_LEN 10

#define CLIP_OP_CONNECT     0u
#define CLIP_OP_FLUSH       1u
#define CLIP_OP_UPDATE      2u
#define CLIP_OP_REQUEST     3u
#define CLIP_OP_REPLY_TEXT  4u

/* `ClipboardFormatType`. One named value in the client (@0x78ece0), and the
 * receive handler (@0x768080) refuses everything else with "Invalid clipboard
 * format". Every serialiser on both sides writes 0. */
#define CLIP_FMT_TEXT       0u

/* === CLIP 2026-10-02 - WHERE THE DEFAULT CEILING COMES FROM =================
 *
 * The VM clamps what it sends. `OnLocalClipboardUpdate_` @0x140c30570:
 *
 *     v14 = (GlobalSize(h) >> 1) - 1;          // UTF-16 code units, no NUL
 *     if ( v14 > 0x100000 ) v14 = 0x100000;    // 1,048,576 units, hard clamp
 *
 * so no REPLY_TEXT from the VM can exceed 1 Mi UTF-16 code units. Converted to
 * UTF-8 the worst case is THREE bytes per code unit (a BMP code point in
 * U+0800..U+FFFF); a surrogate pair costs 4 bytes for 2 units, i.e. only 2
 * bytes per unit, so it is not the worst case. The ceiling is therefore
 * 3 x 1,048,576 = 3,145,728 bytes, and 4 MiB is that rounded up with headroom.
 *
 * The other direction has NO ceiling at all on the server: `sub_140C30D70`
 * hands the u32 straight to a `reserve()`, with nothing between the network and
 * the allocator but a `length_error` above 2^63. So this cap protects US, and
 * we also refuse to BUILD a message larger than it - sending the VM more than
 * it would ever send us is asking for an allocation we cannot predict. */
#define CLIP_WIRE_CAP_DEFAULT (4u * 1024u * 1024u)

/* The VM-side clamp itself, in UTF-16 code units, for callers that want to
 * truncate before converting rather than after. */
#define CLIP_WIRE_VM_UTF16_MAX 0x100000u

typedef struct {
    uint16_t reserved;    /* bytes 0..1. MUST be 0: the client's `bodySize()`
                           * returns -1 otherwise, and that is a protocol error,
                           * not a spare field. The SERVER does not check it. */
    uint16_t opcode;      /* bytes 2..3, CLIP_OP_* */
    uint16_t format;      /* bytes 4..5, CLIP_FMT_* */
    uint32_t payload_len; /* bytes 6..9: payload bytes that FOLLOW the header */
    size_t   msg_len;     /* = CLIP_WIRE_HEADER_LEN + payload_len */
} clip_wire_header_t;

/* Reads a header from `p` (`n` bytes available).
 *
 * Returns false when `n < CLIP_WIRE_HEADER_LEN`, or - in strict mode - when a
 * guard fails. Returns TRUE for a complete header whose PAYLOAD has not arrived
 * yet: `out->msg_len` then says how many bytes the whole message needs. That is
 * what lets the caller reassemble, and it is not optional here: the VM streams
 * the payload with no framing of its own, and its own reader
 * (`DataCache::PushDataToCache` @0x140c30dc0) does exactly the same thing -
 * it logs `state=waiting` until `received == announced`.
 *
 * `cap` is the largest `payload_len` the caller will accept. A larger one
 * returns false: a REFUSAL, not a truncation. Pass CLIP_WIRE_CAP_DEFAULT when
 * there is no reason to be narrower. The check happens BEFORE `msg_len` is
 * computed, on the 32-bit value, so that `10 + 0xFFFFFFFF` cannot wrap where
 * `size_t` is 32 bits - the same ordering, and the same reason, as
 * `cursor_wire_parse_header`.
 *
 * `strict` applies the two guards the official CLIENT applies and the SERVER
 * does not: `reserved == 0` and `format == CLIP_FMT_TEXT`. Being strict on
 * receive while the peer is lax is deliberate - see clip_chan.h for the
 * `SHADOW_CLIP_STRICT` toggle that reverts it.
 *
 * An UNKNOWN opcode still returns true. That is not laxity: the header is
 * well-formed and the only way to stay in sync with the stream is to consume
 * the declared length. The server does precisely this - "process unknown
 * message of type 0x%x." (`DealWithInput:110`) and no `SetInvalid`. Ask
 * `clip_wire_opcode_known()` before acting on one. */
bool clip_wire_parse_header(const uint8_t *p, size_t n, uint32_t cap,
                            bool strict, clip_wire_header_t *out);

/* True for CLIP_OP_CONNECT..CLIP_OP_REPLY_TEXT. There is no sixth opcode:
 * enumerating the client's RTTI classes deriving from `BaseMessage` yields
 * five, and the server's own name table stops at the same five. */
bool clip_wire_opcode_known(uint16_t opcode);

/* The server's own spelling of an opcode (`MESSAGE_TYPE_CONNECT`, ...), or
 * "unknown". Never NULL, and the storage is static read-only - this stays pure.
 * Having the server's exact word in our logs is what makes a log from the VM
 * and a log from us comparable line by line. */
const char *clip_wire_opcode_name(uint16_t opcode);

/* Writes a 10-byte header with no payload into `out` (`out_cap` bytes).
 * Returns CLIP_WIRE_HEADER_LEN, or 0 if it does not fit.
 *
 * Use it for CONNECT, FLUSH, UPDATE and REQUEST, which carry no data. */
size_t clip_wire_build_header(uint8_t *out, size_t out_cap,
                              uint16_t opcode, uint16_t format);

/* Writes header + payload for a REPLY_TEXT into `out` (`out_cap` bytes).
 * Returns the total written (10 + text_len), or 0 on refusal.
 *
 * Refuses when it does not fit, when `text_len > cap`, and when `text` is NULL
 * with a non-zero length. An EMPTY text is allowed and means an empty
 * clipboard: `OnRemoteClipboardUpdate_` @0x140c30930 calls `EmptyClipboard()`
 * before `SetClipboardData`, so a zero-length REPLY_TEXT clears the VM's
 * clipboard rather than being a no-op. That is a usable operation, not an
 * error, and the caller has to be able to express it.
 *
 * No NUL is written: the VM converts exactly `length` bytes
 * (`MultiByteToWideChar(CP_UTF8, 0, buf, length, ...)`), so a terminator would
 * become a U+0000 inside the pasted text. */
size_t clip_wire_build_reply_text(uint8_t *out, size_t out_cap, uint32_t cap,
                                  const char *text, size_t text_len);

/* True when `p[0..n)` is well-formed UTF-8: no overlong form, no surrogate
 * half, nothing above U+10FFFF, no truncated sequence.
 *
 * Why we check what the peer does not: neither of the VM's conversions passes
 * `MB_ERR_INVALID_CHARS`, so malformed input is replaced by U+FFFD without a
 * word. On receive that would put replacement characters in front of the user;
 * on send it would put them in the VM's clipboard. A NUL byte is accepted here
 * - it is valid UTF-8 - but see `clip_wire_utf8_trim_len`. */
bool clip_wire_utf8_valid(const uint8_t *p, size_t n);

/* The largest length <= `limit` that does not cut `p[0..n)` in the middle of a
 * UTF-8 sequence. Returns `n` when `n <= limit`.
 *
 * Needed because every ceiling in this protocol is in BYTES while the content
 * is multi-byte: truncating at a byte boundary would hand the VM a half
 * sequence, which it would paste as U+FFFD. */
size_t clip_wire_utf8_trim_len(const uint8_t *p, size_t n, size_t limit);

#ifdef __cplusplus
}
#endif

#endif /* CLIP_WIRE_H */
