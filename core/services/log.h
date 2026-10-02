/* log.h - COMPATIBILITY FACADE over `core/services/journal.h`.
 *
 * === LIB2 2026-10-02 - MOVED OUT OF `core/common/`, WHICH IS GONE ==========
 *
 * `core/common/` held this file and `stats`, and this one reached up into
 * `core/services/` for `journal.h` - the `common <-> services` cycle inside
 * core/. The cheap fix looked like moving the journal DOWN into common;
 * measured, `journal.c` depends on four other services files (atomic_file,
 * config, sockets_compat, log_redact), so that would have created four back
 * edges to remove one.
 *
 * The real finding is that `core/common` was not a layer. Two files, one of
 * them a macro facade over a service, is a leftover - so it was dissolved
 * instead of defended. `log` and `stats` are services, like the journal this
 * wraps, which is now a sibling.
 *
 * === WHY THIS FILE STILL EXISTS (S81, 2026-08-29) ===
 *
 * The journal now has a severity, a category and a filter: see
 * `core/services/journal.h`, which explains the three axes and why they were missing.
 *
 * This file stays, with its surface unchanged, for a mechanical reason: it is
 * included by some forty modules, and twenty-three of them define their own
 * alias (`vlog`, `alog`, `clog`, `mlog`...) - more than five hundred call sites.
 * Rewriting them in one go is five hundred chances to get it wrong, for a change
 * that adds nothing for the user.
 *
 * So the migration happens PER MODULE, by redirecting the alias at the top of
 * the file to the category that already fits it:
 *
 *     #include "journal.h"
 *     #define vlog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO,  __VA_ARGS__)
 *     #define vdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
 *
 * One line per file, no call site touched, and every line of the module
 * carries its category at once.
 *
 * What still goes through `journal_uncategorised()` is written at `INFO` / `LEGACY`:
 * VISIBLE in the journal, hence measurable, and never dropped in silence.
 * Assuming an unclassified line is noise would amount to deleting by default the
 * traces this repo lives on, on the strength of a heuristic.
 */

#pragma once

#include "journal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A line with no category, at INFO. The only forwarder left: a `static inline`
 * cannot pass a `...` on, so it lives in log.c. (It was `webrtc_log`, a name
 * from the module this journal grew out of; the four lifecycle aliases that
 * went with it - `webrtc_log_flush`, `_shutdown`, `_set_command_handler`,
 * `_reconnect_sink` - are gone, their callers use the `journal_*` functions
 * they forwarded to.) */
void journal_uncategorised(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
