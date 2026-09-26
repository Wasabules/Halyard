/* env_override — which settings are overridden from outside the application.
 *
 * === THE DEFECT THIS CLOSES ===
 *
 * `env.txt` (and, on desktop, the shell environment) has priority over every
 * saved setting: `main.cpp` applies it before anything else, and `applyToggles`
 * deliberately refuses to overwrite a key that came from outside. That priority
 * is RIGHT — a campaign must be able to impose a value without going through the
 * UI — but until now it was SILENT.
 *
 * The screen therefore said "Bitrate 20 Mb/s" while the session ran at 50, and
 * the only way to find out was to read the SD card from a computer. It cost this
 * project a real detour: `SHADOW_VIDEO_NET_TCP=1` sat in a live `env.txt` for an
 * unknown number of sessions, cutting server-side adaptive bitrate, while the
 * setting screen showed the opposite.
 *
 * A setting that lies is worse than no setting at all — the same rule that got
 * "Codec" and "Profile" wired up on 2026-08-27. So: mark them.
 *
 * === WHY A SNAPSHOT AND NOT A `getenv` ===
 *
 * The application sets these very variables itself, from `applyToggles`, every
 * time a setting changes. A `getenv` asked after that always answers yes, and
 * every setting would be reported as forced. Only a reading taken BEFORE our own
 * first `setenv` can tell the two apart — hence `env_override_snapshot()`,
 * called from `main` right after `env.txt` is loaded.
 *
 * That is also why the key list is fixed rather than an enumeration of the
 * environment: the question only has meaning for the keys a SETTING claims to
 * control. The ~200 other `SHADOW_*` toggles are experiment reverts with no UI,
 * and nothing on screen can contradict them.
 */
#ifndef SHADOW_ENV_OVERRIDE_H
#define SHADOW_ENV_OVERRIDE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Records which of the known keys already exist in the environment.
 * Call ONCE, from `main`, after loading `env.txt` and before any `setenv` of
 * ours. Calling it a second time is a no-op: a later call would see our own
 * writes and report every setting as forced. */
void env_override_snapshot(void);

/* 1 if `key` was set from outside, so the matching setting is displayed but not
 * obeyed. Answers 0 for an unknown key and 0 before the snapshot — an
 * un-taken snapshot must never make the UI claim an override. */
int env_override_active(const char *key);

/* Number of keys overridden; 0 = the settings are fully in charge. */
int env_override_count(void);

/* The overridden keys as one line, "SHADOW_FPS SHADOW_VSYNC", for a summary
 * row. Never NULL; empty string when there is none. Points to static storage. */
const char *env_override_summary(void);

/* The known keys, for the test suite and for a diagnostic listing.
 * `env_override_key(i)` is NULL past the end. */
int         env_override_known_count(void);
const char *env_override_key(int i);

#ifdef __cplusplus
}
#endif
#endif
