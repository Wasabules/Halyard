/* relaunch.hpp - hand the console over to another .nro instead of stopping.
 *
 * WHY THIS EXISTS. Every iteration on the console cost one human gesture that
 * nothing could automate: pressing A in hbmenu. Everything around it was already
 * automatic - build, upload over FTP, drive the interface and read the screen
 * through the [devlink] channel - so a session ended with "relaunch it and tell
 * me what you see", which is exactly the shape this repo tries to remove
 * (`memory/feedback_automation_priority.md`).
 *
 * HOW IT IS DONE, and why no new dependency was needed. libnx exposes
 * `envSetNextLoad(path, argv)`: an NRO started by hbloader may name the NRO to
 * load NEXT, and on exit hbloader launches that one rather than returning to
 * hbmenu. It is the very mechanism hbmenu itself uses to start anything. So the
 * application can hand over to a new build of itself, and the loop closes with
 * one launch per SESSION instead of one per iteration.
 *
 * THE TRAP THAT SHAPES THE INTERFACE: a running NRO is LOCKED. The new build
 * therefore cannot land on the path currently executing - the upload fails with
 * FTP 450, which is also why `quit` has existed since S45. Hence the argument:
 * the dev machine pushes under a second name and asks for THAT one, the two
 * names alternating from one iteration to the next. `tools/switch-sync.sh
 * relance` does the alternating.
 *
 * WHAT THIS IS NOT. It is a mechanism for ITERATING, not for RECOVERING. If the
 * application crashes, nothing is left to receive the command and a human must
 * relaunch - a real limit, and the reason a remote-input sysmodule
 * (`sys-botbase`) would complement this rather than be replaced by it.
 *
 * Off console there is no hbloader: `relaunchInto` refuses and says so, rather
 * than pretending. Created 2026-09-12 (RELOAD-1).
 */
#ifndef DEVLINK_RELAUNCH_HPP
#define DEVLINK_RELAUNCH_HPP

#include <string>

namespace devlink {

/* Records the path of the NRO currently running, taken from `argv[0]`. Called
 * once from `main`, before anything else can ask for a relaunch.
 *
 * `argv[0]` is what hbloader passed us, so it is the path that is LOCKED - the
 * one a bare `relaunch` reloads, and the one the caller must NOT push onto. */
void rememberSelf(int argc, char *argv[]);

/* The path recorded above, or "" when there is none (desktop, or an argv[0]
 * that is not a plausible NRO path). */
const std::string &selfPath();

/* Asks for `path` (or, empty, the NRO currently running) to be loaded on exit.
 *
 * Returns true when the handover is ARMED - not when it has happened: the
 * application still has to exit, which the caller does right after. On false,
 * `why` receives a short reason fit to be sent straight back to the dev machine
 * ("hors-console", "no-hbloader", "chemin", "inconnu").
 *
 * The path is expected to have been through `devcmd_path_ok` already; this
 * function re-checks it anyway, because it is the last place before libnx and
 * the cost is a few dozen comparisons. */
bool relaunchInto(const std::string &path, std::string &why);

}  // namespace devlink

#endif /* DEVLINK_RELAUNCH_HPP */
