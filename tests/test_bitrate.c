/* test_bitrate.c - the one bitrate ladder and the one rule that resolves it
 * (core/protocol/bitrate.h).
 *
 * Each check names its COUNTER-CASE: the exact input that produced a real
 * symptom before B1. The three at the top are the ones the user reported as
 * "nothing is coherent about the limits" - each of them is one table
 * disagreeing with another, and each of them is silent.
 */
#include <stdio.h>
#include <string.h>
#include "../core/protocol/bitrate.h"

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                 \
    if (!(cond)) { printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, what);   \
                   failures++; }                                              \
} while (0)

int main(void)
{
    printf("== bitrate: one ladder, one rule ==\n");

    /* COUNTER-CASE 1 - THE SCREEN THAT DISPLAYED "Auto" FOR 25 Mbps.
     * `indexU32` returned 0 on a miss, and index 0 is "Auto". The quality
     * screen therefore stated the opposite of what was stored, and the next
     * press wrote that lie back into the settings. */
    CHECK(bitrate_index(25) != 0, "an off-ladder value NEVER reads as Auto");
    CHECK(BITRATE_LADDER[bitrate_index(25)] == 25, "25 is on the ladder now");
    CHECK(bitrate_index(37) != 0, "COUNTER-CASE: 37 does not read as Auto either");
    CHECK(BITRATE_LADDER[bitrate_index(37)] == 40, "37 reads as its nearest rung");
    CHECK(bitrate_index(0) == 0, "Auto itself is exact, not 'nearest'");

    /* COUNTER-CASE 2 - 40 Mbps IN THE QUALITY SCREEN, THEN RIGHT IN THE PAUSE
     * MENU, GAVE 5 Mbps. The pause menu's shorter ladder had no 40; its
     * `cycleIndex` left the index at 0 and stepped to the next rung. */
    CHECK(bitrate_step(40, +1) == 50, "COUNTER-CASE: stepping up from 40 gives 50, not 5");
    CHECK(bitrate_step(40, -1) == 30, "stepping down from 40 gives 30");
    CHECK(bitrate_step(37, +1) == 50, "an off-ladder value is snapped first, then stepped");

    /* Wrapping, both ways, and it must include Auto - it is a rung. */
    CHECK(bitrate_step(BITRATE_LADDER[BITRATE_LADDER_N - 1], +1) == 0,
          "past the top rung we wrap onto Auto");
    CHECK(bitrate_step(0, -1) == BITRATE_LADDER[BITRATE_LADDER_N - 1],
          "below Auto we wrap onto the top rung");
    CHECK(bitrate_step(0, +1) == 5, "one notch above Auto is the lowest rung");

    /* COUNTER-CASE 3 - "Auto" NEVER REACHED THE WIRE.
     * `ctrl_session_set_video_config` read 0 as "leave unchanged", so choosing
     * Auto mid-session kept the previous value while the screen said Auto.
     * There is no "let the server decide" on this wire: the announcement always
     * carries a number, and Auto IS that number. */
    CHECK(bitrate_wire_mbps(0) == BITRATE_CLIENT_DEFAULT_MBPS,
          "COUNTER-CASE: Auto resolves to a real number, it is not 'unchanged'");
    CHECK(bitrate_wire_mbps(0) != 0, "the wire never carries zero");
    CHECK(bitrate_wire_mbps(50) == 50, "an explicit value goes out as itself");
    CHECK(bitrate_wire_mbps(37) == 40, "an off-ladder value is snapped for the wire too");

    /* --- The ceiling: one number, not two ---------------------------------
     * The settings file clamped the global value to 500 and the per-link ones
     * to 150, while no screen offered more than 150 and the account tops out at
     * 70 Mbps (KB §9, 2026-08-28). A value from an older file must land on the
     * ladder rather than sit between two rungs. */
    CHECK(bitrate_clamp(150) == BITRATE_MAX_MBPS, "150, offered by the old ladder, comes back to the ceiling");
    CHECK(bitrate_clamp(500) == BITRATE_MAX_MBPS, "the old 500 clamp cannot survive either");
    CHECK(bitrate_clamp(0xffffffffu) == BITRATE_MAX_MBPS, "an absurd value clamps, it does not wrap");
    CHECK(bitrate_clamp(0) == 0, "Auto survives the clamp: it is a legitimate choice");
    CHECK(bitrate_clamp(1) == 5, "below the lowest rung we snap up, we do not return 0/Auto");
    CHECK(bitrate_clamp(75) == 80, "the old per-link 75 lands on its nearest rung");

    /* Every rung is a fixed point: clamping what the ladder offers must never
     * move it. Without this, a screen could rewrite the user's choice just by
     * being opened. */
    for (int i = 0; i < BITRATE_LADDER_N; i++) {
        CHECK(bitrate_clamp(BITRATE_LADDER[i]) == BITRATE_LADDER[i],
              "each rung is a fixed point of the clamp");
        CHECK(BITRATE_LADDER[bitrate_index(BITRATE_LADDER[i])] == BITRATE_LADDER[i],
              "each rung is found at its own index");
    }

    /* The ladder is strictly increasing: `bitrate_index` picks the nearest by
     * scanning, and two equal or unordered rungs would make its answer depend
     * on the scan order rather than on the value. */
    for (int i = 1; i < BITRATE_LADDER_N - 1; i++)
        CHECK(BITRATE_LADDER[i] < BITRATE_LADDER[i + 1], "the ladder is strictly increasing");
    CHECK(BITRATE_LADDER[BITRATE_LADDER_N - 1] == BITRATE_MAX_MBPS,
          "the ceiling IS the top rung - two numbers would be two ceilings again");

    /* Stepping stays on the ladder whatever the starting point, including the
     * values the three old tables could leave behind. */
    {
        static const uint32_t stale[] = { 0, 1, 7, 25, 37, 75, 120, 150, 500, 0xffffffffu };
        for (size_t i = 0; i < sizeof stale / sizeof stale[0]; i++) {
            const uint32_t up = bitrate_step(stale[i], +1);
            const uint32_t dn = bitrate_step(stale[i], -1);
            CHECK(up == bitrate_clamp(up), "stepping up lands on the ladder");
            CHECK(dn == bitrate_clamp(dn), "stepping down lands on the ladder");
        }
    }

    /* --- Which value is in force -------------------------------------------
     * The per-link setting silently overrode the global one at connect, and the
     * pause menu then silently overrode the per-link one mid-session. Three
     * places computed it; now there is one. */
    CHECK(bitrate_effective(0, BITRATE_LINK_WIFI, 30, 15, 40) == 30,
          "per-link off: the global value wins, whatever the link");
    CHECK(bitrate_effective(1, BITRATE_LINK_WIFI, 30, 15, 40) == 15, "per-link on, Wi-Fi");
    CHECK(bitrate_effective(1, BITRATE_LINK_ETHERNET, 30, 15, 40) == 40, "per-link on, Ethernet");

    /* COUNTER-CASE - THE SERVICE THAT DID NOT ANSWER.
     * `Unknown` is not "probably Wi-Fi". Reading it as such would throttle a
     * wired link on the strength of a missing answer - the exact mistake
     * device_mode.hpp warns about for every caller. */
    CHECK(bitrate_effective(1, BITRATE_LINK_UNKNOWN, 30, 15, 40) == 30,
          "COUNTER-CASE: an unknown link falls back on the global value, never on Wi-Fi");

    /* Auto propagates through the rule rather than being resolved twice. */
    CHECK(bitrate_effective(0, BITRATE_LINK_WIFI, 0, 15, 40) == 0, "Auto survives as Auto");
    CHECK(bitrate_wire_mbps(bitrate_effective(0, BITRATE_LINK_WIFI, 0, 15, 40))
              == BITRATE_CLIENT_DEFAULT_MBPS,
          "and is resolved once, at the wire");

    /* And the rule snaps too: a per-link value inherited from the old 1..150
     * clamp must not escape the ladder just because it took another path. */
    CHECK(bitrate_effective(1, BITRATE_LINK_WIFI, 30, 150, 40) == BITRATE_MAX_MBPS,
          "a stale per-link value is snapped like any other");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
