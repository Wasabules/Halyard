/* probe - headless questions about the account, answered without a window.
 *
 * === PROBE1 2026-10-03 — WHY A HEADLESS MODE IN A GUI CLIENT ==============
 *
 * Two questions the protocol work keeps running into have the same shape:
 * the answer is in a body the client already receives, and getting at it
 * costs a sign-in, a few HTTP calls and nothing else. Starting a VM is NOT
 * part of it, which matters - a VM start consumes the account's hours, and
 * none of this needs a machine that is awake.
 *
 * `halyard-cli` would be the usual home for a headless mode, but it is not
 * built on Windows (`SHADOW_BUILD_TEST_CLI` is forced OFF there), so putting
 * the probe in it would leave the one platform asking the question unable to
 * run the answer. The Qt client builds on all three desktops. It runs before
 * any widget exists and exits with a status, so it is usable from a script.
 *
 * What it is NOT: a bench. It measures nothing and starts nothing. The
 * numbers in `halyard-cli` stay there.
 *
 * === WHAT IT PRINTS, AND WHY THAT NEEDS SAYING ============================
 *
 * Machine metadata, including HTTP bodies verbatim. No access token, no
 * refresh token, no streaming credential and no session key is printed - the
 * bearer is used and not shown. A body can still carry a machine identifier,
 * so the output goes to a path the caller names rather than into the shared
 * log, and the caller is told as much.
 */
#pragma once

#include <QString>

namespace halyard::probe {

/* Runs the probe named by `what` and writes a JSON report to `outPath`
 * ("-" or empty = stdout). Returns the process exit status: 0 on success,
 * non-zero when the sign-in or the request failed.
 *
 * Known names:
 *   "caps"  - the account's first machine (or `vmId`), its /capabilities
 *             body whole, and the four values we parse out of it. The open
 *             question it exists for: the server enforces a per-client
 *             maximum number of displays from the same limit block as the
 *             resolution and frame-rate ceilings, and those two come from
 *             this endpoint (DISP1).
 *   "vms"   - the machine list, raw.
 */
int run(const QString &what, const QString &vmId, const QString &outPath);

}  // namespace halyard::probe
