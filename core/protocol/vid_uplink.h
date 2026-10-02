/* vid_uplink - the two RULES our video uplink must obey, as pure functions.
 *
 * Header-only and PURE: no state, no I/O, no getenv, no clock of its own (the
 * caller passes the time in). That is what earns it a test in
 * tests/run_tests.sh (test_vid_uplink.c).
 *
 * Both rules were read off the server binary on 2026-10-02 and both were being
 * broken. They lived inline in `session_video_feedback_tick`, where nothing
 * could reach them: a rule that cannot be tested is a rule that drifts.
 * Details, with addresses: halyard-lab/notes/findings/server-vs-halyard.md
 * §1 and §3; summary in KB.md §9 (2026-10-02, SRV1/SRV3).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === SRV1 - field 3 of the `gE` feedback ====================================
 *
 * === RETRACTED 2026-10-02, THE SAME DAY IT WAS WRITTEN ======================
 *
 * What the server does with field 3 was read correctly off ShadowStreamer
 * 6.3.1 and still holds:
 *   sub_140C041E0 : now_us = clock(); if (now_us > f3) estimator(f1, f2, now_us - f3)
 *   sub_140BFA710 : sliding-window MEAN of those ages, published at est+96
 *   sub_140BFA270 : est+96 / 1000 -> the RTT in ms the server attributes to us
 *
 * What was WRONG is the other half: what this client was already sending there.
 * The SRV1 note asserted it was "g_last_frame_id - a small counter", so the
 * server would be computing its own uptime as our RTT. That assertion was never
 * checked, and the repository contradicted it two lines from the assignment:
 *   - `vid_reasm.c:2226` sets `g_last_frame_id = wh.frame_id` (bytes 6-9 of the
 *     video chunk header), and the V9 comment immediately below says
 *     "Bytes 6-9 are a send timestamp in microseconds";
 *   - the declaration (`vid_reasm.c:269`) calls it the value "echoed back in gE
 *     feedback packets".
 * Measured (SRV1-AB, four alternating runs): with the echo restored, field 3
 * KEEPS RUNNING BETWEEN SESSIONS - 139.7 M us at one session's t=10 s, 476.2 M
 * at the next, a gap of 336.5 s against ~340 s of wall clock. A per-channel
 * frame counter would restart at each session. It does not. Bytes 6-9 are a
 * clock, and the old code was echoing the SERVER's own send timestamp back to
 * it, which is exactly what `now_us - f3` is designed to consume: a genuine
 * round trip, measured in the server's own clock domain.
 *
 * So the echo is correct and `vid_uplink_ge_f3()` below is the regression: it
 * substitutes a clock in OUR domain, which the server then differences against
 * ITS clock, yielding a constant offset equal to the streamer's uptime at
 * session start (inferred at ~130 s in the A/B, against ~20-70 ms for the
 * echo). The same failure the note attributed to the old code is produced by
 * the new one.
 *
 * The A/B found no behavioural difference on a clean link (zero loss, zero
 * NACKs, bitrate pinned at the 20 Mb/s ceiling in all four runs), which is
 * expected: a rate controller with no congestion to react to gives the same
 * answer whatever its delay estimate reads. That is why the code evidence
 * decides this and the measurement does not.
 *
 * THEREFORE: `SHADOW_GE_TS` now defaults to **0** = echo the server's
 * timestamp, the behaviour this client always had. `SHADOW_GE_TS=1` selects the
 * session-relative clock below, kept only so the question stays falsifiable -
 * it is NOT a fix and must not be promoted without a congested-link A/B.
 *
 * Still open, and it needs the IDA database rather than the client: whether the
 * streamer's chunk-send stamp and the `clock()` in `sub_140C041E0` are the same
 * steady clock. If they are, the echo is exact.
 *
 * `t0_us` is the session origin and `armed` says whether it has been set; both
 * are owned by the caller and filled HERE on the first call (pass zeroed
 * fields).
 *
 * `armed` is a SEPARATE flag and not `*t0_us == 0`, which is what the first
 * version used: a session whose clock legitimately read 0 would have re-armed
 * the origin on every call and reported 0 forever. CLOCK_MONOTONIC makes that
 * unlikely rather than impossible, and a sentinel that collides with a valid
 * value is the defect family this repo keeps paying for - test_vid_uplink.c
 * caught it on the first run.
 */
static inline uint32_t vid_uplink_ge_f3(int64_t now_us, int64_t *t0_us, int *armed)
{
    if (!t0_us || !armed) return 0;
    if (!*armed) { *t0_us = now_us; *armed = 1; }
    /* u32 of microseconds wraps every ~71 min. The server differences
     * consecutive samples, so a wrap costs one sample, not a session. */
    return (uint32_t)(uint64_t)(now_us - *t0_us);
}

/* === SRV3 - the key-frame request counter ===================================
 *
 * `sub_140C12F70` @0x140c12f70 accepts an `iP` only when
 *     N > stored  ||  stored == 0xFFFF
 * and then stores N. There is no other escape: a counter that does not increase
 * is discarded with no log on either side. `0xFFFF` is the sentinel a fresh
 * client gets from its constructor (`*(_DWORD*)(this+520) = -1`) and that an
 * `A` registration restores; `0xFFFE` is the "served" value the `A` path writes
 * at +522.
 *
 * So our counter must never reach 0xFFFE: past it, every request for the rest of
 * the session is ignored. Returns true when the caller must re-register (which
 * resets the server's gate AND forces a reference frame) before sending.
 */
static inline int vid_uplink_ifr_needs_rereg(uint16_t counter)
{
    return counter >= 0xFFFEu;
}

/* The value to hold the counter at when a re-registration was NOT possible:
 * one below the sentinels, so we never emit 0xFFFE or 0xFFFF ourselves. */
#define VID_UPLINK_IFR_HOLD 0xFFFDu

/* True if `n` is a counter value the server would accept, given the last one it
 * stored. Expresses the server's test exactly, so a change to our emission can
 * be checked against it rather than against a comment. */
static inline int vid_uplink_ifr_accepted(uint16_t n, uint16_t stored)
{
    return n > stored || stored == 0xFFFFu;
}

#ifdef __cplusplus
}
#endif
