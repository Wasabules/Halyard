/* envfile.hpp - reading and rewriting `env.txt` from the development machine.
 *
 * WHY. `CLAUDE.md` names the ~120 `SHADOW_*` toggles as THE experiment
 * mechanism of this repo: "every fix ships with a revert toggle so a live A/B
 * needs no rebuild". On the console that promise was only half kept - changing
 * one meant dropping a file over FTP, then relaunching by hand. Since RELOAD-1 a
 * relaunch costs 18 s and no gesture, so the missing half is this: setting the
 * toggle from here. An A/B on console then costs one command and one relaunch.
 *
 * WHY A FILE AND NOT `setenv`. Nearly every toggle is read ONCE into a function
 * `static` - `static int g_x = -1; if (g_x < 0) { ... getenv ... }` - so setting
 * the variable in a running process changes nothing at all, except in whichever
 * module has not read it yet. That would be worse than doing nothing: an
 * experiment that half applies, differently on each run. The file is therefore
 * written for the NEXT launch, which is the only moment the whole application
 * agrees on a value.
 *
 * The format is the one `shadow_load_env_file` reads (main.cpp): `KEY=VALUE` per
 * line, `#` comments, and only `SHADOW_*` keys. `devcmd_env_key_ok` enforces the
 * prefix on the way in, so this file never writes a line the reader would drop
 * in silence.
 *
 * Created 2026-09-12 (DEVL-6).
 */
#ifndef DEVLINK_ENVFILE_HPP
#define DEVLINK_ENVFILE_HPP

#include <string>
#include <vector>

namespace devlink {

/* The toggles currently in the file, in file order, as "KEY=VALUE".
 *
 * An absent file is not an error: it is an empty list, which is also what the
 * application understands as "no toggle". */
std::vector<std::string> envList();

/* Writes `key=value`, or REMOVES the key when `value` is empty.
 *
 * Rewrites the whole file: the toggles are a handful of lines, and rewriting is
 * what keeps one key on one line - an append-only scheme would leave two
 * assignments of the same toggle, and the reader takes the last, so the file
 * would stop saying what the session will do.
 *
 * Comments and unknown lines of the existing file are PRESERVED in place: the
 * dev machine is not the only writer, and silently dropping a hand-written note
 * would be a poor way to repay whoever left it.
 *
 * Returns false and fills `why` on failure ("cle", "ecriture"). The write goes
 * through `shadow/atomic_file.h` (AF4): a cut mid-write must not leave a
 * truncated env.txt, which would load as "no toggle" without a word. */
bool envSet(const std::string &key, const std::string &value, std::string &why);

}  // namespace devlink

#endif /* DEVLINK_ENVFILE_HPP */
