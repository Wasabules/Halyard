/* launch_uri - hand a URI to whatever application the system registered for it.
 *
 * === FT4 2026-10-02 — WHY THIS IS NOT "RUN WINSCP" =========================
 *
 * The one caller today wants WinSCP to open an `sftp://` session. It would be
 * shorter to look for `winscp.exe` and run it — and wrong in three ways:
 *
 *   - the user may have FileZilla, or Explorer's own SFTP add-in, or nothing,
 *     and the application that should answer is THEIR choice, not ours;
 *   - the path to `winscp.exe` is not knowable (Program Files, a portable copy,
 *     a package manager's shim), so finding it means a search that fails on the
 *     one machine that matters;
 *   - a scheme handler is the mechanism the operating system already has for
 *     exactly this question.
 *
 * So we hand over the URI and let the system decide. `ShellExecute` on Windows,
 * `xdg-open` on Linux, nothing at all on console — where there is no second
 * application to hand anything to.
 *
 * THE URI CARRIES A PASSWORD. Two consequences that callers must know:
 *   - it is never logged here, not even truncated. The caller logs that it
 *     launched something, never what;
 *   - on Linux it is passed as an ARGV entry, not through a shell, so it never
 *     reaches a command line a shell would expand or a history file would keep.
 *     It is still visible in `/proc` to the same user for the lifetime of the
 *     `xdg-open` process; that is inherent to handing a URI to another program
 *     and is stated rather than hidden.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when this platform can hand a URI to another application at all. */
bool launch_uri_available(void);

/* Opens `uri` with whatever the system registered for its scheme.
 *
 * Returns true when the request was handed over — NOT that the application
 * opened, which happens asynchronously and which we cannot observe. A false
 * means the handover itself failed: no handler registered, or the platform has
 * no mechanism.
 *
 * Does not block: it must be callable from a UI callback. */
bool launch_uri(const char *uri);

#ifdef __cplusplus
}
#endif
