/* win_compat.h - the POSIX functions the Windows CRT does not provide.
 *
 * === WIN1 2026-09-10 - WHY THIS EXISTS ===
 *
 * The Windows desktop build worked on 2026-05-24 and no longer built on
 * 2026-09-09. The cause is worth naming, because it will recur: `env.txt`
 * (`main.cpp`) and `Settings::applyToggles` were written against POSIX
 * `setenv`/`unsetenv`, which the Windows CRT simply does not have. Nothing
 * warned, because no loop builds Windows - the regression sat there for months
 * and was found only by trying.
 *
 * These two are the only missing pieces that are NOT about sockets;
 * `sockets_compat.h` owns everything else, including `poll` and the socket
 * timeouts.
 *
 * OFF WINDOWS THIS HEADER DECLARES NOTHING. `<stdlib.h>` already provides the
 * real functions there, and re-declaring them would be a portability bug of its
 * own - the exact class of defect this file exists to close.
 */

#pragma once

#if defined(_WIN32)

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX setenv(3). With `overwrite == 0`, an already-defined variable is left
 * untouched. Returns 0, or -1 with `errno` set. */
int setenv(const char *name, const char *value, int overwrite);

/* POSIX unsetenv(3) - it REMOVES the variable, and removal is the whole point:
 * a settings row going back to "automatic" must erase the choice, not leave an
 * empty variable behind that a later reader would take for a value
 * (`settings.cpp` says so itself, next to its `unsetenv` calls). */
int unsetenv(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
