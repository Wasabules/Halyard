/* The server's word for a machine's state, turned into a pill class.
 *
 * === UI1 2026-10-03 — WHY THE UNKNOWN CASE IS "off" AND NOT "ok" ==========
 *
 * The colour of the pill is a CLAIM about the machine. Green says "this is
 * awake, press Connect"; getting that wrong sends someone connecting to a
 * machine that is not there, and the failure arrives seven steps later as a
 * timeout with no obvious cause.
 *
 * The launcher's vocabulary is not ours and is not frozen - it has shipped
 * `running`, `started`, `ready`, `starting`, `pending`, `stopped` and
 * `shutdown` at various times, and a new one can appear without warning. So
 * matching is on SUBSTRINGS of the lowercased state, and anything unrecognised
 * falls to "off": the conservative claim, the one whose cost is a pointless
 * click rather than a confusing failure.
 *
 * Pure and Qt-Core-only so `tests/test_qt_machine_state.hpp` can hold the
 * vocabulary down - this is a table of strings, which is precisely the kind of
 * thing that rots silently.
 */
#pragma once

#include <QString>

namespace halyard {

/* "ok" awake, "busy" on its way, "off" asleep or unknown. The returned string
 * is the `pill` dynamic property the stylesheet selects on. */
inline const char *pillClassFor(const QString &state)
{
    const QString s = state.toLower();

    /* Transitional first, and matched on "starting" and NOT on "start".
     *
     * The first version tested `contains("start")` here and listed "started"
     * among the awake words further down - where it could never be reached,
     * because "started" contains "start". A running machine was painted amber
     * for ever. Matching the participle is what separates the two, and it is
     * the reason this function is worth a test at all. */
    if (s.contains(QStringLiteral("starting")) ||
        s.contains(QStringLiteral("booting")) ||
        s.contains(QStringLiteral("pending")) ||
        s.contains(QStringLiteral("creating")) ||
        s.contains(QStringLiteral("provision")) ||
        s.contains(QStringLiteral("resuming")) ||
        s.contains(QStringLiteral("waking")))
        return "busy";

    if (s.contains(QStringLiteral("running")) ||
        s.contains(QStringLiteral("started")) ||
        s.contains(QStringLiteral("ready")) ||
        s.contains(QStringLiteral("active")) ||
        s.contains(QStringLiteral("online")))
        return "ok";

    return "off";
}

}  // namespace halyard
