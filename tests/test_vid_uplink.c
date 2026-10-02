/* test_vid_uplink - the three SRV rules that had no coverage at all.
 *
 * SRV1 (gE field 3), SRV3 (the key-frame counter) and SRV6 (the axis int16).
 * Each was read off ShadowStreamer 6.3.1 and each is stated here as an
 * EXECUTABLE form of the server's own test, so a future change is checked
 * against the server's arithmetic rather than against a comment restating it.
 *
 * Compiled with -Wall -Wextra -Werror -O1 by tests/run_tests.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../core/protocol/vid_uplink.h"
#include "../core/protocol/gamepad_wire.h"
#include "../core/protocol/ann_reply.h"
#include "../core/protocol/ctrl_msgs_chan.h"

static int checks = 0, failures = 0;

static void eq(const char *what, long long got, long long want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %s: got %lld, expected %lld\n", what, got, want);
    }
}
static void ok(const char *what, int cond)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL %s\n", what); }
}

int main(void)
{
    /* ===== SRV1 - gE field 3 ============================================= */
    {
        int64_t t0 = 0; int armed = 0;
        /* The origin is armed on the first call, so the first sample is 0 - and
         * NOT the raw clock, which is the whole point: the server drops any
         * sample whose timestamp is above its own uptime. */
        eq("ge_f3 first sample is 0",     vid_uplink_ge_f3(1764123456789012LL, &t0, &armed), 0);
        ok("ge_f3 armed the origin",      t0 == 1764123456789012LL);
        /* Then it is the elapsed microseconds, monotonically. */
        eq("ge_f3 +50 ms",                vid_uplink_ge_f3(1764123456839012LL, &t0, &armed), 50000);
        eq("ge_f3 +1 s",                  vid_uplink_ge_f3(1764123457789012LL, &t0, &armed), 1000000);
        /* Monotonic over a long run. */
        {
            uint32_t prev = 0; int monotonic = 1;
            for (int i = 1; i <= 2000; i++) {
                const uint32_t v = vid_uplink_ge_f3(t0 + (int64_t)i * 50000, &t0, &armed);
                if (v <= prev) monotonic = 0;
                prev = v;
            }
            ok("ge_f3 monotonic over 2000 samples (100 s)", monotonic);
        }
        /* NOTE (SRV1 retracted 2026-10-02): this function is NOT the default
         * any more. `SHADOW_GE_TS` defaults to 0 = echo the server's own send
         * timestamp, which is what field 3 is built to carry; this
         * session-relative clock measured as a regression. See
         * vid_uplink.h §SRV1. The checks stay because the function stays
         * reachable behind the toggle, and a reachable function with no test
         * is how the wrong premise survived in the first place. */
        ok("ge_f3 grows into the millions of us over a session",
           vid_uplink_ge_f3(t0 + 100000000LL, &t0, &armed) >= 100000000u);
        /* A NULL origin must not crash nor invent a value. */
        eq("ge_f3 NULL origin -> 0",      vid_uplink_ge_f3(123, NULL, &armed), 0);
        eq("ge_f3 NULL armed  -> 0",      vid_uplink_ge_f3(123, &t0, NULL), 0);
        /* The u32 wrap is tolerated by design (the server differences
         * consecutive samples): it must wrap, not saturate or go negative. */
        {
            int64_t w = 0; int wa = 0;
            /* A session whose clock reads exactly 0: the armed flag must hold,
             * or the origin re-arms on every call and the field is stuck at 0.
             * That is the defect this suite found on its first run. */
            eq("ge_f3 at clock 0 arms and returns 0", vid_uplink_ge_f3(0, &w, &wa), 0);
            eq("ge_f3 from a 0 origin still advances", vid_uplink_ge_f3(1234, &w, &wa), 1234);
            eq("ge_f3 wraps at 2^32 us (~71 min)",
               vid_uplink_ge_f3(4294967296LL + 7, &w, &wa), 7);
        }
    }

    /* ===== SRV3 - the key-frame request counter =========================== */
    {
        /* The server's test, stated exactly. A fresh client holds 0xFFFF. */
        ok("fresh client (0xFFFF) accepts anything",
           vid_uplink_ifr_accepted(1, 0xFFFF));
        ok("strictly greater is accepted",     vid_uplink_ifr_accepted(2, 1));
        ok("equal is REFUSED",                !vid_uplink_ifr_accepted(2, 2));
        ok("smaller is REFUSED",              !vid_uplink_ifr_accepted(1, 2));
        /* Which is why a wrapped counter is silently ignored for the rest of
         * the session - the defect SRV3 exists to prevent. */
        ok("a wrapped counter (1 after 65533) is REFUSED",
           !vid_uplink_ifr_accepted(1, 65533));

        /* Our own obligation: re-register before reaching the sentinels. */
        ok("no re-registration needed at 1",        !vid_uplink_ifr_needs_rereg(1));
        ok("none at 0xFFFD",                        !vid_uplink_ifr_needs_rereg(0xFFFD));
        ok("needed at 0xFFFE (the served value)",    vid_uplink_ifr_needs_rereg(0xFFFE));
        ok("needed at 0xFFFF (the sentinel)",        vid_uplink_ifr_needs_rereg(0xFFFF));
        /* The hold value must never be one of the two sentinels. */
        ok("hold value is below both sentinels",
           VID_UPLINK_IFR_HOLD < 0xFFFEu);
        /* Walking the whole range: every value we would emit is accepted by the
         * server's test against the previous one. */
        {
            int all_ok = 1;
            uint16_t stored = 0xFFFF;         /* fresh client */
            for (uint32_t n = 1; n < 0xFFFE; n++) {
                if (!vid_uplink_ifr_accepted((uint16_t)n, stored)) { all_ok = 0; break; }
                stored = (uint16_t)n;
            }
            ok("1..0xFFFD are all accepted in sequence", all_ok);
        }
    }

    /* ===== SRV6 - the axis int16 ========================================== */
    {
        uint8_t b[GAMEPAD_WIRE_BODY_LEN];

        /* The claim the finding rests on: our 8-bit encoding round-trips
         * through the SERVER's transform for every one of the 256 values. */
        int round_trip = 1, first_bad = -1;
        for (int v = 0; v <= 255; v++) {
            gamepad_wire_build_axis8(b, 4, (uint8_t)v);
            const uint8_t back = gamepad_wire_server_axis_value(b, true);
            if (back != (uint8_t)v) { round_trip = 0; if (first_bad < 0) first_bad = v; }
        }
        ok("8-bit axis round-trips for all 256 values", round_trip);
        if (!round_trip) printf("    first mismatch at v=%d\n", first_bad);

        /* The three anchors quoted in the finding. */
        gamepad_wire_build_axis8(b, 4, 0);
        eq("v=0   -> i16 -32768", (int16_t)((uint16_t)b[12] | ((uint16_t)b[13] << 8)), -32768);
        gamepad_wire_build_axis8(b, 4, 128);
        eq("v=128 -> i16 128",    (int16_t)((uint16_t)b[12] | ((uint16_t)b[13] << 8)), 128);
        gamepad_wire_build_axis8(b, 4, 255);
        eq("v=255 -> i16 32767",  (int16_t)((uint16_t)b[12] | ((uint16_t)b[13] << 8)), 32767);

        /* And that it is byte-for-byte what this client has always sent:
         * b[12] = v, b[13] = v + 128. A change here would silently alter a
         * wire that 2598 decrypted packets confirmed. */
        int historical = 1;
        for (int v = 0; v <= 255; v++) {
            gamepad_wire_build_axis8(b, 2, (uint8_t)v);
            if (b[12] != (uint8_t)v || b[13] != (uint8_t)((v + 128) & 0xFF)) historical = 0;
        }
        ok("8-bit encoding is byte-identical to the captured wire", historical);

        /* Envelope: version, kind and index, and nothing else set. */
        gamepad_wire_build_axis16(b, 3, 1234);
        eq("byte 0 = protocol version 4", b[0], 0x04);
        eq("byte 2 = kind 1 (axis)",      b[2], 0x01);
        eq("byte 11 = axis index",        b[11], 3);
        eq("byte 1 = device id 0 (the server allocates it)", b[1], 0);
        {
            int others_zero = 1;
            const int used[] = {0, 2, 11, 12, 13};
            for (int i = 0; i < GAMEPAD_WIRE_BODY_LEN; i++) {
                int is_used = 0;
                for (size_t k = 0; k < sizeof(used)/sizeof(used[0]); k++)
                    if (used[k] == i) is_used = 1;
                if (!is_used && b[i] != 0) others_zero = 0;
            }
            ok("every other byte is zero", others_zero);
        }
        /* 16-bit: the full range survives, which is the point of SRV6 - the
         * 8-bit path uses 256 of the 65,536 values the wire carries. */
        gamepad_wire_build_axis16(b, 0, -32768);
        eq("i16 -32768 low",  b[12], 0x00);
        eq("i16 -32768 high", b[13], 0x80);
        gamepad_wire_build_axis16(b, 0, 32767);
        eq("i16 32767 low",   b[12], 0xFF);
        eq("i16 32767 high",  b[13], 0x7F);
        {
            int distinct = 0;
            uint8_t seen_lo = 0, seen_hi = 0;
            gamepad_wire_build_axis16(b, 0, 1000);  seen_lo = b[12]; seen_hi = b[13];
            gamepad_wire_build_axis16(b, 0, 1001);
            if (b[12] != seen_lo || b[13] != seen_hi) distinct = 1;
            ok("adjacent 16-bit values are distinct on the wire", distinct);
        }
        /* NULL must not crash. */
        gamepad_wire_build_axis16(NULL, 0, 0);
        checks++;   /* reaching here is the check */
    }

    /* ===== SRV5 - the two channel orders are a bijection ================== */
    {
        /* `SHADOW_CHAN_IDX_*` indexes OUR announcement bodies; `ann_channel_t`
         * is the server's channel number. Confusing them re-announces the wrong
         * channel, which on a working channel hands the server a duplicate
         * stream. The mapping must be a bijection onto 0..7. */
        const int chans[8] = {
            ANN_CHAN_VIDEO, ANN_CHAN_AUDIO, ANN_CHAN_INPUT, ANN_CHAN_CURSOR,
            ANN_CHAN_MICRO, ANN_CHAN_GAMEPAD, ANN_CHAN_CLIPBOARD, ANN_CHAN_FILEXFER
        };
        int seen[8] = {0};
        int bijection = 1;
        for (int i = 0; i < 8; i++) {
            const int bi = shadow_chan_idx_from_ann(chans[i]);
            if (bi < 0 || bi > 7 || seen[bi]) { bijection = 0; break; }
            seen[bi] = 1;
        }
        ok("server channel -> body index is a bijection onto 0..7", bijection);

        /* The four that matter by name, as derived from the captured body tags. */
        eq("VIDEO  -> idx 0", shadow_chan_idx_from_ann(ANN_CHAN_VIDEO),  SHADOW_CHAN_IDX_VIDEO);
        eq("AUDIO  -> idx 3", shadow_chan_idx_from_ann(ANN_CHAN_AUDIO),  SHADOW_CHAN_IDX_AUDIO);
        eq("CURSOR -> idx 1", shadow_chan_idx_from_ann(ANN_CHAN_CURSOR), SHADOW_CHAN_IDX_CURSOR);
        eq("INPUT  -> idx 2", shadow_chan_idx_from_ann(ANN_CHAN_INPUT),  SHADOW_CHAN_IDX_INPUT);
        /* AUDIO is NOT 1: that was the first reading, and a re-announcement of
         * index 1 would have hit the cursor channel instead. */
        ok("AUDIO is not body index 1", SHADOW_CHAN_IDX_AUDIO != 1);
        /* An unknown channel number must be refused, not mapped to 0 (= video). */
        eq("unknown channel -> -1",  shadow_chan_idx_from_ann(0),  -1);
        eq("out of range -> -1",     shadow_chan_idx_from_ann(99), -1);
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
