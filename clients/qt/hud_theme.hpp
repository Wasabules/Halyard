/* The HUD's own palette: Material-dark, flat, no borders.
 *
 * === HUD2 2026-10-03 — WHY THE HUD DOES NOT USE `theme` =================
 *
 * `theme.hpp` derives every colour from the system palette so the app chrome
 * follows a light or a dark desktop. The HUD cannot: it sits on a video, the
 * video is whatever the VM is showing, and a HUD that turned light because
 * the user's desktop is light would be unreadable over a dark game. So this
 * is a FIXED dark set, and that is the whole reason it is a separate header
 * rather than four more functions in `theme`.
 *
 * === THE RULE THAT REPLACED THE BORDERS =================================
 *
 * The first HUD separated things with 1 px coloured borders. Flat/Material
 * separates them with FILL: a surface one step lighter than its parent reads
 * as "on top of", and nothing needs a line. Four steps are enough; a fifth
 * would be a difference nobody can see.
 *
 * And a coloured fill carries DARK ink, never white. White on amber is about
 * 2.5:1 and fails at any size; the dark ink paired with each accent below is
 * 9:1 or better. `inkOn()` picks it, and `tests/test_qt_hud_theme.cpp` holds
 * every pair to 4.5:1.
 */
#pragma once

#include <QColor>

namespace halyard::hud {

/* ---- the surface ladder. Each step is "one level above" the previous. --- */
/* The steps are spaced by LUMINANCE, not by eye. The first set looked right
 * and two of its three steps came out at 1.12 and 1.14 - below what reads as
 * a distinct surface on an LCD, so the "card" and the thing inside it merged
 * into one sheet. The test computes the ratios and refuses anything under
 * 1.12; these are 1.12, 1.20 and 1.26. */
inline QColor bg()     { return QColor(0x0C, 0x0F, 0x15); }
inline QColor surf1()  { return QColor(0x17, 0x1C, 0x26); }   /* a card      */
inline QColor surf2()  { return QColor(0x24, 0x2B, 0x38); }   /* inside one  */
inline QColor surf3()  { return QColor(0x33, 0x3B, 0x4B); }   /* a control   */

/* ---- ink ---- */
inline QColor text()   { return QColor(0xE8, 0xEB, 0xF2); }
/* 6.5:1 on surf1. The first pass used #6E7788, which is ~4:1 - under the
 * threshold for the 10-11 px labels this HUD is made of. */
inline QColor muted()  { return QColor(0xA3, 0xAC, 0xBD); }

/* ---- the accents, and the ink that goes ON them ---- */
inline QColor primary(){ return QColor(0x7F, 0xB5, 0xFF); }
inline QColor good()   { return QColor(0x6F, 0xD9, 0xA6); }
inline QColor warn()   { return QColor(0xFF, 0xB9, 0x60); }
inline QColor bad()    { return QColor(0xFF, 0x8A, 0x80); }

/* The dark ink for a light fill. Each is a dark cousin of its own hue rather
 * than plain black: pure black on amber is harsher than it needs to be, and a
 * tinted ink keeps the chip reading as one object. */
inline QColor inkOn(const QColor &fill)
{
    if (fill == primary()) return QColor(0x0B, 0x1B, 0x30);
    if (fill == good())    return QColor(0x06, 0x24, 0x1A);
    if (fill == warn())    return QColor(0x23, 0x17, 0x05);
    if (fill == bad())     return QColor(0x2C, 0x0A, 0x08);
    /* Anything else: the better of black and white by luminance. The 0.45
     * cut rather than 0.5 is because a mid-tone reads darker than its
     * arithmetic lightness suggests. */
    return fill.lightnessF() > 0.45 ? QColor(0x14, 0x16, 0x1C) : text();
}

/* ---- glass ----
 *
 * `backdrop-filter` does not exist in Qt Widgets, and the Windows acrylic
 * APIs blur the DESKTOP, not the video widget underneath our own tool window
 * - so neither gives glass over our own picture. The blur is computed from
 * the decoded frame we already hold (`VideoWidget::currentFrameImage`), which
 * is the only surface that actually is behind the HUD.
 *
 * The alpha is 0.76 and not the 0.5 the glassmorphism look usually takes:
 * below about 0.7 the muted ink drops under 4.5:1 the moment the picture
 * behind it brightens, and an unreadable metric on a snow level is worse than
 * a panel that is slightly less spectacular. */
inline int glassAlpha(int opacityPct)
{
    /* The user's opacity slider scales it, but never below the floor where
     * the text stops passing. */
    const int a = qRound(194.0 * opacityPct / 100.0);
    return qBound(150, a, 240);
}

/* The radius everything rounds to. One value: two would be a decision nobody
 * can justify at this size. */
inline int radius() { return 12; }

}  // namespace halyard::hud
