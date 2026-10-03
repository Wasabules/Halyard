/* Grading - a reading turned into a colour, and the thresholds it uses.
 *
 * === OV5/HUD1 2026-10-03 — WHY THIS IS ITS OWN HEADER =====================
 *
 * The grade decides the colour of a HUD row, and the whole point of matching
 * Borealis value for value is that the SAME reading carries the SAME colour in
 * both clients - otherwise "it is orange here and green there" arrives as a
 * bug report about the protocol rather than about a threshold.
 *
 * Kept apart from `hud_model.hpp` because that header pulls in core's
 * `stats.h` and `shadow_input.h`, and a snapshot can only be built by calling
 * into a live session. These four functions and the thresholds they are used
 * with are arithmetic on plain numbers, so they belong in the offline suite
 * (`tests/test_qt_hud_grade.cpp`) where a boundary can actually be pinned.
 *
 * The boundary convention is the part worth pinning: both comparisons are
 * INCLUSIVE, so a reading exactly on the "good" threshold is good and one
 * exactly on the "warn" threshold is warn. Borealis does the same, and an
 * off-by-one here is invisible until two clients are put side by side.
 */
#pragma once

namespace halyard {

enum class Grade { Neutral, Good, Warn, Bad };

/* Higher is better: frames per second, bitrate. */
inline Grade gradeHi(double v, double warn, double good)
{
    if (v >= good) return Grade::Good;
    if (v >= warn) return Grade::Warn;
    return Grade::Bad;
}

/* Lower is better: latency, loss, a wait. */
inline Grade gradeLo(double v, double warn, double good)
{
    if (v <= good) return Grade::Good;
    if (v <= warn) return Grade::Warn;
    return Grade::Bad;
}

/* A counter that should stay at zero. Neutral and not Good at zero: a count of
 * zero errors is the normal state, and painting it green would spend the
 * reader's attention on the thing that is fine. */
inline Grade badIfAny(unsigned long long v)
{
    return v ? Grade::Bad : Grade::Neutral;
}

inline Grade warnIfAny(unsigned long long v)
{
    return v ? Grade::Warn : Grade::Neutral;
}

/* The named thresholds, so the two clients cite one source.
 *
 * `kDispWait*` are in milliseconds and come straight from frame pacing: 16 ms
 * is one frame at 60 Hz, 33 ms is two. `kLoss*` are per cent of expected
 * chunks. `kRtt*` are the control channel's round trip in milliseconds. */
namespace grade {
inline constexpr double kDispWaitWarnMs = 33.0, kDispWaitGoodMs = 16.0;
inline constexpr double kLossWarnPct    = 1.0,  kLossGoodPct    = 0.3;
inline constexpr double kRttWarnMs      = 80.0, kRttGoodMs      = 30.0;
}  // namespace grade

}  // namespace halyard
