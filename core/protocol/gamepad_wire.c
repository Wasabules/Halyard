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
