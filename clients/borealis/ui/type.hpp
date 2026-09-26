/* ui::type - the typographic scale: five steps, each named for its INTENT.
 *
 * === WHY A SCALE AT ALL ===
 *
 * The framework used to carry twelve different sizes (56, 40, 34, 30, 26, 22,
 * 21, 20, 18, 17, 16, 15, 14), each picked on the spot at its call site. That is
 * what makes an interface look "cobbled together" without anyone being able to
 * say why: nothing answers anything else. Two labels playing the same role get
 * two different sizes, and the eye keeps hunting for a hierarchy that is not
 * there.
 *
 * Five steps cover everything we display. They are named for what they are FOR,
 * not for how big they are: a call site that writes `type::CORPS` says what it
 * means, and it follows automatically when the scale moves. Writing `18.0f` says
 * nothing and freezes the decision where it was made.
 *
 * === WHY THESE VALUES ===
 *
 * This screen is looked at from a DISTANCE, controller in hand, sometimes on a
 * TV. So the scale is tighter at the top and more generous at the bottom than a
 * web page scale would be: the ratio between two neighbouring steps is around
 * 1.2 - enough for the hierarchy to read, not enough for body text to shrink
 * into nothing.
 *
 * `MENTION` is the FLOOR: nothing may go below it. At 13 points on a 720p screen
 * seen from a metre away it is already at the edge of comfortable, and the UI
 * scale setting (75 to 200%) is there for the rest.
 */
#pragma once

namespace ui {
namespace type {

/* Screen title. One per page. */
inline const float SCREEN = 34.0f;

/* Section heading, name of a card: what the eye scans. */
inline const float SECTION = 22.0f;

/* Running text: the label of a setting, the name of a step. */
inline const float BODY = 18.0f;

/* Supporting text: a setting's description, the value on the right, a button
 * hint, a status line. */
inline const float SECONDARY = 15.0f;

/* Quiet detail: a step's fine print, a unit, a timer. FLOOR. */
inline const float CAPTION = 13.0f;

/* --- Two sizes OUTSIDE the scale, and why they are allowed ----------------
 *
 * These are not steps: they are two one-of-a-kind elements, each seen once,
 * whose size is dictated by how it is used rather than by the hierarchy.
 */

/* The application name on the boot screen. It has nothing beside it to be
 * compared against, and it is the first contact with the product. */
inline const float HERO = 56.0f;

/* The pairing code. It gets COPIED BY HAND onto a phone, often looking up
 * between two characters: legibility decides its size, not its rank in the
 * hierarchy. */
inline const float CODE = 40.0f;

}  // namespace type
}  // namespace ui
