/* bitrate.h - the ONE ladder of bitrate values, and the ONE rule that says
 * which one is in force. PURE: no state, no I/O, no getenv, no clock.
 *
 * === WHY THIS FILE EXISTS (B1, 2026-09-02) ===
 *
 * The same quantity - "how many Mbps do we ask the server for" - was described
 * in FIVE places that did not agree:
 *
 *   quality_view.cpp   auto, 5, 10, 15, 20, 30, 40, 50, 80, 100, 150
 *   settings_view.cpp        5, 10, 15, 20, 25, 30, 40, 50, 75, 100
 *   stream_activity.cpp auto, 5, 10, 15, 20, 30, 50
 *   settings.cpp        clamped 0..500 for the global value
 *   settings.cpp        clamped 1..150 for the per-link values
 *
 * Three ladders and two ceilings for one number. The user reported it as
 * "nothing is coherent about the limits", and every symptom below follows
 * mechanically from that disagreement:
 *
 *   - CHOOSE 40 Mbps IN THE QUALITY SCREEN, OPEN THE PAUSE MENU, PRESS RIGHT:
 *     you get 5 Mbps. `cycleIndex` does not find 40 in its own shorter ladder,
 *     leaves its index at 0, and steps to the next one.
 *   - CHOOSE 25 Mbps (per-link ladder), LOOK AT THE QUALITY SCREEN: it displays
 *     "Auto". `indexU32` returns 0 on a miss, and index 0 is "Auto". The screen
 *     states the opposite of what is stored.
 *   - CHOOSE "Auto" DURING A SESSION: nothing goes out.
 *     `ctrl_session_set_video_config` reads 0 as "leave unchanged", so the
 *     previous value stays in force while the screen says Auto.
 *
 * None of those three is a rendering bug: they are all one table disagreeing
 * with another. So there is now one table, and everything reads it.
 *
 * === WHAT "AUTO" MEANS, AND WHY IT IS NOT "LET THE SERVER DECIDE" ===
 *
 * There is no such thing on the wire. The video channel announcement ALWAYS
 * carries a `max_bitrate_bps`; when the user picks "Auto" we send the desktop
 * client's hardcoded value, 20 Mbps (KB §2, `ctrl_msgs.h`). The label was
 * therefore lying about a real number. `BITRATE_CLIENT_DEFAULT_MBPS` is that
 * number, defined once, and the UI says so.
 *
 * === WHERE THE LADDER'S CEILING COMES FROM ===
 *
 * Measured, not chosen: on this account the official client negotiates
 * **55 Mbps** at 2560x1440 (70 Mbps on the "unlimited" setting) - KB §9,
 * 2026-08-28. The old 150 Mbps rung was above anything ever observed, and the
 * pause menu's 50 was below what the account really grants. The ladder now
 * spans what the account can actually do, with one rung of headroom.
 *
 * The earlier "server ceiling ~14 Mbps" number is NOT used here: KB §9 records
 * that it was invalidated by S18 - it was measured through a malformed message
 * that wrote the bitrate into the wrong protobuf field, so the server never saw
 * our requests at all.
 */
#ifndef SHADOW_BITRATE_H
#define SHADOW_BITRATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What "Auto" resolves to on the wire: the desktop client's hardcoded value.
 * ctrl_session.c uses this same constant for its announcement default, so the
 * screen and the wire can no longer drift apart. */
#define BITRATE_CLIENT_DEFAULT_MBPS 20u

/* The frame rate's default, next to the bitrate's so that every reader takes it
 * from one place (B1): split across two files, the announcement said 143.85
 * while the mid-session resend said 142.0.
 * CFG-4 2026-09-11: this used to say the frame rate travels in the same message
 * as the bitrate. It travels ONLY in the video channel announcement - the live
 * message (`VideoEncodingConfig`, kUpdateSession f13) has had no frame-rate
 * field since S18, so nothing sends one mid-session any more. */
#define FPS_CLIENT_DEFAULT 143.85f

/* 0 = "Auto" (= BITRATE_CLIENT_DEFAULT_MBPS on the wire). The rest is the
 * ladder offered everywhere: settings, quality screen, pause menu. */
static const uint32_t BITRATE_LADDER[] = {
    0, 5, 10, 15, 20, 25, 30, 40, 50, 60, 80, 100
};
#define BITRATE_LADDER_N ((int)(sizeof BITRATE_LADDER / sizeof BITRATE_LADDER[0]))
#define BITRATE_MAX_MBPS 100u

/* Index of `mbps` in the ladder. NEVER returns a silent 0 for a value that is
 * not on it: an off-ladder value returns its NEAREST rung.
 *
 * That is the whole point. The previous helpers returned 0 on a miss, and index
 * 0 is "Auto" - so a screen displayed "Auto" while 25 Mbps was stored, and the
 * next press wrote that lie back into the settings. A nearest-rung answer is
 * wrong by at most one notch and never changes the meaning. */
static inline int bitrate_index(uint32_t mbps)
{
    int best = 0;
    uint32_t best_d = 0xffffffffu;
    if (mbps == 0) return 0;              /* Auto is exact, never "nearest" */
    for (int i = 1; i < BITRATE_LADDER_N; i++) {
        const uint32_t v = BITRATE_LADDER[i];
        const uint32_t d = (v > mbps) ? (v - mbps) : (mbps - v);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Snaps a value onto the ladder. Used when loading the settings file, so a
 * value written by an older version - or by hand - can never sit between two
 * rungs and make every screen disagree about where it is. */
static inline uint32_t bitrate_clamp(uint32_t mbps)
{
    if (mbps == 0) return 0;
    if (mbps > BITRATE_MAX_MBPS) return BITRATE_MAX_MBPS;
    return BITRATE_LADDER[bitrate_index(mbps)];
}

/* One notch up (dir >= 0) or down, wrapping. `cur` need not be on the ladder:
 * it is snapped first, so stepping from a stale value moves by one notch
 * instead of jumping to the bottom. */
static inline uint32_t bitrate_step(uint32_t cur, int dir)
{
    const int i = bitrate_index(bitrate_clamp(cur));
    const int n = BITRATE_LADDER_N;
    return BITRATE_LADDER[(i + (dir >= 0 ? 1 : n - 1)) % n];
}

/* What the wire must carry for `mbps`, "Auto" resolved. Never returns 0: the
 * announcement always carries a number. */
static inline uint32_t bitrate_wire_mbps(uint32_t mbps)
{
    return mbps ? bitrate_clamp(mbps) : BITRATE_CLIENT_DEFAULT_MBPS;
}

/* Which link the per-link setting is choosing for. Mirrors device::LinkType
 * without depending on it: this header is C and is compiled into the headless
 * binary, which does not carry the C++ layer. */
typedef enum {
    BITRATE_LINK_UNKNOWN = 0,   /* the service did not answer */
    BITRATE_LINK_WIFI    = 1,
    BITRATE_LINK_ETHERNET = 2
} bitrate_link_t;

/* THE rule: which value is actually in force.
 *
 * `UNKNOWN` means "the network service did not answer", and it returns the
 * GLOBAL value - never a per-link one. Treating a failed query as "Wi-Fi" would
 * throttle a wired link on the strength of a missing answer, which is the exact
 * mistake `device_mode.hpp` warns about for the whole module. */
static inline uint32_t bitrate_effective(int per_link, bitrate_link_t link,
                                         uint32_t global_mbps,
                                         uint32_t wifi_mbps, uint32_t eth_mbps)
{
    if (!per_link) return bitrate_clamp(global_mbps);
    if (link == BITRATE_LINK_ETHERNET) return bitrate_clamp(eth_mbps);
    if (link == BITRATE_LINK_WIFI)     return bitrate_clamp(wifi_mbps);
    return bitrate_clamp(global_mbps);
}

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_BITRATE_H */
