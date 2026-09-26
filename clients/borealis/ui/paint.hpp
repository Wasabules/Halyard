/* ui::paint - the visual language of the application.
 *
 * This module knows NO screen and NO view: all it can do is paint rectangles,
 * text and a busy indicator. That is deliberate. The in-house framework that
 * replaces the Borealis widgets describes its screens as DATA and redraws them
 * every frame; the only thing that must be shared between them is the look, not
 * objects.
 *
 * Practical consequence: every function here is stateless. None keeps a pointer,
 * none reads the clock - time arrives as a parameter (`t`), read ONCE per frame
 * by the caller, otherwise two animations started in the same frame would not be
 * in phase.
 *
 * --- Two choices that must not be undone ---
 *
 * 1. NO BLUR PASS. The usual "verre" look (a blurred backdrop) needs an
 *    off-screen buffer and at least two full-screen multi-sample passes. On the
 *    Tegra X1 the GPU and the decoder share the same memory bandwidth, and
 *    during a session it is already decoding 1080p H.264 that then has to be
 *    displayed: that is exactly the resource a blur would eat. So we fake glass
 *    by layering instead: dark translucency + gradient + a light edge along the
 *    top + grain. Cost: a handful of quads.
 *
 * 2. THE LIGHT EDGE IS ON TOP ONLY. A light border all the way round reads as an
 *    OUTLINE (a drawn widget); a lit top edge reads as a surface catching light
 *    coming from above - the very light that `shadowBg` puts at the top of the
 *    screen. Without a blur this reflection is the main cue left; removing it, or
 *    closing it into a frame, loses the effect rather than making it subtler.
 *
 * The ui::theme palette (text, grades) stays the reference and is NOT duplicated
 * here: we include it. What is added below are the tints theme.hpp did not have -
 * those of the blue background and of the glass, which did not exist as long as
 * the only hand-painted surfaces were two panels laid over the video.
 */
#pragma once

#include "theme.hpp"

#include <nanovg.h>

#include "type.hpp"
#include <string>

namespace ui {
namespace paint {

/* ------------------------------------------------------------- palette */

/* Page background. The top is a deep night blue, the bottom almost black: that
 * wide spread is what gives depth - a timid gradient just looks like a dirty
 * flat fill. */
inline const NVGcolor nightBlueTop    = nvgRGBA( 16,  26,  54, 255);
inline const NVGcolor nightBlueBottom = nvgRGBA(  4,   6,  14, 255);
/* Diffuse glow at the top: the "light source" of the whole interface. */
inline const NVGcolor topGlow         = nvgRGBA( 58, 112, 214,  52);

/* Glass. The panel body is translucent (alpha < 255): that is what lets the
 * gradient underneath show through and avoids the "grey cardboard glued on
 * top" look. */
inline const NVGcolor glassFill       = nvgRGBA( 17,  23,  38, 198);
inline const NVGcolor glassFillBottom = nvgRGBA( 10,  14,  25, 216);
inline const NVGcolor glassHighlight  = nvgRGBA(196, 220, 255,  70);
inline const NVGcolor glassBorder     = nvgRGBA(120, 146, 196,  40);
inline const NVGcolor glassGrain      = nvgRGBA(198, 216, 255,  13);
inline const NVGcolor shadowColor     = nvgRGBA(  0,   0,   0, 120);

/* List cards. */
inline const NVGcolor cardBgTop          = nvgRGBA( 38,  50,  78, 255);
inline const NVGcolor cardBgBottom       = nvgRGBA( 24,  32,  52, 255);
inline const NVGcolor cardBgFocusTop     = nvgRGBA( 46,  84, 146, 255);
inline const NVGcolor cardBgFocusBottom  = nvgRGBA( 28,  52,  98, 255);
inline const NVGcolor cardBorder       = nvgRGBA(104, 124, 168,  48);
/* A "switched off" card: we do not grey the text (unreadable on this
 * background), we take the light out of the background. The difference has to
 * be visible without reading anything. */
inline const NVGcolor cardBgMuted        = nvgRGBA( 30,  36,  50, 255);

/* Accent. `accent` is exactly theme::rowFocus: the pause menu and the in-house
 * framework can appear on screen at the same time, and two neighbouring but
 * distinct blues would look like a mistake there. `accentVif` is its lit
 * version, for the edge of the focused element, which has to stand out against
 * the blue background. */
inline const NVGcolor accent     = nvgRGBA( 49, 130, 246, 255);
inline const NVGcolor accentVif  = nvgRGBA(122, 184, 255, 255);
inline const NVGcolor accentHalo = nvgRGBA( 49, 130, 246, 140);

/* Status LED on a card (machine running / stopped). */
inline const NVGcolor ledActive  = nvgRGBA(116, 220, 140, 255);   /* = theme::good */
inline const NVGcolor ledOff = nvgRGBA( 96, 104, 124, 210);

/* ------------------------------------------------------------ metrics */

inline const float PANEL_RADIUS = 16.0f;
inline const float CARD_RADIUS   = 12.0f;
inline const float BADGE_HEIGHT = 22.0f;
inline const float BADGE_FONT    = type::CAPTION;   /* follows the scale */

/* --------------------------------------------------------- backgrounds */

/* Page background: vertical night-blue gradient + diffuse glow at the top.
 * Call it first, over the whole screen area. */
void shadowBg(NVGcontext *vg, float x, float y, float w, float h);

/* Dithering: breaks up the banding of an 8-bit gradient.
 *
 * A long, low-contrast gradient shows its steps - the background has one every
 * 60 pixels. The maths is right; it is the DISPLAY that lacks the precision. So
 * we overlay noise whose amplitude is below the quantisation step: the eye
 * averages it out and no longer sees the boundary.
 * One repeated 64x64 texture, hence ONE fill whatever the area. */
/* Vertical gradient DITHERED AT THE SOURCE - a drop-in replacement for
 * `nvgLinearGradient(vg, x, y, x, y + h, haut, bas)`, except that it does not
 * show its bands. Returns an NVGpaint: the path is still the caller's to
 * choose (rectangle, rounded corners...).
 *
 * A long, low-contrast 8-bit gradient makes a visible STAIRCASE; it cannot be
 * fixed after the fact, only computed in floating point and quantised with
 * error diffusion. See the long comment in paint.cpp: it gives the measurements
 * and the three approaches that were refuted. */
NVGpaint degradeVertical(NVGcontext *vg, float x, float y, float h,
                         NVGcolor top, NVGcolor bottom);

/* A sheet of light sweeping across a rounded shape.
 *
 * === THIS FUNCTION EXISTS BECAUSE I FAILED TWICE ===
 *
 * It encodes two NanoVG gradient traps, each paid for once:
 *
 *   1. A GRADIENT IS CLAMPED. Past its end point it keeps its end colour, so a
 *      plain `transparent -> light` lit up the whole right-hand side of the card
 *      in one block, with a hard edge sweeping across it. Two halves are needed,
 *      a rise then a fall.
 *   2. A DIAGONAL GRADIENT GIVES THE SCISSOR AWAY. If its value also depends on
 *      height, it is not zero at the scissor edge: you get a hard vertical seam.
 *      So both halves are HORIZONTAL - same `y` at both ends, hence exactly zero
 *      at the edges.
 *
 * And two settings that are not a matter of taste:
 *   - the sheet is WIDER than the shape (2.2 times). Any narrower and the
 *     transition is too short: it reads as a line crossing the card, not as
 *     light.
 *   - its travel runs from "entirely off to the left" to "entirely off to the
 *     right", so its appearance and disappearance are never seen.
 *
 * `phase`: 0 to 1, the position along that travel. The caller computes it and so
 * chooses the period - see ui::duree::AMBIANCE_LENTE. */
void lightSheen(NVGcontext *vg, float x, float y, float w, float h,
                float radius, float phase, int peak_alpha);

/* Slow ripples at the bottom of the page. The only motion in the application
 * that carries no information: it is there so the screen feels alive.
 * Deliberately at the edge of perception - a wave you notice becomes a pattern,
 * and a pattern behind text makes the text harder to read.
 * No blur, no off-screen buffer: filled curves, about forty segments each. The
 * GPU is already decoding 1080p next door. */
void backgroundWaves(NVGcontext *vg, float x, float y, float w, float h, double t);

/* Translucent glass-like panel. See the file header for why the light edge is
 * on top only and why there is no blur. */
void glassPanel(NVGcontext *vg, float x, float y, float w, float h, float radius);

/* A list card (a machine, a setting...).
 *   focused : this is the element the cursor is on -> accent + halo;
 *   active  : the thing it represents is running -> live background; otherwise
 *             a muted one;
 *   led     : draw the status dot in the top-right corner.
 * The text is NOT drawn here: a card does not know its own content.
 *
 * === S78 2026-08-29 - THE DOT IS NOT AN ORNAMENT ===
 *
 * It used to be drawn on EVERY card, settings rows included. But a status dot
 * answers the question "is this thing running?", which only means something for
 * a MACHINE. On a setting it was either redundant with the switch sitting twenty
 * pixels away, or - on a choice or an action - permanently grey, that is, an
 * indicator observing nothing.
 *
 * That is worse than one ornament too many: a grey dot reads as "off", hence as
 * an unavailable setting. A settings page showed a dozen of them, and nothing
 * said which ones were genuinely inert. So `led` is an EXPLICIT parameter:
 * whoever draws a card has to say whether they have a state to show. */
void card(NVGcontext *vg, float x, float y, float w, float h,
           bool focused, bool active, bool led, double t);

/* Status pill. (x, y) = TOP LEFT corner; height = BADGE_HEIGHT.
 * `tint` is used for the text, the border and the background (at reduced
 * alpha): a single colour to pass, so no mongrel combination is possible. */
void badge(NVGcontext *vg, float x, float y, const char *text, NVGcolor tint);

/* The width `badge` will take - needed to place whatever comes next to it, since
 * badge() returns nothing. It sets its own font size, just like badge(). */
float badgeWidth(NVGcontext *vg, const char *text);

/* A button hint: the round pill carrying the letter, followed by the label.
 * Returns the total width taken, so the caller can lay several of them side by
 * side without measuring them itself.
 *
 * The pill is ROUND, not rounded: that is the shape of the console's buttons,
 * and it tells itself apart at a glance from a status pill (`badge`), which is a
 * capsule. Two different kinds of information must not share a silhouette.
 *
 * K21 2026-09-12: unless the name does not fit in the circle. Since the footer
 * also names KEYBOARD KEYS (`ui/key_label.h`), a hint can read "Enter" or
 * "L/R", which spilled out of the circle over its neighbours. Such a hint gets
 * a third silhouette - a rounded rectangle, a key - and the width follows the
 * text. Which shape you get is decided by MEASURING the name, not by counting
 * its characters: "↑" is three bytes and one glyph. */
float hintButton(NVGcontext *vg, float x, float y, const char *button,
                 const char *label);

/* The width `hintBouton` will take, drawing nothing. */
float hintWidth(NVGcontext *vg, const char *button, const char *label);

/* ---------------------------------------------------------------- text */

/* Text width IN THE CURRENT STATE (the caller's font and size): measuring at a
 * size other than the one used to draw is the surest way to make a label
 * overflow. */
float textWidth(NVGcontext *vg, const char *txt);

/* `txt` cut to `maxw`, with an ellipsis. Cuts on a UTF-8 boundary. */
std::string truncated(NVGcontext *vg, const std::string &txt, float maxw);

/* Draw `txt` bounded to `maxw`. Font and colour are the caller's: this function
 * only deals with width.
 *   scroll = true  -> the text scrolls (back and forth, with a pause at each
 *                     end); reserve it for the SELECTED row, scrolling a whole
 *                     page at once makes it unreadable;
 *   scroll = false -> cut with an ellipsis.
 * (clipY, clipH) bounds the scissor vertically, `t` is the time in seconds.
 * Either way a scissor bounds the drawing: even if the width computation is
 * wrong, no pixel escapes the area. */
void clampedText(NVGcontext *vg, float x, float y, float maxw,
                float clipY, float clipH,
                const std::string &txt, bool scroll, double t);

/* ------------------------------------------------------------- waiting */

/* Busy indicator. `t` is the time in seconds, supplied by the caller: one clock
 * read per frame (see the file header). */
void spinner(NVGcontext *vg, float cx, float cy, float r, double t);

}  // namespace paint
}  // namespace ui
