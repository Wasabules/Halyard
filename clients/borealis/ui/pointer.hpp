/* ui::pointer — the demo pointer, for screen recordings.
 *
 * === WHY IT EXISTS ===
 *
 * A screen capture shows the mouse differently depending on the recorder, and
 * it NEVER shows the click: you watch a menu open without knowing whether the
 * user clicked, double-clicked, or whether the application decided on its own.
 * To show a walkthrough, the gesture is what has to be visible, not just its
 * result.
 *
 * So this replaces the cursor with a DOT that changes state:
 *   at rest    — a discreet ring, saying where you are without drawing the eye
 *   on click   — a filled disc plus a ripple spreading outward, marking the
 *                INSTANT of the click and staying visible half a second after
 *
 * The ripple matters as much as the disc: on video, a state change lasting three
 * frames goes unnoticed. That is the difference between "you can see the cursor"
 * and "you can see the click".
 *
 * Desktop only: the console has no mouse, and its touch input already has
 * feedback of its own.
 */
#pragma once

#include <borealis.hpp>

namespace ui {
namespace pointer {

/* Enabled? Read from settings, re-read live. */
void setEnabled(bool on);
bool enabled();

/* Call at the end of rendering, above everything else: a pointer drawn beneath
 * a panel would be hidden by it at the very moment you want to see it. */
void draw(NVGcontext *vg, double t);

/* === 2026-09-02 - THE MOUSE, READABLE BY A SCREEN ===
 *
 * `ui_touch_current()` (ui/touch.h) answers "where is the finger" on console and
 * returns nothing off it, so a self-drawn screen that wants to be usable with a
 * mouse had nowhere to ask. The values were already read here, once a frame, to
 * paint the dot - and thrown away.
 *
 * Fills `x`/`y` in Borealis' LOGICAL frame (the same one `paint()` draws in) and
 * `pressed` with the button's LEVEL, not its edge: a caller that needs the click
 * instant does its own rising-edge detection, because only the caller knows what
 * a click means on its screen.
 *
 * Returns false on console and whenever there is no mouse to read, leaving the
 * outputs untouched. Independent of `setEnabled` - that flag governs the drawn
 * dot, not whether a mouse exists. */
bool state(float &x, float &y, bool &pressed);

}  // namespace pointer
}  // namespace ui
