/* ui::theme - palette and metrics shared by the hand-drawn surfaces (the metrics
 * panel, the pause menu).
 *
 * They share a theme for a simple reason: they are two windows onto the same
 * session, and making them look alike avoids the "bricolage" feel we had
 * when each one picked its own greys in its own corner.
 *
 * Unlike devui/, this directory is compiled for EVERY platform: a Switch user
 * wants their metrics and their pause menu too.
 */
#pragma once

#include <nanovg.h>

namespace ui {

/* How a value is judged - the grade picks the colour, not the call site, so a
 * "bonne" metric has the same tint everywhere. */
enum class Grade { Neutral, Good, Warn, Bad };

namespace theme {

/* Surfaces */
inline const NVGcolor panelBg     = nvgRGBA( 18,  20,  26, 226);
inline const NVGcolor panelBorder = nvgRGBA( 92,  99, 120, 110);
inline const NVGcolor backdrop    = nvgRGBA(  0,   0,   0, 200);
inline const NVGcolor rowBg       = nvgRGBA( 44,  49,  63, 255);
inline const NVGcolor rowFocus    = nvgRGBA( 49, 130, 246, 255);
inline const NVGcolor rowDanger   = nvgRGBA(182,  54,  64, 255);

/* Text */
inline const NVGcolor title    = nvgRGBA(238, 240, 246, 255);
inline const NVGcolor label    = nvgRGBA(158, 165, 182, 255);
inline const NVGcolor value    = nvgRGBA(228, 233, 243, 255);
inline const NVGcolor sectionT = nvgRGBA(126, 148, 190, 255);
inline const NVGcolor hint     = nvgRGBA(150, 157, 176, 255);

/* Grades */
inline const NVGcolor good = nvgRGBA(116, 220, 140, 255);
inline const NVGcolor warn = nvgRGBA(238, 206,  96, 255);
inline const NVGcolor bad  = nvgRGBA(240, 112, 112, 255);

inline NVGcolor forGrade(Grade g)
{
    switch (g) {
        case Grade::Good: return good;
        case Grade::Warn: return warn;
        case Grade::Bad:  return bad;
        default:          return value;
    }
}

/* Borealis loads a single font; it is named here so the two frameworks cannot
 * drift apart if it ever changes. */
inline const char *font() { return "regular"; }

}  // namespace theme
}  // namespace ui
