/* theme - the one place that owns colour, spacing and type.
 *
 * === QT4 2026-10-03 — WHY A THEME MODULE BEFORE ANY SCREEN =================
 *
 * The first settings window hardcoded `color: palette(mid)` for its secondary
 * text. On a dark desktop theme that is dark grey on dark grey: unreadable, and
 * reported as such. The defect is not the colour, it is that a screen chose one
 * at all — the second screen would have chosen a different wrong one.
 *
 * So: no widget in this client names a colour. It asks here, and here the
 * answer is DERIVED FROM THE PALETTE, which means it follows the system theme
 * in both directions without a dark-mode branch anywhere.
 *
 * `muted()` blends the window's text colour toward its background. At 45% it
 * is clearly secondary and still readable on light and on dark — which
 * `palette(mid)` is not, because `mid` is a FRAME shade, not a text shade, and
 * nothing guarantees it contrasts with the window.
 *
 * === THE SCALES ============================================================
 *
 * Four spacings and three type steps, and that is on purpose. A scale with ten
 * values is a scale nobody respects; the Borealis client's own UI settled on
 * about this many (`ui/type.hpp` has HERO / SCREEN / BODY / SECONDARY) and it
 * has held.
 *
 * Minimalist by decision, not by omission: system palette, system font, no
 * stylesheet beyond what the palette cannot express. A visual pass can replace
 * this file without touching a screen.
 */
#pragma once

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPalette>
#include <QString>

class QWidget;

namespace halyard::theme {

/* ------------------------------------------------------------------ colour */

/* Secondary text: readable, clearly not primary, on any theme. */
QColor muted(const QWidget *context = nullptr);

/* A value that is set from outside and cannot be changed here. Warmer than
 * muted so the difference is visible without reading the label. */
QColor overridden(const QWidget *context = nullptr);

QColor good(const QWidget *context = nullptr);
QColor warn(const QWidget *context = nullptr);
QColor bad(const QWidget *context = nullptr);

/* `color: #rrggbb;` for a stylesheet, from one of the above. Widgets use this
 * rather than writing a literal, so there is exactly one way to be wrong. */
QString css(const QColor &c);

/* ----------------------------------------------------------------- spacing */

/* Four steps. Anything between them is a decision somebody should justify. */
constexpr int SpaceTight  = 4;
constexpr int SpaceRow    = 10;
constexpr int SpaceGroup  = 20;
constexpr int SpacePage   = 28;

/* ------------------------------------------------------------------- type */

/* Relative to the system font, so a user who has set a larger one keeps it. */
QFont titleFont(const QWidget *context = nullptr);     /* a window's heading */
QFont headingFont(const QWidget *context = nullptr);   /* a section */
QFont monoFont(const QWidget *context = nullptr);      /* an identifier */

/* -------------------------------------------------------------------- mark */

/* The application's mark, drawn rather than loaded.
 *
 * A halyard is the line that hoists a sail, so the mark is a mast, a hoisted
 * sail and the line that raised it — three strokes. Drawn with QPainter at the
 * requested size instead of shipping a PNG, for three reasons: it is sharp at
 * every DPI, it costs no asset to deploy next to the binary (which the DLL
 * bundling already made delicate), and it takes the FOREGROUND COLOUR, so it
 * is correct on a light and on a dark title bar without a second file.
 */
QIcon appIcon();
QIcon appIcon(const QColor &ink);

}  // namespace halyard::theme
