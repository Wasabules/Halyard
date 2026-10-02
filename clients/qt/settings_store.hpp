/* settings_store - where the settings window's choices survive a restart.
 *
 * === QT5 2026-10-03 — THE WINDOW FORGOT EVERYTHING =========================
 *
 * Until now the settings window called `setenv` and nothing else. That is
 * correct for the running process - core reads the environment - and it means
 * every choice was lost on exit. Nobody had noticed because nobody had yet
 * closed the client after changing something.
 *
 * Persisted with QSettings, in INI format (set in main.cpp), under `[env]`,
 * one key per variable, the exact string core will read. An INI and not the
 * Windows registry: it can be read, diffed, deleted and attached to a report,
 * which is what the Borealis client's `settings.txt` already taught.
 *
 * === THE ORDER, AND WHY IT IS CONTRACT 3 AGAIN =============================
 *
 * `loadIntoEnvironment()` runs AFTER `env_override_snapshot()` and BEFORE any
 * session. After, so the snapshot records only what came from outside - a
 * stored value must not be mistaken for the user's environment, or the window
 * would lock its own rows. Before, because core caches what it reads at first
 * use.
 *
 * And it never overwrites a variable set from outside: `env.txt` / the shell
 * wins over this file, exactly as it wins over the window.
 *
 * Only variables that are rows of the settings table are loaded. A stale file
 * from another build must not be able to switch on a campaign instrument the
 * window does not even show.
 */
#pragma once

#include <QString>

namespace halyard::store {

/* Apply every stored value to the environment, except where the variable came
 * from outside the application. Returns how many were applied. */
int loadIntoEnvironment();

/* Remember `value` for `env`; an empty value forgets it (the variable is unset,
 * so there is nothing to restore). */
void saveVariable(const QString &env, const QString &value);

/* Forget every stored variable. Used by "Restore defaults". */
void forgetAll();

}  // namespace halyard::store
