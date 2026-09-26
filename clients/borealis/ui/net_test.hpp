/* ui::nettest - the link-quality page, SHARED by both its hosts.
 *
 * Drawn in two places, like the gamepad and mouse testers whose pattern it
 * follows: from Settings and from the pause menu. One body of code, two hosts -
 * two copies would end up diverging, which this repo has already paid for.
 *
 * === WHAT IT SHOWS, AND WHY IT EXISTS BESIDE THE METRICS PANEL ===
 *
 * The panel gives the instant: bitrate, loss, round trip, here and now. This
 * page gives something the panel cannot - what the link CARRIES, accumulated
 * over the session.
 *
 * The difference is the one that cost an evening on 2026-09-03: the user had set
 * 100 Mb/s over Ethernet, was receiving 30, and was seeing pieces of the
 * previous picture. No screen said that their path saturated around 17-20 Mb/s.
 * It took quitting, pulling the log, and crossing bitrate against loss by hand.
 *
 * === WHAT IT REFUSES TO SHOW ===
 *
 * A single, crisp number. A link does not have ONE bitrate: it has a ceiling
 * beyond which it loses, and that ceiling moves. So the page shows the highest
 * bitrate seen clean, the lowest seen lossy, and how many samples back them -
 * and when there is not enough to conclude, it SAYS so rather than displaying a
 * number that would look just as sure of itself.
 *
 * It sends nothing either. No test traffic: this repo is byte-exact by
 * discipline, and saying on a channel what the server does not expect can kill
 * it (KB §9, S57). It reads what the session already measures.
 */
#pragma once

#include <nanovg.h>

namespace ui {
namespace nettest {

/* One sample per second of session, taken by the host. Separate from the
 * drawing because the measurement must continue even when the page is not on
 * screen - which is exactly the defect this repo has fixed four times: a counter
 * that only exists while you are looking at it measures nothing. */
void sample(double t);

/* Resets - a settings change invalidates what came before: the earlier samples
 * were taken under a different ceiling. */
void reset();

/* Draws. Returns TRUE when B has been held long enough to leave. */
bool draw(NVGcontext *vg, float x, float y, float w, float h, double t);

}  // namespace nettest
}  // namespace ui
