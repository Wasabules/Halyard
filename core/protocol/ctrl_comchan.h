/* ComChan over TCP+TLS on :base+14. RE'd through TIER 6 M4 + TIER 8 U4 and
 * cross-validated on 2026-05-16, when we still read it as a lifecycle and
 * keepalive bus broadcasting focus events.
 *
 * SUPERSEDED 2026-08-26: this channel is the VM's CLIPBOARD, and opcode 4
 * carries clipboard content, not an application name (KB.md §3.37 - the
 * official client names its sockets after their port). See ctrl_comchan.c;
 * the description below is kept because the wire format it documents is still
 * exact, and because it records the reading that was refuted.
 *
 * Byte-exact wire format:
 *   [u32_be type][u32_be sub][u16_be len][string payload]
 *
 * === THE WIRE, SETTLED AT THE BINARY (RE-5, 2026-09-03) ===
 *
 * Ten bytes, FOUR fields - not the three we had:
 *
 *   [u16_be reserved][u16_be opcode][u16_be format][u32_be length][payload]
 *    0..1             2..3            4..5           6..9
 *
 * `reserved` is a GATE, not a spare: `bodySize()` @0xabd460 begins
 * `cmp WORD PTR [rsi],0x0 / jne -> return -1`. Non-zero is a protocol error and
 * the channel is dropped. Every serialiser writes zero.
 *
 * `format` is a ClipboardFormatType. One value is named (@0x78ece0):
 * `0 = TEXT`, and the receive handler (@0x768080) refuses anything else with
 * "Invalid clipboard format: {}".
 *
 * `length` is a u32 at offset SIX. We read a u16 at offset eight, which is its
 * LOW HALF - and that is the whole value for anything under 65 536 bytes. The
 * two encodings are byte-identical below that, measured, which is why the error
 * survived: it is not wrong on any message this project has ever sent or seen.
 * Past 64 KiB the old model writes a truncated length and the framing never
 * resynchronises. A clipboard that big is not exotic.
 *
 * The u16 fields are BIG-ENDIAN: the Update serialiser (@0xabd360) byte-swaps
 * the format with `rol ax,8` before storing it.
 *
 * === THE FIVE OPCODES, AND THEIR REAL NAMES ===
 *
 * Every RTTI class deriving from BaseMessage gives five clipboard messages and
 * no sixth; each serialiser pins its own number:
 *
 *   0 Connect    (@0xac4080) ten zero bytes, sent right after the handshake
 *   1 Flush      (@0xabd290) "drop whatever you hold"
 *   2 Update     (@0xabd360) "my clipboard now holds this FORMAT", no data
 *   3 Request    (@0xabd390) "send me that format"
 *   4 ReplyText  (@0xabd3c0) THE CONTENT: length = u32_be payload size, then
 *                            the raw string - no terminator, no charset marker,
 *                            no padding
 *
 * This file used to call 1 FOCUS_CHANGED and 2 WINDOW_STATE_CHANGED, and read
 * the "1 -> 2 -> 4 triplet on every focus change" as a focus bus. It is not: it
 * is Flush + Update + ReplyText, i.e. "drop what you have / I have TEXT / here
 * it is". The bytes were right; the story around them was invented.
 *
 * === THE TWO DIRECTIONS DO NOT USE THE SAME MODEL ===
 *
 *   server -> client is PULL : Update(2,TEXT), then WE send Request(3,TEXT),
 *                              then the server sends ReplyText(4). Measured
 *                              3/3, one RTT between Request and ReplyText.
 *   client -> server is PUSH : Flush(1), Update(2,TEXT), ReplyText(4), back to
 *                              back and unsolicited.
 *
 * So serving this channel means answering an Update with a Request - a client
 * that only listens receives nothing.
 *
 * === WHAT THE CAPTURES CANNOT SAY ===
 *
 * There is no fragmentation and none is needed: `bodySize()` returns the u32
 * with no cap and no chunk flag, and `headerSize()` (@0xabd440) is a fixed 10.
 * One clipboard is one message, up to 4 GiB, which the reader must reassemble
 * across TCP reads. The largest payload in any capture is 155 bytes, so nothing
 * ever crossed a TLS record boundary here - the large-payload path is inferred
 * from the code, never observed.
 *
 * Initial bootstrap, measured: Connect(0), then Flush(1) and Update(2), all of
 * zero length.
 * Heartbeat: never pinned byte-exact; we used to try one every 7 s.
 *
 * V15 2026-05-16: byte-exact implementation, written to test the hypothesis
 * that type=2 is what gates the server's multi-NAL bottom slice
 * (TIER 8, U4.7).
 */
#pragma once

/* The opcodes and the one known format, so the code stops carrying bare
 * numbers. See the wire notes above for where each number is pinned. */
#define COMCHAN_OP_CONNECT    0u
#define COMCHAN_OP_FLUSH      1u
#define COMCHAN_OP_UPDATE     2u
#define COMCHAN_OP_REQUEST    3u
#define COMCHAN_OP_REPLY_TEXT 4u

#define COMCHAN_FORMAT_TEXT   0u


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctrl_comchan_s ctrl_comchan_t;

/* Opens the TLS connection to vm_host:base_port+14 and sends the type=0 init
 * followed by the initial 1/2 handshake - opcode 4 is deliberately not sent
 * any more, see ctrl_comchan.c. Spawns a heartbeat thread (7 s period, off by
 * default). Returns 0 on success, -1 otherwise. */
int  ctrl_comchan_open (ctrl_comchan_t **out,
                        const char *vm_host, uint16_t base_port,
                        const char *app_name);

/* `ctrl_comchan_notify_focus()` was removed on 2026-08-26. It signalled a
 * focus/activity change by sending the 1/2/4 triplet with app_name - and
 * opcode 4 actually carries the VM's CLIPBOARD CONTENT (KB.md §3.37). It had
 * no caller outside this module. */

void ctrl_comchan_close(ctrl_comchan_t *c);

#ifdef __cplusplus
}
#endif
