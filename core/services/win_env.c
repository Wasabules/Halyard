/* win_env.c - `setenv` / `unsetenv`, which MinGW does not provide.
 *
 * Compiled everywhere, empty everywhere but Windows. It used to share a file
 * with the SCTP stubs of the abandoned WebRTC path (`legacy_dc_stubs.c`, and
 * before that `win_stubs.c`); those went with that path on 2026-09-26, and the
 * one thing left was never related to them - a Windows gap, not a legacy one.
 */
#ifdef _WIN32
#include <errno.h>
#include <stdlib.h>   /* getenv, _putenv_s */
#include <string.h>   /* strchr */
#endif

#ifdef _WIN32

/* === WIN1 2026-09-10 - POSIX ENVIRONMENT FUNCTIONS ===
 *
 * See `win_compat.h` for why these are missing and what broke without them. */

int setenv(const char *name, const char *value, int overwrite)
{
    /* POSIX requires EINVAL for an empty name or one containing '='. Rejecting
     * it matters: `_putenv_s` would otherwise happily create a variable no
     * `getenv` can ever name back. */
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(name)) return 0;
    /* `_putenv_s` COPIES both strings. `putenv` would keep a pointer into the
     * caller's buffer, and `main.cpp` parses `env.txt` into a STACK buffer -
     * with `putenv` the environment would point at a dead frame as soon as the
     * loop moved on. */
    return _putenv_s(name, value ? value : "") == 0 ? 0 : -1;
}

int unsetenv(const char *name)
{
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    /* Assigning an EMPTY value is how the Windows CRT deletes an entry.
     * Measured on this UCRT before relying on it: after `_putenv_s(k, "")`,
     * `getenv(k)` returns NULL, not "". Had it returned "", every "automatic"
     * setting would have read back as a deliberate empty value. */
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

#endif /* _WIN32 */
