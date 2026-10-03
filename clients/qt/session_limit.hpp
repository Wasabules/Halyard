/* When to warn that a session is running out, and how loudly.
 *
 * === LIM1 2026-10-03 — TWO DIFFERENT LIMITS, AND THEY MEAN DIFFERENT THINGS
 *
 * `/vms/{id}/capabilities` carries two, and confusing them would make the
 * warning say the wrong thing at the worst moment:
 *
 *  - `usage.max_session_length` is ONE SESSION's ceiling (21600 s on the
 *    account measured). When it runs out Shadow ends the session - almost
 *    certainly the `get-out` event in the SSE taxonomy. You reconnect and
 *    you get another full one.
 *  - `usage.max_duration` is the PERIOD's allowance (fair use), against
 *    `usage.fair_use_usage`, renewing at `fair_use_renew_date`. Running out
 *    of that is not about this session at all.
 *
 * So the session warning says "this session ends in N minutes, you can
 * reconnect", and the fair-use warning is a different sentence said once.
 *
 * === WHY THE THRESHOLDS ARE WHAT THEY ARE ================================
 *
 * 30, 15, 5 and 1 minutes. Thirty is enough to finish something; fifteen is
 * enough to save and quit; five is "stop what you are doing"; one is "it is
 * about to go". Each fires ONCE - a warning repeated every tick is a warning
 * people learn to dismiss without reading, which is worse than none at the
 * moment it finally matters.
 *
 * PURE so `tests/test_qt_session_limit.cpp` can walk a whole six hours in a
 * loop: the edges here are all "did this fire exactly once", and that is not
 * something to verify by sitting in front of a stream for 5 h 45.
 */
#pragma once

#include <QString>

namespace halyard {

/* How hard the UI should push. The caller maps these to a toast, a banner and
 * a colour; this header does not know what either is. */
enum class LimitLevel {
    None = 0,   /* nothing to say */
    Notice,     /* 30 min - a toast, no banner */
    Warn,       /* 15 and 5 min - a toast and a banner */
    Urgent,     /* 1 min and past the end - a banner that stays */
};

struct LimitWarning {
    LimitLevel level = LimitLevel::None;
    int        minutes = 0;    /* the threshold that fired, 0 = time is up */
    bool       fired = false;  /* a NEW threshold was crossed on this call */
};

/* The thresholds, in seconds, largest first. Exposed so the test can walk
 * them rather than hardcode a copy that could drift. */
inline const int *limitThresholds(int *count)
{
    static const int kT[] = { 30 * 60, 15 * 60, 5 * 60, 60 };
    if (count) *count = 4;
    return kT;
}

/* Decides what to show, and remembers how far it has got in `state`.
 *
 * `state` is an opaque counter the caller owns and initialises to 0: it is
 * the number of thresholds already announced. Keeping it OUTSIDE this
 * function is what makes the function pure and the whole thing testable; it
 * also means a reconnection resets the warnings simply by zeroing it, which
 * is correct - a new session has a new ceiling.
 *
 * `ceilingSec` <= 0 means the server gave no ceiling: returns None and fires
 * nothing, ever. A countdown invented from a default would be the exact
 * mistake the console client refused to make.
 */
inline LimitWarning sessionLimitCheck(int elapsedSec, int ceilingSec, int *state)
{
    LimitWarning w;
    if (ceilingSec <= 0 || !state) return w;

    const int left = ceilingSec - elapsedSec;

    int n = 0;
    const int *t = limitThresholds(&n);

    /* How many thresholds SHOULD have been announced by now. Computed from
     * the remaining time rather than by stepping one at a time, so a client
     * that was asleep - a laptop lid, a suspended VM - catches up to the
     * right place instead of replaying every warning it missed. */
    int due = 0;
    for (int i = 0; i < n; i++)
        if (left <= t[i]) due = i + 1;
    if (left <= 0) due = n + 1;   /* past the end */

    if (due <= *state) {
        /* Nothing new. Still report the CURRENT level, because a banner has
         * to stay up between ticks; only `fired` distinguishes the two. */
        if (*state > n)      { w.level = LimitLevel::Urgent; w.minutes = 0; }
        else if (*state > 0) {
            w.minutes = t[*state - 1] / 60;
            w.level = w.minutes <= 1 ? LimitLevel::Urgent
                    : w.minutes >= 30 ? LimitLevel::Notice
                                      : LimitLevel::Warn;
        }
        return w;
    }

    /* Crossed one or more. Announce the MOST URGENT reached, not each in
     * turn: waking up with 4 minutes left should say four, not thirty. */
    *state = due;
    w.fired = true;
    if (due > n) { w.level = LimitLevel::Urgent; w.minutes = 0; return w; }
    w.minutes = t[due - 1] / 60;
    w.level = w.minutes <= 1 ? LimitLevel::Urgent
            : w.minutes >= 30 ? LimitLevel::Notice
                              : LimitLevel::Warn;
    return w;
}

/* Seconds as hours and minutes, for a sentence. Here rather than in each
 * window because three of them now say the same kind of thing and a second
 * copy is how two of them end up disagreeing about the separator. */
inline QString fmtHours(int seconds)
{
    if (seconds <= 0) return QStringLiteral("0");
    const int h = seconds / 3600, m = (seconds % 3600) / 60;
    if (h <= 0) return QStringLiteral("%1 min").arg(m);
    return m > 0 ? QStringLiteral("%1 h %2").arg(h).arg(m, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1 h").arg(h);
}

/* Is the account past the server's own fair-use alert point?
 *
 * The threshold is the SERVER's (`fair_use_alert_threshold`, 0.8 on the
 * account measured) and not one we chose: it is already in the reply, and
 * inventing 80 % would have been indistinguishable in practice and wrong in
 * principle. False when either figure is missing - no allowance, no warning.
 */
inline bool fairUseAlert(int used, int allowance, double threshold)
{
    if (allowance <= 0 || threshold <= 0.0) return false;
    return double(used) / double(allowance) >= threshold;
}

}  // namespace halyard
