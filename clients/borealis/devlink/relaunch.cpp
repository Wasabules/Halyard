/* relaunch.cpp - see relaunch.hpp. */

#include "relaunch.hpp"
#include "devcmd.h"

#include <cstdio>
#include <cstring>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "../../../core/services/journal.h"
#define rllog(...) JOURNAL_INFO_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)

namespace devlink {
namespace {

/* MODULE state, not a function `static`, and it is not SESSION state: the path
 * of the running NRO is fixed for the whole life of the process. The repo names
 * that distinction first in CLAUDE.md; this is the side of it that is legitimate. */
std::string g_self;

}  // namespace

void rememberSelf(int argc, char *argv[])
{
    g_self.clear();
    if (argc < 1 || !argv || !argv[0]) return;

    const char *a = argv[0];

    /* === THE DEVICE PREFIX, measured on console 2026-09-12 ===================
     *
     * hbloader hands us the path in libnx form, with the storage device in
     * front: "sdmc:/switch/halyard.nro". That does not start with
     * "/switch/", so the check below refused it and `version` answered
     * `nro=(inconnu)` on a console that knew perfectly well what it was running.
     *
     * We keep the STRIPPED form, not the original: it is the one the network
     * command accepts, and the one already PROVEN to work - the three handovers
     * of this same day passed "/switch/halyard.b.nro" to
     * `envSetNextLoad` and hbloader launched it. One single form everywhere
     * beats two that have to agree. */
    const char *slash = std::strchr(a, '/');
    const char *colon = std::strchr(a, ':');
    if (colon && (!slash || colon < slash)) a = colon + 1;

    const size_t n = std::strlen(a);

    /* We accept it only if it is a path this channel would accept as an
     * argument. A bare `relaunch` must not be able to load something the
     * explicit form would refuse - otherwise the shortest command would be the
     * least checked, which is the wrong way round. Off console `argv[0]` is
     * "./halyard", which fails here and leaves `g_self` empty: exactly
     * right, there is nothing to hand over to. */
    if (devcmd_path_ok(a, n)) { g_self.assign(a, n); return; }

    /* Say WHAT was refused. Without this the only symptom is "(inconnu)", and
     * the next person has to guess at the shape of a string they cannot see -
     * which is the guess that cost this very cycle.
     *
     * === SRV-DOC 2026-10-02 - ONLY WHERE A RELAUNCH EXISTS ==================
     * This line used to be emitted on every platform, so a Windows session
     * opened with
     *     [relaunch] argv[0] unusable as a relaunch path: 'C:\...\halyard.exe'
     * on a perfectly valid path. Nothing is wrong there: handing the machine
     * over to another binary is an hbloader mechanism, there is no hbloader off
     * console, and the comment above already calls the empty `g_self` "exactly
     * right". A diagnostic that fires once per launch on a condition that
     * cannot be fixed is noise, and noise is what makes a real line invisible -
     * which is the failure mode this repo keeps paying for.
     * So: a refusal is reported where it means something, and stays quiet where
     * it does not. */
#ifdef __SWITCH__
    rllog("[relaunch] argv[0] unusable as a relaunch path: '%s'", argv[0]);
#endif
}

const std::string &selfPath() { return g_self; }

bool relaunchInto(const std::string &path, std::string &why)
{
    const std::string &cible = path.empty() ? g_self : path;

    if (cible.empty()) {
        /* Either we are not on console, or `argv[0]` was not a usable path.
         * Distinguishing the two costs nothing and saves a wrong diagnosis. */
#ifdef __SWITCH__
        why = "inconnu";        /* on console, but argv[0] told us nothing */
#else
        why = "hors-console";
#endif
        return false;
    }
    if (!devcmd_path_ok(cible.c_str(), cible.size())) { why = "chemin"; return false; }

#ifdef __SWITCH__
    /* hbloader is what reads the next-load slot. Launched any other way - a
     * title takeover, for instance - the slot is not read and setting it would
     * silently do nothing: better to refuse and say which of the two it is. */
    if (!envHasNextLoad()) { why = "no-hbloader"; return false; }

    /* argv[0] by convention is the program's own path. hbloader passes the
     * string on as is, and the NRO we are launching runs `rememberSelf` on it -
     * which is what lets the NEXT iteration hand over in turn. */
    const Result rc = envSetNextLoad(cible.c_str(), cible.c_str());
    if (R_FAILED(rc)) {
        rllog("[relaunch] envSetNextLoad refused (rc=0x%x)", (unsigned)rc);
        why = "libnx";
        return false;
    }
    rllog("[relaunch] handing over to %s on exit", cible.c_str());
    return true;
#else
    why = "hors-console";
    return false;
#endif
}

}  // namespace devlink
