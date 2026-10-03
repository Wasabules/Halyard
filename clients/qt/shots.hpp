/* shots - render the UI to PNGs, with no session and no display.
 *
 * === SHOT1 2026-10-03 — WHY THIS EXISTS =================================
 *
 * A design was proposed as a set of artboards and then "implemented", and
 * what shipped was the old layout in new colours. Nobody caught it until the
 * user looked at the running client, because nothing between the mockup and
 * the screen ever compared the two — the author was reading code and
 * imagining pixels.
 *
 * `halyard-qt --shots <dir>` builds the real widgets, feeds them a synthetic
 * frame and synthetic numbers, and writes one PNG per surface. It needs no
 * VM, no account and no display (`QT_QPA_PLATFORM=offscreen`), so it runs in
 * a loop while a layout is being written, and the image is the evidence.
 *
 * It renders the REAL widgets, never a copy of them: a harness with its own
 * version of the layout would pass while the product was wrong, which is the
 * exact failure it is here to prevent.
 *
 * NOT a pixel-diff test. There is no reference image to compare against —
 * the reference is a design on a canvas, and the comparison is a person
 * looking. What this removes is the guessing, not the judgement.
 */
#pragma once

#include <QString>

namespace halyard::shots {

/* Writes the PNGs into `dir` (created if missing) and returns a process exit
 * status: 0 when every surface was written. */
int run(const QString &dir);

}  // namespace halyard::shots
