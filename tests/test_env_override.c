/* test_env_override.c - which settings are overridden from outside
 * (core/services/env_override.c).
 *
 * This module answers ONE question — "is the value on screen the one the
 * machine obeys?" — and it answers it from a snapshot taken at a single precise
 * instant. Both halves of that sentence are what this suite pins:
 *
 *   - the snapshot must be taken BEFORE the application sets these same
 *     variables itself. `applyToggles` writes `SHADOW_HWACCEL`,
 *     `SHADOW_CODEC`, `SHADOW_RUMBLE_PCT`... every time a setting changes. A
 *     reading taken after that finds them all and reports EVERY setting as
 *     forced by `env.txt` — a warning on every row, which is the same lie as no
 *     warning at all, only louder.
 *
 *   - before the snapshot the answer is NO, never "maybe". A caller that runs
 *     early must not paint a warning it cannot yet justify.
 *
 * The counter-case is concrete and was paid for: `SHADOW_VIDEO_NET_TCP=1` sat
 * in a live `env.txt` while the settings screen showed TCP off. The check named
 * `[VIDEO_NET_TCP]` is that session, replayed.
 *
 * Order matters here — the module is deliberately single-shot — so the cases
 * run in one process, from "before" to "after", and the file says so at each
 * step rather than pretending the checks are independent.
 */
#include "../core/services/env_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                   \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* ── Avant l'instantane ───────────────────────────────────────────────────── */

static void before_the_snapshot(void)
{
    /* Counter-case: answering "forced" here would put a warning on rows whose
     * state is not yet known. Silence is the only honest answer. */
    CHECK(env_override_active("SHADOW_FPS") == 0,
          "[avant] no key is forced before the snapshot");
    CHECK(env_override_count() == 0, "[avant] the count is zero before the snapshot");
    CHECK(env_override_summary() != NULL, "[avant] the summary is never NULL");
    CHECK(env_override_summary()[0] == 0, "[avant] the summary is empty before the snapshot");
}

/* ── The table of known keys ──────────────────────────────────────────────── */

static void the_known_keys(void)
{
    const int n = env_override_known_count();
    int i;
    CHECK(n > 0, "[table] the key table is not empty");
    CHECK(env_override_key(-1) == NULL, "[table] index -1 gives nothing");
    CHECK(env_override_key(n) == NULL, "[table] one past the end gives nothing");
    for (i = 0; i < n; i++) {
        const char *k = env_override_key(i);
        CHECK(k != NULL, "[table] every index within bounds names a key");
        /* Only `SHADOW_` keys: `env.txt` refuses the rest at load time, so a
         * key here that could never come from that file would be dead weight
         * the reader would have to prove harmless. */
        CHECK(k && strncmp(k, "SHADOW_", 7) == 0, "[table] every key is prefixed SHADOW_");
    }
    /* Duplicates: harmless for `active()`, but they would count twice and put
     * the same name twice in the summary line. */
    for (i = 0; i < n; i++) {
        int j;
        for (j = i + 1; j < n; j++)
            CHECK(strcmp(env_override_key(i), env_override_key(j)) != 0,
                  "[table] no key appears twice");
    }
}

/* ── L'instantane ─────────────────────────────────────────────────────────── */

static void the_snapshot(void)
{
    /* What `env.txt` would have deposited. Set BEFORE the snapshot, which is
     * exactly the ordering `main.cpp` guarantees. */
    setenv("SHADOW_VIDEO_NET_TCP", "1", 1);
    setenv("SHADOW_FPS", "60", 1);
    setenv("SHADOW_VSYNC", "", 1);          /* empty on purpose, see below */
    unsetenv("SHADOW_HWACCEL");
    unsetenv("SHADOW_CODEC");
    unsetenv("SHADOW_RUMBLE_PCT");

    env_override_snapshot();

    /* The session that cost us the detour. */
    CHECK(env_override_active("SHADOW_VIDEO_NET_TCP") == 1,
          "[VIDEO_NET_TCP] a key present in env.txt is reported as forced");
    CHECK(env_override_active("SHADOW_FPS") == 1,
          "[instantane] a second key is reported too");

    /* An EMPTY value is still an outside decision. `SHADOW_VSYNC=` gives an
     * `atoi` of 0, which readers take for "off" — a choice the user did not make
     * on screen, so the row must still be marked. Treating empty as absent
     * would leave exactly that case unmarked. */
    CHECK(env_override_active("SHADOW_VSYNC") == 1,
          "[vide] a key set to an empty value is still forced");

    CHECK(env_override_active("SHADOW_HWACCEL") == 0,
          "[instantane] an absent key is not forced");
    CHECK(env_override_active("SHADOW_CODEC") == 0,
          "[instantane] a second absent key is not forced either");

    CHECK(env_override_count() == 3, "[compte] exactly the three keys that were set");

    /* The summary line feeds a diagnostic row: it must name them, and name only
     * them. */
    CHECK(strstr(env_override_summary(), "SHADOW_VIDEO_NET_TCP") != NULL,
          "[resume] the summary names the forced key");
    CHECK(strstr(env_override_summary(), "SHADOW_FPS") != NULL,
          "[resume] the summary names the second one");
    CHECK(strstr(env_override_summary(), "SHADOW_HWACCEL") == NULL,
          "[resume] the summary does not name a key that is not forced");
}

/* ── Les bornes ───────────────────────────────────────────────────────────── */

static void the_bounds(void)
{
    CHECK(env_override_active(NULL) == 0, "[bornes] NULL is not a forced key");
    CHECK(env_override_active("") == 0, "[bornes] the empty string is not a forced key");
    /* A key that exists in the environment but that NO setting claims to
     * control. Answering yes would let a caller paint a warning on a row whose
     * value nothing overrides. */
    setenv("SHADOW_PARITY_TRIM", "28", 1);
    CHECK(env_override_active("SHADOW_PARITY_TRIM") == 0,
          "[bornes] a toggle with no setting is not reported");
    CHECK(env_override_active("SHADOW_FP") == 0, "[bornes] a truncated key does not match");
    CHECK(env_override_active("SHADOW_FPSX") == 0, "[bornes] a lengthened key does not match");
}

/* ── The trap: a second snapshot ──────────────────────────────────────────── */

static void a_second_snapshot(void)
{
    /* THE counter-case of this file. What follows is what `applyToggles` does,
     * a few milliseconds after startup and then on every settings change: it
     * writes the very keys this module watches. */
    setenv("SHADOW_HWACCEL", "1", 1);
    setenv("SHADOW_CODEC", "1", 1);
    setenv("SHADOW_RUMBLE_PCT", "80", 1);

    env_override_snapshot();   /* must change NOTHING */

    CHECK(env_override_active("SHADOW_HWACCEL") == 0,
          "[2e instantane] a variable WE set is not reported as forced");
    CHECK(env_override_active("SHADOW_CODEC") == 0,
          "[2e instantane] nor the second");
    CHECK(env_override_active("SHADOW_RUMBLE_PCT") == 0,
          "[2e instantane] nor the third");
    CHECK(env_override_count() == 3,
          "[2e instantane] the count does not grow with our own writes");
    CHECK(strstr(env_override_summary(), "SHADOW_HWACCEL") == NULL,
          "[2e instantane] the summary does not grow either");

    /* And the real answers still hold: idempotence must not erase the snapshot
     * either. */
    CHECK(env_override_active("SHADOW_VIDEO_NET_TCP") == 1,
          "[2e instantane] the real override is still reported");
}

int main(void)
{
    printf("test_env_override - settings forced from the outside\n");
    before_the_snapshot();
    the_known_keys();
    the_snapshot();
    the_bounds();
    a_second_snapshot();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
