/* hid_lock - Caps Lock, Num Lock and Scroll Lock, agreed with the VM.
 *
 * === HID1 2026-10-02 — WHAT THE VM ACTUALLY OFFERS, AND WHAT IT DOES NOT ====
 *
 * Looking for "USB device forwarding" in ShadowStreamer 6.3.1 finds something
 * different and smaller, but real: the VM does NOT tunnel raw USB. It installs
 * one purpose-built virtual driver per device class, and the streamer drives
 * them:
 *
 *   ShadowVirtualHid/BladeSysVirtHID.sys    mouse, keyboard, a vendor pipe
 *   ShadowVirtualGamepad/ViGEmBus.sys       Xbox 360 / DS4 pads
 *   ShadowVirtualAudio, ShadowVirtualSpeaker, ShadowVirtualStorage
 *
 * `BladeSysVirtHID.sys` carries four HID report descriptors, read out of the
 * driver (halyard-lab/notes/hid_BladeSysVirtHID.txt):
 *
 *   ReportID  3   mouse, ABSOLUTE: 5 buttons, X/Y as int16 -32768..32767,
 *                 wheel int8, AC Pan int8
 *   ReportID  4   mouse, RELATIVE: 5 buttons, X/Y/wheel as int8, AC Pan int8
 *   ReportID  7   keyboard: 8 modifier bits + an array of 6 key codes
 *   ReportID 64   VENDOR, 64 bytes IN and 64 bytes OUT - a raw bidirectional
 *                 pipe, and the only thing in the whole surface that is not
 *                 tied to one device class
 *
 * The two mouse descriptors are worth noticing on their own: absolute and
 * relative are TWO DIFFERENT DEVICES on the VM side, which is what
 * `SHADOW_INPUT_ABS` has been switching between all along without anybody
 * knowing there were two report IDs behind it.
 *
 * ReportID 64 is the lead for "plug any peripheral", and it is NOT what this
 * module does — the streamer reaches it with `HidD_SetOutputReport`, i.e. from
 * the VM side, and no client-side message carrying 64 raw bytes has been found
 * yet. That stays open rather than guessed at.
 *
 * === WHAT THIS MODULE DOES =================================================
 *
 * `ProcessHidRequestMessage_` @0x140b30330 is the one HID handler reachable
 * from the control channel, and it is the LOCK KEYS. Decompiled, it does
 * exactly this, per lock:
 *
 *   - read the state the client sent;
 *   - compare it with the VM's own, `GetKeyState(VK_NUMLOCK / VK_CAPITAL /
 *     VK_SCROLL)`;
 *   - if they differ, call `VMUtils::Utilities::Keyboard::ToggleKey(vk)`;
 *   - reply with the state the VM now has.
 *
 * A lock the client does NOT send is left alone - that is why each lock is a
 * sub-message rather than a bare bool: the presence of the sub-message is what
 * says "I have an opinion about this one".
 *
 * It fixes a real annoyance and a mechanical one: with Caps Lock on locally and
 * off in the VM, everything you type arrives in the wrong case, and nothing on
 * either screen explains why. The same message is also how a client learns the
 * VM's lock states in order to show them.
 *
 * === CONFIDENCE, HONESTLY ==================================================
 *
 * `[C95]` the handler, the three locks, the toggle-to-match semantics and the
 * Request oneof field: `DispatchControlMessage_` @0x140b2ca30 computes
 * `case = field - 2` and its `case 4` calls this handler, so the field is 6.
 *
 * `[C98]` the field numbers INSIDE the Hid message - inferred from the memory
 * layout, then MEASURED against the live server on 2026-10-02. The reply to a
 * request asserting all three locks came back as `f6:len6`, six bytes being
 * three sub-messages of `08 00`, with fields 1, 2 and 3 present. So
 * numlock = 1, capslock = 2, scrolllock = 3, and the bool inside each is
 * field 1. `hid_lock_parse_reply()` still walks whatever arrives and still
 * reports the fields it saw: the cost is one bitmask and it is what turned the
 * guess into a number.
 *
 * `[C95]` AND A RULE THAT IS NOT GUESSABLE: **an empty Hid request answers
 * nothing.** Measured in the same sitting - a request with an empty Hid body
 * got `f6:len0` back, no lock reported at all. The decompile explains it: the
 * handler reads the FIRST field slot of the incoming Hid and, when it is null,
 * jumps to the reply without creating any of the three lock sub-messages.
 *
 * There is therefore **no way to read the VM's lock states without asserting
 * your own**, and asserting one makes the VM toggle its key to match. Reading
 * and writing are the same operation here. A client that wants to DISPLAY the
 * VM's lock state has to accept that asking changes it - which is worth
 * knowing before building an indicator on top of this.
 *
 * Pure: no state, no I/O, no getenv. Tested by tests/test_hid_lock.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Request oneof field that carries the Hid message. `[C95]` */
#define HID_REQ_FIELD 6

/* The three locks, as field numbers inside the Hid message. Measured, see the
 * header. Named anyway, so that a future version is one edit in one place. */
#define HID_LOCK_F_NUM    1   /* [C98] measured */
#define HID_LOCK_F_CAPS   2   /* [C98] measured */
#define HID_LOCK_F_SCROLL 3   /* [C98] measured */

/* The bool inside each lock sub-message. Measured: the reply's sub-messages are
 * `08 00`, i.e. field 1, varint. */
#define HID_LOCK_F_STATE  1   /* [C98] measured */

/* One lock, as this client sees it. `known == false` means "no opinion": the
 * lock is then left out of the message entirely and the VM does not touch it,
 * which is the whole reason each lock is a sub-message. */
typedef struct {
    bool known;
    bool on;
} hid_lock_state;

typedef struct {
    hid_lock_state num;
    hid_lock_state caps;
    hid_lock_state scroll;
} hid_locks;

/* Builds the BODY of the Hid request - what goes inside Request field 6, not
 * the whole Message. The caller wraps it, the way `ctrl_msgs.c` wraps every
 * other request, so this module needs to know nothing about sequence numbers.
 *
 * Returns the number of bytes written, or -1 on a bad argument or no room.
 * Returns 0 when no lock is known. REFUTED 2026-10-02: an empty body was
 * believed to be how a client asks "what are yours?". It is not - the server
 * answers an empty Hid with an empty Hid, reporting nothing (see the header).
 * An empty body is therefore a message with no effect, kept only because
 * refusing it here would hide the asymmetry rather than document it. */
static inline int hid_lock_build(uint8_t *out, size_t cap, const hid_locks *in)
{
    if (!out || !in) return -1;

    int off = 0;
    /* `uint32_t`, matching `pb_write_submsg`'s parameter. As `int` it compiled
     * and worked - the values are 1, 2, 3 - but it is an implicit signed to
     * unsigned conversion on a field number, which is the kind of thing that
     * stops being harmless the day a number is computed rather than written. */
    const struct { uint32_t field; const hid_lock_state *st; } LOCKS[3] = {
        { HID_LOCK_F_NUM,    &in->num },
        { HID_LOCK_F_CAPS,   &in->caps },
        { HID_LOCK_F_SCROLL, &in->scroll },
    };

    for (int i = 0; i < 3; i++) {
        if (!LOCKS[i].st->known) continue;
        /* The inner message: one bool. Written even when false - the VALUE is
         * what the VM compares against, and omitting a false would read as
         * "no opinion", which is a different instruction. */
        uint8_t inner[4];
        const int n = pb_write_bool(inner, sizeof inner, 0,
                                    HID_LOCK_F_STATE, LOCKS[i].st->on);
        if (n < 0) return -1;
        off = pb_write_submsg(out, cap, off, LOCKS[i].field, inner, (size_t)n);
        if (off < 0) return -1;
    }
    return off;
}

/* Reads a Hid reply body (the contents of the reply's Hid field) into `out`.
 *
 * Tolerant on purpose: an unknown field is skipped, a lock that is absent stays
 * `known == false`, and a lock sub-message whose bool is missing is reported as
 * known-but-off rather than dropped - the VM only ever sends a lock it has a
 * state for. Returns true when the body parsed without running off the end.
 *
 * It does NOT require the field numbers to be the `[C70]` guesses: it reports
 * which fields it actually saw through `seen_fields`, so a single session says
 * whether the guess was right. Pass NULL if you do not care. */
static inline bool hid_lock_parse_reply(const uint8_t *body, size_t len,
                                        hid_locks *out, uint32_t *seen_fields)
{
    if (seen_fields) *seen_fields = 0;
    if (!out) return false;
    out->num.known = out->caps.known = out->scroll.known = false;
    out->num.on = out->caps.on = out->scroll.on = false;
    if (!body && len > 0) return false;
    if (len == 0) return true;            /* an empty reply states nothing */

    int off = 0;
    while ((size_t)off < len) {
        uint32_t field = 0, wt = 0;
        const int after_tag = pb_read_tag(body, len, off, &field, &wt);
        if (after_tag < 0) return false;

        if (wt == 2) {
            const uint8_t *sub = NULL;
            size_t sl = 0;
            const int after = pb_read_lendelim(body, len, after_tag, &sub, &sl);
            if (after < 0) return false;
            if (seen_fields && field < 32) *seen_fields |= (uint32_t)1u << field;

            hid_lock_state *target =
                  (field == HID_LOCK_F_NUM)    ? &out->num
                : (field == HID_LOCK_F_CAPS)   ? &out->caps
                : (field == HID_LOCK_F_SCROLL) ? &out->scroll
                : NULL;
            if (target) {
                target->known = true;
                target->on = false;
                /* Walk the sub-message for the state bool. One field today, so
                 * a loop looks like too much - but a reply that one day carries
                 * a second field must not hide the first. */
                int so = 0;
                while ((size_t)so < sl) {
                    uint32_t sf = 0, swt = 0;
                    const int sat = pb_read_tag(sub, sl, so, &sf, &swt);
                    if (sat < 0) return false;
                    if (sf == HID_LOCK_F_STATE && swt == 0) {
                        uint64_t v = 0;
                        const int sa = pb_read_varint(sub, sl, sat, &v);
                        if (sa < 0) return false;
                        target->on = v != 0;
                        so = sa;
                    } else {
                        const int sa = pb_skip_field(sub, sl, sat, swt);
                        if (sa < 0) return false;
                        so = sa;
                    }
                }
            }
            off = after;
        } else {
            if (seen_fields && field < 32) *seen_fields |= (uint32_t)1u << field;
            const int after = pb_skip_field(body, len, after_tag, wt);
            if (after < 0) return false;
            off = after;
        }
    }
    return true;
}

/* True when the VM would have to toggle at least one lock to match `local`.
 * For a caller that only wants to send the message when it would change
 * something - the VM's answer is idempotent, but a message per frame is not. */
static inline bool hid_lock_differs(const hid_locks *local, const hid_locks *vm)
{
    if (!local || !vm) return false;
    const hid_lock_state *l[3] = { &local->num, &local->caps, &local->scroll };
    const hid_lock_state *r[3] = { &vm->num,    &vm->caps,    &vm->scroll };
    for (int i = 0; i < 3; i++)
        if (l[i]->known && r[i]->known && l[i]->on != r[i]->on) return true;
    return false;
}

#ifdef __cplusplus
}
#endif
