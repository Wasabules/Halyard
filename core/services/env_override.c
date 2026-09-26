/* See env_override.h for the why. */
#include "env_override.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The keys a SETTING claims to control. Adding a setting that pushes a variable
 * means adding it here, otherwise the screen goes back to lying about it.
 *
 * Order is the display order of the summary line, so it is grouped by screen
 * rather than alphabetical. */
static const char *const KNOWN[] = {
    /* Image / quality */
    "SHADOW_BITRATE_MBPS", "SHADOW_FPS", "SHADOW_CODEC", "SHADOW_PROFILE_ID",
    "SHADOW_REG_F5", "SHADOW_HWACCEL", "SHADOW_VIDEO_NET_TCP", "SHADOW_VSYNC",
    "SHADOW_STRETCH", "SHADOW_DISPLAY_HEIGHT",
    /* Audio */
    "SHADOW_AUDIO_CODEC", "SHADOW_HWOPUS", "SHADOW_VOLUME",
    /* Controls */
    "SHADOW_GAMEPAD_PLUG", "SHADOW_GAMEPAD_TYPE", "SHADOW_RUMBLE_PCT",
    "SHADOW_INPUT_ABS",
    /* Advanced */
    "SHADOW_JOURNAL_NIVEAU",
};
#define KNOWN_N ((int)(sizeof KNOWN / sizeof KNOWN[0]))

/* One flag per known key. Module state, not function state: the repo's most
 * expensive defect family is session state hidden in a function `static`, and
 * this one is read from three translation units. */
static char g_forced[KNOWN_N];
static int  g_taken = 0;
static int  g_count = 0;
static char g_summary[512];

static int index_of(const char *key)
{
    int i;
    if (!key) return -1;
    for (i = 0; i < KNOWN_N; i++)
        if (strcmp(KNOWN[i], key) == 0) return i;
    return -1;
}

void env_override_snapshot(void)
{
    size_t off = 0;
    int i;

    /* Idempotent on purpose. A second call would run AFTER `applyToggles` has
     * written these same variables, and would report every setting as forced —
     * the exact false alarm this module exists to prevent. */
    if (g_taken) return;
    g_taken = 1;

    g_count = 0;
    g_summary[0] = 0;
    for (i = 0; i < KNOWN_N; i++) {
        const char *v = getenv(KNOWN[i]);
        /* An EMPTY value counts as set: `SHADOW_FPS=` in `env.txt` produces a
         * key whose `atoi` is 0, which several readers treat as "automatic".
         * That is still an outside decision, so it is still an override. */
        g_forced[i] = (v != NULL);
        if (!g_forced[i]) continue;
        g_count++;
        {
            int n = snprintf(g_summary + off, sizeof g_summary - off,
                             "%s%s", off ? " " : "", KNOWN[i]);
            if (n > 0 && (size_t)n < sizeof g_summary - off) off += (size_t)n;
        }
    }
}

int env_override_active(const char *key)
{
    int i;
    if (!g_taken) return 0;
    i = index_of(key);
    return i >= 0 ? g_forced[i] : 0;
}

int env_override_count(void) { return g_count; }

const char *env_override_summary(void) { return g_summary; }

int env_override_known_count(void) { return KNOWN_N; }

const char *env_override_key(int i)
{
    return (i >= 0 && i < KNOWN_N) ? KNOWN[i] : NULL;
}
