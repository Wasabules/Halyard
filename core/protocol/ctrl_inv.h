/* ctrl_inv - which Request oneof field a CtrlChanV2 message carries.
 *
 * Header-only and PURE: no state, no I/O, no getenv. That is what earns it a
 * test in tests/run_tests.sh (test_ctrl_inv.c).
 *
 * Why it is a module of its own: the inventory this feeds
 * (`SHADOW_CTRL_INVENTORY`, ctrl_tcp.c) is how we compare what WE emit on
 * `:base+11` against what the official client emits, and an inventory with a
 * blind spot cannot do that.
 *
 * === SRV7 2026-10-02 - THE MESSAGE THE INVENTORY NEVER COUNTED ===============
 * The first version skipped field 1 (the sequence number) unconditionally,
 * because every message it had been written against carried one. The
 * Capabilities message does NOT (`ctrl_build_capabilities` emits f2 first), so
 * the skip consumed its f2 tag, the `(body[i] >> 3) == 2` test then failed, and
 * the message was counted as NOTHING. The inventory silently undercounted the
 * one message that opens every session.
 *
 * It did NOT produce a phantom count - a first reading of this code claimed it
 * explained the "f1x1" in the official-client inventory quoted in three
 * comments (ctrl_session.c twice, ctrl_tcp.c once), and that was wrong: walk
 * the old code on `12 06 1a 04 ...` and it falls out of the `if` without
 * counting. test_ctrl_inv.c pins both behaviours so the claim cannot drift
 * again. Where "f1x1" came from is still open - it was produced by a
 * capture-analysis tool, not by this code. What IS established server-side is
 * that a Request field 1 has no handler at all in ShadowStreamer 6.3.1
 * (`DispatchControlMessage_` @0x140b2ca30 computes `case = field - 2`, so
 * field 1 underflows into its `default`, which answers an error reply), so
 * whatever that count was, it was not a request kind the server serves.
 *
 * Wire shape (KB §3.6, §3.8):
 *   Message { [f1 varint seq]? , f2 = Request { <ONE oneof field> ... }, f4, f5 }
 * Field 1 is optional; field 2 is the Request and always present on the
 * messages we send.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the Request oneof field number carried by `body`, or -1 when the
 * message does not have the shape above (truncated, no f2, or f2 empty).
 *
 * Deliberately tolerant about what it does NOT need: it never looks past the
 * first tag inside the Request, because the inventory only counts kinds. */
static inline int ctrl_inv_request_field(const uint8_t *body, size_t len)
{
    if (!body) return -1;
    size_t i = 0;

    /* Field 1, varint, ONLY if present. `(tag & 7) == 0` is the varint wire
     * type: a field 1 of any other wire type is not our sequence number and
     * must not be skipped as one. */
    if (i < len && (body[i] >> 3) == 1 && (body[i] & 7) == 0) {
        i++;
        while (i < len && (body[i] & 0x80)) i++;
        if (i >= len) return -1;
        i++;
    }

    /* Field 2, length-delimited: the Request. */
    if (i >= len) return -1;
    if ((body[i] >> 3) != 2 || (body[i] & 7) != 2) return -1;
    i++;

    /* Its length, which we skip: the oneof tag is the next byte either way. */
    while (i < len && (body[i] & 0x80)) i++;
    if (i >= len) return -1;
    i++;

    if (i >= len) return -1;
    return (int)(body[i] >> 3);
}

#ifdef __cplusplus
}
#endif
