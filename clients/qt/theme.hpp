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
class QGraphicsDropShadowEffect;

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

/* ---------------------------------------------------------------- surfaces */

/* === UI1 2026-10-03 — A DESIGN SYSTEM, NOT A PILE OF STYLE SHEETS =========
 *
 * The shell (sign-in, machines, connecting) was deliberately plain while the
 * structure was being settled. Dressing it up one widget at a time is how a
 * client ends up with six slightly different greys, so the surfaces and the one
 * application-wide style sheet live HERE, derived from the palette like every
 * other colour in this file - which is what keeps light and dark both right
 * without a theme branch anywhere.
 */
QColor surface(const QWidget *context = nullptr);      /* a card */
QColor surfaceAlt(const QWidget *context = nullptr);   /* a card's header */
QColor border(const QWidget *context = nullptr);
QColor accent(const QWidget *context = nullptr);       /* the one brand colour */

/* The whole application's style sheet. Applied once to qApp; every screen then
 * gets the same buttons, inputs and cards without repeating a rule. A widget
 * opts into the card look with setProperty("card", true). */
/* === D3 2026-10-03 — SHOULD THIS APPLICATION ANIMATE? =====================
 *
 * Three sources, in order: `SHADOW_QT_ANIM=0` (ours, and the escape hatch the
 * house rules ask for), then the system's own "show animations" preference,
 * then yes.
 *
 * The system setting is not a nicety. Animation is a known trigger for
 * vestibular disorders, every desktop exposes a switch for it, and an
 * animation that cannot be turned off is an accessibility defect rather than a
 * style choice. Windows answers through `SPI_GETCLIENTAREAANIMATION`, which is
 * what the "Show animations in Windows" toggle writes; on other platforms we
 * have no portable answer and default to yes.
 *
 * Read once and cached: it is consulted on every animated transition, and a
 * SystemParametersInfo call per card of a staggered list would be absurd. The
 * cost of caching is that toggling it needs a restart, which is the same
 * contract the ~260 SHADOW_* toggles already have. */
bool animationsEnabled();

QString appStyleSheet(const QWidget *context = nullptr);

/* A soft drop shadow for a card. Returns a new effect each call: a
 * QGraphicsEffect belongs to exactly one widget. */
/* === D2 2026-10-03 — LIGHT, DARK, OR WHAT THE SYSTEM SAYS ================
 *
 * Every colour in this file is derived from `QPalette`, so a theme choice is a
 * PALETTE override and not a second stylesheet: set the palette, call
 * `appStyleSheet` again, and cards, pills, borders and accents all follow.
 * That is the whole reason the derivation was built that way.
 *
 * `Auto` leaves the palette Qt resolved from the desktop alone. */
enum class ThemeMode { Auto = 0, Light, Dark };

/* Applies `mode` to the application palette. Returns true when it changed
 * something, so the caller knows whether to re-apply the stylesheet. */
bool applyThemeMode(ThemeMode mode);

QGraphicsDropShadowEffect *elevation(QWidget *on, int radius = 18);

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

/* === ICON1 2026-10-03 — THE TASKBAR WANTS A TILE, NOT A DRAWING ===========
 *
 * `appIcon` is three strokes in the FOREGROUND colour on transparency. Inside
 * the application that is exactly right - it sits on our own surfaces and
 * follows a light or dark theme. In the taskbar it is wrong, and was reported
 * as "a weird grey striped thing": next to every other application's solid,
 * coloured, full-bleed icon, three thin palette-grey lines on nothing do not
 * read as a logo at all. They do not even read as a shape.
 *
 * So the window and application icon is a TILE: a rounded square filled with
 * the brand colour, the mark on it in white. Self-contained, so it owes
 * nothing to the desktop's theme - which is the right call for an icon that
 * Windows will also composite onto a light taskbar, a dark one, a jump list
 * and an alt-tab panel without telling us which.
 *
 * The mark is SIMPLIFIED below 32px: the halyard's curve is a hairline that
 * turns to mud at 16px, which is the size the taskbar actually uses. Under
 * that threshold the sail and the mast carry the mark alone.
 */
QIcon appTileIcon();

}  // namespace halyard::theme
