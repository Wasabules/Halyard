/* test_gamepad_wire - the rumble, offline.
 *
 * Each check names its COUNTER-CASE: the exact input that would have passed
 * under a wrong reading. Here the counter-cases all come from REAL messages,
 * taken from our own console logs of 2026-08-28 - 368 bodies with a non-zero
 * motor in a single session.
 */
#include <stdio.h>
#include <string.h>

#include "../core/protocol/gamepad_wire.h"

static int checks = 0, failures = 0;
static void CHECK(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

int main(void)
{
    printf("== gamepad channel rumble (:base+13) ==\n");
    gamepad_wire_vibration_t v;

    /* === THE FIVE REAL BODIES, taken from switch_AUD16_flac.log ===
     * They are the reason this format is not a deduction: the 22 kVibration
     * messages in the OFFICIAL client's captures are all zero, for lack of a
     * game that rumbles. A structure read out of zeros is not a structure. */
    const uint8_t full[14]   = {0x04,0x00,0x07,0xff,0xff,0,0,0,0,0,0,0,0,0};
    const uint8_t low_only[14]={0x04,0x00,0x07,0xff,0x00,0,0,0,0,0,0,0,0,0};
    const uint8_t mixte[14]   = {0x04,0x00,0x07,0x1f,0x5f,0,0,0,0,0,0,0,0,0};
    const uint8_t high_only[14]={0x04,0x00,0x07,0x00,0xe8,0,0,0,0,0,0,0,0,0};
    const uint8_t faible[14]  = {0x04,0x00,0x07,0x21,0x24,0,0,0,0,0,0,0,0,0};

    CHECK(gamepad_wire_parse_vibration(full, sizeof full, &v)
            && v.low == 0xff && v.high == 0xff, "both motors at full");
    CHECK(gamepad_wire_parse_vibration(low_only, sizeof low_only, &v)
            && v.low == 0xff && v.high == 0x00, "moteur basse seul");
    CHECK(gamepad_wire_parse_vibration(mixte, sizeof mixte, &v)
            && v.low == 0x1f && v.high == 0x5f, "amplitudes distinctes");
    CHECK(gamepad_wire_parse_vibration(high_only, sizeof high_only, &v)
            && v.low == 0x00 && v.high == 0xe8, "moteur haute seul");
    CHECK(gamepad_wire_parse_vibration(faible, sizeof faible, &v)
            && v.low == 0x21 && v.high == 0x24, "vibration faible");

    /* COUNTER-CASE 1 - BYTE 5, AND THE READING THE DISASSEMBLY REFUTED.
     * One report read `04 [flag] 07 [id] [A] [B]`: identifier at 3, motors at 4
     * and 5. Across 620 real bodies, byte 5 is 00 six hundred and twenty times,
     * and byte 3 takes 147 distinct values. A motor does not stay at zero over
     * 620 messages; an identifier does not take 147 values with a single
     * controller. This case pins the right reading: byte 5 is NOT read. */
    uint8_t o5[14] = {0x04,0x00,0x07,0x10,0x20,0xff,0,0,0,0,0,0,0,0};
    CHECK(gamepad_wire_parse_vibration(o5, sizeof o5, &v)
            && v.low == 0x10 && v.high == 0x20,
            "byte 5 is not part of the reading (the 'binary' reading is refuted)");

    /* COUNTER-CASE 2 - A kReply MUST NOT PASS FOR A RUMBLE.
     * Both arrive on the same socket, 20 ms apart, and the kReply carries `03`
     * at byte 3: confusing them would rumble at 3/255 every time a controller is
     * plugged in. */
    const uint8_t reply[14] = {0x04,0x00,0x08,0x03,0,0,0,0,0,0,0,0x01,0,0};
    CHECK(!gamepad_wire_parse_vibration(reply, sizeof reply, &v),
            "a kReply (type 8) is refused");

    /* COUNTER-CASE 3 - THE BOUNDS. The channel is encrypted but not
     * authenticated against us: a truncated body must not be read. */
    CHECK(!gamepad_wire_parse_vibration(full, 13, &v), "13 bytes: refused");
    CHECK(!gamepad_wire_parse_vibration(full, 0, &v),  "0 bytes: refused");
    CHECK(!gamepad_wire_parse_vibration(NULL, 14, &v),  "null pointer: refused");

    /* COUNTER-CASE 4 - THE VERSION. A body that does not start with 0x04 is not
     * from this protocol; accepting it would rumble on noise. */
    uint8_t mauvaise[14] = {0x05,0x00,0x07,0xff,0xff,0,0,0,0,0,0,0,0,0};
    CHECK(!gamepad_wire_parse_vibration(mauvaise, sizeof mauvaise, &v),
            "version != 0x04: refused");

    /* COUNTER-CASE 5 - THE IDENTIFIER DOES NOT FILTER.
     * It is 0x00 in all 620 observed bodies: with a single controller one cannot
     * tell "identifier" from "constant". Filtering on it would bet on a semantics
     * that is not established, and a secondary controller would stop rumbling
     * with no error message. */
    uint8_t autre_id[14] = {0x04,0x07,0x07,0x40,0x50,0,0,0,0,0,0,0,0,0};
    CHECK(gamepad_wire_parse_vibration(autre_id, sizeof autre_id, &v)
            && v.id == 0x07 && v.low == 0x40,
            "a non-zero identifier is READ, never a reason to reject");

    /* Rest is a valid value, not an absence: that is how the server turns the
     * motors off. Refusing it would leave the controller rumbling. */
    const uint8_t repos[14] = {0x04,0x00,0x07,0,0,0,0,0,0,0,0,0,0,0};
    CHECK(gamepad_wire_parse_vibration(repos, sizeof repos, &v)
            && v.low == 0 && v.high == 0,
            "rest (0,0) is a VALID message - it is the shutdown");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
