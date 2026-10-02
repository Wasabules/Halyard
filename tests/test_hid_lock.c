/* test_hid_lock - the lock-key message, byte for byte.
 *
 * The semantics are not symmetrical and that is the whole point of the suite:
 *
 *   - a lock that is NOT known must be ABSENT from the message, because its
 *     absence is what tells the VM to leave that lock alone
 *     (`ProcessHidRequestMessage_` checks each sub-message pointer before
 *     touching the corresponding key);
 *   - a lock that is known and OFF must be PRESENT with a false, because the
 *     value is what the VM compares against. Omitting a false would read as
 *     "no opinion" - a different instruction with a different effect.
 *
 * Those two lines are one `if` apart in the encoder and produce messages that
 * differ by three bytes, so the mutation checks at the bottom pin them.
 *
 * Compiled with -Wall -Wextra -Werror -O1 by tests/run_tests.sh.
 */
#include <stdio.h>
#include <string.h>

#include "../core/protocol/hid_lock.h"

static int checks = 0, failures = 0;

static void hex(const uint8_t *b, int n, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < n && o + 3 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%02x ", b[i]);
}

static void eq_bytes(const uint8_t *got, int gn,
                     const uint8_t *want, int wn, const char *what)
{
    checks++;
    if (gn != wn || (wn > 0 && memcmp(got, want, (size_t)wn) != 0)) {
        failures++;
        char g[128], w[128];
        hex(got, gn, g, sizeof g);
        hex(want, wn, w, sizeof w);
        printf("  FAIL %-44s got [%s] (%d), expected [%s] (%d)\n",
               what, g, gn, w, wn);
    }
}

static void eq_int(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %-44s got %d, expected %d\n", what, got, want);
    }
}

static void eq_lock(hid_lock_state got, bool known, bool on, const char *what)
{
    checks++;
    if (got.known != known || (known && got.on != on)) {
        failures++;
        printf("  FAIL %-44s got known=%d on=%d, expected known=%d on=%d\n",
               what, got.known, got.on, known, on);
    }
}

int main(void)
{
    printf("== HID lock keys on the control channel (HID1 2026-10-02) ==\n");
    uint8_t buf[64];

    /* --- nothing known: an EMPTY body, which is legitimate -------------- */
    {
        hid_locks in = {{false, false}, {false, false}, {false, false}};
        const int n = hid_lock_build(buf, sizeof buf, &in);
        eq_int(n, 0, "no opinion gives an empty body");
    }

    /* --- one lock, on. Field 2 (caps), wire type 2, 2 bytes: f1 varint 1.
     * tag = 2<<3|2 = 0x12, len = 2, inner tag = 1<<3|0 = 0x08, value 1. */
    {
        hid_locks in = {{false, false}, {true, true}, {false, false}};
        const int n = hid_lock_build(buf, sizeof buf, &in);
        const uint8_t want[] = { 0x12, 0x02, 0x08, 0x01 };
        eq_bytes(buf, n, want, (int)sizeof want, "caps on");
    }

    /* --- the same lock, OFF. PRESENT with a zero, not omitted. */
    {
        hid_locks in = {{false, false}, {true, false}, {false, false}};
        const int n = hid_lock_build(buf, sizeof buf, &in);
        const uint8_t want[] = { 0x12, 0x02, 0x08, 0x00 };
        eq_bytes(buf, n, want, (int)sizeof want, "caps off is sent, not omitted");
    }

    /* --- num (field 1) and scroll (field 3) ----------------------------- */
    {
        hid_locks in = {{true, true}, {false, false}, {false, false}};
        const int n = hid_lock_build(buf, sizeof buf, &in);
        const uint8_t want[] = { 0x0a, 0x02, 0x08, 0x01 };
        eq_bytes(buf, n, want, (int)sizeof want, "num on is field 1");
    }
    {
        hid_locks in = {{false, false}, {false, false}, {true, true}};
        const int n = hid_lock_build(buf, sizeof buf, &in);
        const uint8_t want[] = { 0x1a, 0x02, 0x08, 0x01 };
        eq_bytes(buf, n, want, (int)sizeof want, "scroll on is field 3");
    }

    /* --- all three, in field order ------------------------------------- */
    {
        hid_locks in = {{true, false}, {true, true}, {true, false}};
        const int n = hid_lock_build(buf, sizeof buf, &in);
        const uint8_t want[] = { 0x0a, 0x02, 0x08, 0x00,
                                 0x12, 0x02, 0x08, 0x01,
                                 0x1a, 0x02, 0x08, 0x00 };
        eq_bytes(buf, n, want, (int)sizeof want, "all three, ascending fields");
    }

    /* --- the refusals --------------------------------------------------- */
    {
        hid_locks in = {{true, true}, {true, true}, {true, true}};
        eq_int(hid_lock_build(NULL, sizeof buf, &in), -1, "no output buffer");
        eq_int(hid_lock_build(buf, sizeof buf, NULL), -1, "no input");
        /* Twelve bytes are needed for three locks; eleven must refuse rather
         * than write a message missing its last lock. */
        eq_int(hid_lock_build(buf, 11, &in), -1, "one byte short refuses");
        eq_int(hid_lock_build(buf, 12, &in), 12, "exactly enough succeeds");
    }

    /* --- parsing a reply ------------------------------------------------ */
    {
        const uint8_t reply[] = { 0x0a, 0x02, 0x08, 0x01,     /* num on   */
                                  0x12, 0x02, 0x08, 0x00,     /* caps off */
                                  0x1a, 0x02, 0x08, 0x01 };   /* scroll on */
        hid_locks out;
        uint32_t seen = 0;
        checks++;
        if (!hid_lock_parse_reply(reply, sizeof reply, &out, &seen)) {
            failures++;
            printf("  FAIL a well-formed reply did not parse\n");
        }
        eq_lock(out.num,    true, true,  "reply: num on");
        eq_lock(out.caps,   true, false, "reply: caps off");
        eq_lock(out.scroll, true, true,  "reply: scroll on");
        eq_int((int)seen, (1 << 1) | (1 << 2) | (1 << 3), "the fields actually seen");
    }

    /* A reply naming only one lock leaves the other two UNKNOWN - not false.
     * "The VM did not say" and "the VM said off" are different answers and a
     * client that shows an indicator must be able to tell them apart. */
    {
        const uint8_t reply[] = { 0x12, 0x02, 0x08, 0x01 };
        hid_locks out;
        checks++;
        if (!hid_lock_parse_reply(reply, sizeof reply, &out, NULL)) {
            failures++; printf("  FAIL partial reply did not parse\n");
        }
        eq_lock(out.caps,   true,  true,  "partial: caps known");
        eq_lock(out.num,    false, false, "partial: num stays UNKNOWN");
        eq_lock(out.scroll, false, false, "partial: scroll stays UNKNOWN");
    }

    /* An empty reply states nothing, and is not an error. */
    {
        hid_locks out;
        checks++;
        if (!hid_lock_parse_reply((const uint8_t *)"", 0, &out, NULL)) {
            failures++; printf("  FAIL an empty reply must parse\n");
        }
        eq_lock(out.num, false, false, "empty reply: nothing known");
    }

    /* A lock sub-message with no bool in it: known, and off. The VM is not
     * expected to send this; dropping the lock instead would lose the one piece
     * of information that DID arrive, namely that the VM named it. */
    {
        const uint8_t reply[] = { 0x12, 0x00 };
        hid_locks out;
        checks++;
        if (!hid_lock_parse_reply(reply, sizeof reply, &out, NULL)) {
            failures++; printf("  FAIL empty sub-message did not parse\n");
        }
        eq_lock(out.caps, true, false, "a lock with no bool is known and off");
    }

    /* Unknown fields are skipped, and the locks around them still read. This is
     * what lets the `[C70]` field numbers be wrong without the parser lying. */
    {
        const uint8_t reply[] = { 0x40, 0x07,                 /* f8 varint   */
                                  0x12, 0x02, 0x08, 0x01,     /* caps on     */
                                  0x4a, 0x03, 'a', 'b', 'c' }; /* f9 bytes   */
        hid_locks out;
        uint32_t seen = 0;
        checks++;
        if (!hid_lock_parse_reply(reply, sizeof reply, &out, &seen)) {
            failures++; printf("  FAIL unknown fields were not skipped\n");
        }
        eq_lock(out.caps, true, true, "caps read past two unknown fields");
        eq_int((int)(seen & ((1u << 8) | (1u << 9))),
               (1 << 8) | (1 << 9), "the unknown fields are REPORTED, not hidden");
    }

    /* Truncation must be refused, not half-read. */
    {
        const uint8_t trunc[] = { 0x12, 0x05, 0x08 };   /* says 5, has 1 */
        hid_locks out;
        checks++;
        if (hid_lock_parse_reply(trunc, sizeof trunc, &out, NULL)) {
            failures++; printf("  FAIL a truncated reply must not parse\n");
        }
    }

    /* --- hid_lock_differs ----------------------------------------------- */
    {
        hid_locks local = {{true, true}, {true, false}, {false, false}};
        hid_locks vm    = {{true, true}, {true, false}, {true, true}};
        checks++;
        if (hid_lock_differs(&local, &vm)) {
            failures++; printf("  FAIL identical known locks must not differ\n");
        }
        local.caps.on = true;
        checks++;
        if (!hid_lock_differs(&local, &vm)) {
            failures++; printf("  FAIL a disagreeing lock must be detected\n");
        }
        /* A lock only one side knows is not a disagreement: there is nothing to
         * reconcile, and reporting one would send a message per poll for ever. */
        hid_locks only_local = {{true, true}, {false, false}, {false, false}};
        hid_locks nothing    = {{false, false}, {false, false}, {false, false}};
        checks++;
        if (hid_lock_differs(&only_local, &nothing)) {
            failures++; printf("  FAIL an unknown lock is not a disagreement\n");
        }
    }

    /* === MUTATION CHECKS ================================================ */

    /* 1. "Skip a lock that is off" - the optimisation someone will make,
     *    because it looks like it saves three bytes. It changes the MEANING:
     *    the VM then leaves that lock as it is instead of turning it off. */
    {
        hid_locks off_only = {{false, false}, {true, false}, {false, false}};
        const int n = hid_lock_build(buf, sizeof buf, &off_only);
        checks++;
        if (n == 0) {
            failures++;
            printf("  FAIL a lock known to be OFF must still be sent\n");
        }
    }

    /* 2. The field numbers must stay distinct and ascending. Collapse two of
     *    them and the three-lock message above still has twelve bytes, but two
     *    locks address the same key. */
    checks++;
    if (!(HID_LOCK_F_NUM < HID_LOCK_F_CAPS && HID_LOCK_F_CAPS < HID_LOCK_F_SCROLL)) {
        failures++;
        printf("  FAIL the three lock fields must be distinct and ordered\n");
    }

    /* 3. The Request oneof field. `DispatchControlMessage_` computes
     *    `case = field - 2` and reaches this handler on case 4, so 6 is not a
     *    preference - it is the only value the server answers. */
    eq_int(HID_REQ_FIELD, 6, "the Hid request is Request field 6");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
