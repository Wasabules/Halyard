/* gamepad_wire.c - see gamepad_wire.h for the reason behind each rule. */

#include "gamepad_wire.h"

bool gamepad_wire_parse_vibration(const uint8_t *corps, size_t n,
                                  gamepad_wire_vibration_t *out)
{
    if (!corps || !out || n < GAMEPAD_WIRE_BODY_LEN) return false;
    if (corps[0] != 0x04) return false;                    /* version */
    if (corps[2] != GAMEPAD_WIRE_VIBRATION) return false;  /* type */

    out->id    = corps[1];
    out->low = corps[3];
    out->high = corps[4];
    return true;
}

/* === SRV6 2026-10-02 — THE AXIS BODY, AND WHY BYTE 13 IS NOT A SECOND COPY ===
 *
 * Read off ShadowStreamer 6.3.1,
 * `Controller::Clients::SufpClientV4::DealWithInput` @0x140c53f70, message
 * kind 1: bytes 12 and 13 are ONE little-endian int16.
 *   - trigger axes (mapped type 4 or 5): (double)i16@12 * 0.00390625 + 128.0
 *   - stick axes: the raw u16@12 is forwarded, read as the signed thumb value
 *
 * The 8-bit encoding this client has always used - `b[12] = v`, `b[13] = v+128`
 * - is exactly the int16 `257*v + 32768 (mod 2^16)`, which round-trips through
 * the server's transform back to `v`. That is why it works, and the old comment
 * calling byte 13 "the same value in the other representation" described the
 * coincidence rather than the mechanism. tests/test_vid_uplink.c checks the
 * round-trip over all 256 values; without that, the claim is a reading.
 *
 * Here as pure builders so the arithmetic is testable: ctrl_gamepad.c owns the
 * socket and the cipher and cannot be linked into an offline test. */
void gamepad_wire_build_axis16(uint8_t corps[GAMEPAD_WIRE_BODY_LEN],
                               uint8_t axis_idx, int16_t value)
{
    if (!corps) return;
    for (size_t i = 0; i < GAMEPAD_WIRE_BODY_LEN; i++) corps[i] = 0;
    corps[0]  = 0x04;          /* gamepad protocol version */
    corps[2]  = 0x01;          /* kind 1 = axis */
    corps[11] = axis_idx;
    corps[12] = (uint8_t)((uint16_t)value & 0xFF);
    corps[13] = (uint8_t)((uint16_t)value >> 8);
}

void gamepad_wire_build_axis8(uint8_t corps[GAMEPAD_WIRE_BODY_LEN],
                              uint8_t axis_idx, uint8_t value)
{
    /* `257*value + 32768` as an int16 - see above. Expressed through the 16-bit
     * builder so the two can never drift apart. */
    gamepad_wire_build_axis16(corps, axis_idx,
                              (int16_t)(uint16_t)(257u * (unsigned)value + 32768u));
}

/* The server's own transform, for a test to check our encoding against rather
 * than against a restatement of it. `trigger` picks the branch of kind 1. */
uint8_t gamepad_wire_server_axis_value(const uint8_t corps[GAMEPAD_WIRE_BODY_LEN],
                                       bool trigger)
{
    const int16_t raw = (int16_t)((uint16_t)corps[12] | ((uint16_t)corps[13] << 8));
    if (trigger) {
        /* (double)i16 * 0.00390625 + 128.0, truncated to int as the server does */
        return (uint8_t)(int)((double)raw * 0.00390625 + 128.0);
    }
    /* Stick: the server forwards the signed value; mapped back to our 0..255
     * scale it is the same arithmetic. */
    return (uint8_t)(int)((double)raw * 0.00390625 + 128.0);
}
