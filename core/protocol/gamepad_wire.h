/* gamepad_wire - PURE parsing of the gamepad channel's downstream messages.
 *
 * Why a module of its own: the `:base+13` channel is bidirectional (G53), and
 * what the server sends back on it is protocol, not user interface. Putting it
 * here makes it testable offline - no console, no VM, no network - the way
 * `cursor_wire.c` was for the cursor channel.
 *
 * CONTRACT: no state, no I/O, no `getenv`. That is what earns it a place in
 * MODULES_PURS in tests/run_tests.sh.
 *
 * === THE FORMAT, MEASURED AND NOT DEDUCED (G57, 2026-08-28) ===
 *
 * 14-byte body, after chacha20-poly1305 decryption. BARE envelope
 * `[ct 14][nonce 12][tag 16]` = 42 bytes, with no SUFP header (KB.md §3.40).
 *
 *   byte 0  : 0x04, gamepad protocol version        (620/620)
 *   byte 1  : remote device identifier              (0x00 on 620/620)
 *   byte 2  : message type - 7 = kVibration, 8 = kReply
 *   byte 3  : AMPLITUDE of the LOW frequency motor  (0..255)
 *   byte 4  : AMPLITUDE of the HIGH frequency motor (0..255)
 *   bytes 5..13 : never read by the official client
 *
 * The format was at first believed impossible to pin down: the 22 kVibration
 * messages in the official-client captures are all zero, because nothing was
 * rumbling while the capture ran. It was pinned down by OUR OWN console logs -
 * 368 messages with a non-zero motor in a single session, the counter reaching
 * #19450. Three successive reports had missed it, because no inventory tool
 * scans the `captures/tests_autonomes_<date>` directory.
 *
 * THREE PROPERTIES THAT CONSTRAIN THE CALLER:
 *
 * 1. NO DURATION. The official client calls SDL with `duration_ms = 0`, which
 *    means "never expires". Rumble is therefore a STATE that holds until the
 *    next message - not a pulse. Whoever implements it must plan the SHUTDOWN:
 *    the last message of a session is often non-zero (`04 00 07 ff 00` seen at
 *    the end of a log, big motor at full power).
 *
 * 2. SUSTAINED RATE. Median 33.5 messages/s, p90 46.4, max 48.2. This is not a
 *    burst: one IPC round trip per packet is out of the question. The caller
 *    must keep the LAST value and apply it once per frame.
 *
 * 3. THE SCALE IS THE FULL BYTE, 0 to 255. The official client widens it by an
 *    eight-bit shift (255 -> 0xFF00) because the SDL API takes u16; do not
 *    reproduce that shift, it is an API artefact and not a protocol quantity.
 *
 * THE LEFT/RIGHT ASSIGNMENT IS AT [C80]. It comes from reading two register
 * swaps that cancel each other out, plus one link that was never disassembled
 * (`call QWORD PTR [r8+0x50]`, a variant visitor). Independent cross-check: on
 * the Windows side the server relays
 * `XUSB_VIBRATION { LeftMotorSpeed; RightMotorSpeed; }` in that order. Hence a
 * swap toggle on the caller side rather than a bet.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAMEPAD_WIRE_BODY_LEN 14

/* Message types at byte 2, named as the official binary names them. */
#define GAMEPAD_WIRE_VIBRATION 7
#define GAMEPAD_WIRE_REPLY     8

typedef struct {
    uint8_t id;      /* octet 1 : peripherique distant */
    uint8_t low;   /* byte 3: the low-frequency motor's amplitude */
    uint8_t high;   /* byte 4: the high-frequency motor's amplitude */
} gamepad_wire_vibration_t;

/* Returns true if `corps` is a readable kVibration, and fills `out`.
 *
 * DOES NOT FILTER on the device identifier: it is 0x00 in all 620 observed
 * bodies, and with a single gamepad there is no way to tell an "identifier"
 * from a constant. Using it to REJECT would be betting on semantics we have not
 * established - the official binary does log "Received vibration message for
 * unknown remote device ID" and gives up, which proves it compares, but not
 * against what. */
bool gamepad_wire_parse_vibration(const uint8_t *corps, size_t n,
                                  gamepad_wire_vibration_t *out);

/* SRV6 2026-10-02 - UPSTREAM axis bodies. Bytes 12..13 of an axis message are
 * one little-endian int16, not a value plus a second representation; the 8-bit
 * form this client has always sent encodes `257*v + 32768`. Pure, so the
 * arithmetic is covered by tests/test_vid_uplink.c - ctrl_gamepad.c owns the
 * socket and cannot be linked offline. `corps` is zeroed, then filled. */
void gamepad_wire_build_axis16(uint8_t corps[GAMEPAD_WIRE_BODY_LEN],
                               uint8_t axis_idx, int16_t value);
void gamepad_wire_build_axis8(uint8_t corps[GAMEPAD_WIRE_BODY_LEN],
                              uint8_t axis_idx, uint8_t value);
/* The server's transform applied to such a body, so a test can check our
 * encoding against the server's reading instead of against a paraphrase. */
uint8_t gamepad_wire_server_axis_value(const uint8_t corps[GAMEPAD_WIRE_BODY_LEN],
                                       bool trigger);

#ifdef __cplusplus
}
#endif
