/* test_ctrl_inv - which Request oneof field a CtrlChanV2 message carries.
 *
 * The checks that matter are the ones on `inv_ancien()`, which reproduces the
 * parser as it stood before SRV7. They assert that it counted the Capabilities
 * message as NOTHING - the blind spot the fix removes - and that it was
 * otherwise right. Without that counter-case the fix is a claim; with it, both
 * the defect and its limits are pinned. In particular it did NOT invent a
 * phantom "f1", which a first reading of this code wrongly asserted.
 *
 * Compiled with -Wall -Wextra -Werror by tests/run_tests.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../core/protocol/ctrl_inv.h"

static int checks = 0, failures = 0;

static void eq(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %s: got %d, expected %d\n", what, got, want);
    }
}

/* === The parser as it was before SRV7, kept so the defect stays visible ===
 * It skipped field 1 unconditionally. */
static int inv_ancien(const uint8_t *body, size_t len)
{
    size_t i = 0;
    while (i < len && (body[i] & 0x80)) i++;
    if (i < len) i++;
    while (i < len && (body[i] & 0x80)) i++;
    if (i < len) i++;
    if (i + 2 < len && (body[i] >> 3) == 2) {
        i++;
        while (i < len && (body[i] & 0x80)) i++;
        if (i < len) i++;
        if (i < len) return (int)(body[i] >> 3);
    }
    return -1;
}

int main(void)
{
    /* --- Capabilities: NO field 1. Shape from ctrl_build_capabilities():
     *   f2 { f3 { f1 = "" } }  f4 {...}  f5 {...}
     * i.e. `12 LL 1a LL 0a 00 ...`. Only the first four bytes matter here. */
    const uint8_t caps[] = {
        0x12, 0x06, 0x1a, 0x04, 0x0a, 0x02, 0x00, 0x00,
        0x22, 0x02, 0x08, 0x02, 0x2a, 0x02, 0x08, 0x01
    };
    eq("capabilities -> f3", ctrl_inv_request_field(caps, sizeof caps), 3);
    /* THE DEFECT: the old parser skipped a field 1 that is not there, ate the
     * f2 tag, and counted this message as nothing at all. */
    eq("capabilities, OLD parser -> not counted", inv_ancien(caps, sizeof caps), -1);

    /* --- Heartbeat: f1 = seq, then f2 { f3 { f2 = "" } }. */
    const uint8_t hb[] = {
        0x08, 0x0d, 0x12, 0x04, 0x1a, 0x02, 0x12, 0x00,
        0x22, 0x02, 0x08, 0x02
    };
    eq("heartbeat -> f3", ctrl_inv_request_field(hb, sizeof hb), 3);
    eq("heartbeat, OLD parser -> f3 too", inv_ancien(hb, sizeof hb), 3);

    /* --- A multi-byte sequence number must not shift the result. seq = 300
     * encodes as `ac 02`. */
    const uint8_t hb_big[] = {
        0x08, 0xac, 0x02, 0x12, 0x04, 0x1a, 0x02, 0x12, 0x00
    };
    eq("heartbeat seq=300 -> f3", ctrl_inv_request_field(hb_big, sizeof hb_big), 3);

    /* --- The eight announcements are Request field 8. */
    const uint8_t ann[] = { 0x08, 0x05, 0x12, 0x04, 0x42, 0x02, 0x08, 0x01 };
    eq("announcement -> f8", ctrl_inv_request_field(ann, sizeof ann), 8);

    /* --- Authentication is field 4, Encryption field 12 (the two anchors the
     * server-side dispatch mapping was derived from). */
    const uint8_t auth[] = { 0x08, 0x01, 0x12, 0x04, 0x22, 0x02, 0x10, 0x01 };
    eq("authentication -> f4", ctrl_inv_request_field(auth, sizeof auth), 4);
    const uint8_t enc[] = { 0x08, 0x02, 0x12, 0x04, 0x62, 0x02, 0x08, 0x20 };
    eq("encryption -> f12", ctrl_inv_request_field(enc, sizeof enc), 12);

    /* --- Field 1 present but NOT a varint: it is not our sequence number, so
     * it must not be skipped as one. `0a` = field 1 wire 2. The message then has
     * no f2 where one is expected, and the answer is "unknown". */
    const uint8_t f1_bytes[] = { 0x0a, 0x02, 0x00, 0x00, 0x12, 0x02, 0x22, 0x00 };
    eq("field 1 length-delimited -> unknown",
       ctrl_inv_request_field(f1_bytes, sizeof f1_bytes), -1);

    /* --- Robustness: nothing may read past the end. These are the inputs a
     * truncated TLS record would hand us. */
    eq("NULL -> unknown",            ctrl_inv_request_field(NULL, 10), -1);
    eq("empty -> unknown",           ctrl_inv_request_field(caps, 0), -1);
    eq("seq tag alone -> unknown",   ctrl_inv_request_field(hb, 1), -1);
    eq("seq only -> unknown",        ctrl_inv_request_field(hb, 2), -1);
    eq("f2 tag alone -> unknown",    ctrl_inv_request_field(hb, 3), -1);
    eq("f2 len, no body -> unknown", ctrl_inv_request_field(hb, 4), -1);
    eq("one oneof byte -> f3",       ctrl_inv_request_field(hb, 5), 3);
    {
        /* An unterminated varint for the sequence number: every byte has the
         * continuation bit set, so the scan must stop at the end of the buffer
         * and report unknown rather than read on. */
        const uint8_t runaway[] = { 0x08, 0x80, 0x80, 0x80 };
        eq("unterminated seq -> unknown",
           ctrl_inv_request_field(runaway, sizeof runaway), -1);
    }
    /* A length-delimited f2 whose own length is an unterminated varint. */
    {
        const uint8_t runaway2[] = { 0x08, 0x01, 0x12, 0x80, 0x80 };
        eq("unterminated f2 length -> unknown",
           ctrl_inv_request_field(runaway2, sizeof runaway2), -1);
    }
    /* f2 with the wrong wire type is not the Request. */
    {
        const uint8_t bad_wire[] = { 0x08, 0x01, 0x10, 0x04, 0x42, 0x00 };
        eq("f2 varint -> unknown",
           ctrl_inv_request_field(bad_wire, sizeof bad_wire), -1);
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
