/* ui::alpha, ui::duree - the framework's two perceptual scales.
 *
 * === WHY THEY EXIST ===
 *
 * While tuning the look of this interface, two numbers were picked blind about a
 * dozen times, and turned out wrong in both directions:
 *
 *   - the background waves at alpha 10-18: INVISIBLE. Raised to 34-52: they
 *     washed out the whole screen. Removed.
 *   - the sheen on the thumbnails at alpha 40: you could see an OBJECT crossing.
 *     At 18: you feel it pass without being able to point at an edge.
 *
 * The problem was not taste but the absence of a reference point: "26" says
 * nothing, and every call site rediscovered the threshold at its own expense.
 * These scales name what we WANT to achieve, not the value.
 *
 * They are calibrated for a console screen watched from one or two metres, on a
 * dark background. On a light background the same values would look stronger.
 */
#pragma once

namespace ui {

namespace alpha {

/* You do not see it, you FEEL it. This is the threshold for effects that must
 * exist without ever catching the eye: sheens, grain, texture. Above it, the
 * effect becomes a shape you can point at - and a shape behind text makes the
 * text harder to read. */
inline const int SUBLIMINAL = 18;

/* Present if you look for it. For what should be noticeable without imposing
 * itself: a border, a separator, an outline. */
inline const int DISCREET = 46;

/* Read at a glance, without dominating. Status dots, badge backgrounds. */
inline const int MARKED = 120;

/* There is NO dithering constant here, and that is deliberate: laying a pattern
 * over an already-quantised gradient dissolves no banding at all, whatever its
 * amplitude (three measured attempts, see paint.cpp). Gradients are dithered AT
 * THE SOURCE by `ui::paint::degradeVertical`. */

}  // namespace alpha

namespace duration {

/* === Durations, in seconds ===
 *
 * Two families, and they must not be confused:
 *
 * RESPONSES accompany a gesture. They have to be short: past 0.25 s you end up
 * waiting for the animation instead of navigating.
 *
 * AMBIENT motions answer nothing. They have to be SLOW, and the wider the shape
 * the slower still - at equal speed, a wide sweep appears to travel faster than
 * a narrow one, and gets noticed for that reason alone. That was the mistake
 * made on the sheen: widening it without slowing it down cancelled the benefit.
 */
inline const float RESPONSE_QUICK = 0.14f;   /* focus outline */
inline const float RESPONSE       = 0.20f;   /* scroll catching up */
inline const float RESPONSE_SOFT  = 0.22f;   /* content fading in */

inline const float AMBIANCE_LENTE = 9.0f;    /* sheen crossing a thumbnail */

}  // namespace duree
}  // namespace ui
