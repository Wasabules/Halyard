/* K21 2026-09-12 - the Borealis-aware half of `key_label.h`.
 *
 * Kept apart from the table so the table stays pure and testable. This header
 * is the only place that asks Borealis what is plugged in, and the two footers
 * that paint hints (`ui/screen.cpp`, `ui/steps.cpp`) both go through it - a
 * second copy of this query would drift the moment either footer is touched.
 */
#pragma once

#include <cstdlib>
#include <string>

#include <borealis.hpp>

#include "key_label.h"

namespace ui {

/* The text to DRAW for a logical hint id. Returns the id itself when it must
 * keep its glyph, so a caller can always paint what comes back. */
inline std::string hintGlyph(const std::string &logical)
{
#ifdef __GLFW__
    /* SHADOW_UI_KEY_LABELS=0 restores the glyph-only footer exactly. The
     * default is 1 because the measurement that opened this is a report, not a
     * benchmark: on the Windows client the bar named four buttons the machine
     * does not have. */
    static int g_keys = -1;
    if (g_keys < 0) {
        const char *e = getenv("SHADOW_UI_KEY_LABELS");
        g_keys = e ? atoi(e) : 1;
    }
    if (g_keys) {
        /* The condition is the NUMBER OF GAMEPADS, not the platform: on a
         * desktop with a gamepad plugged in, the glyphs are right and a
         * keyboard label would lie. Zero gamepads is the only case where the
         * answer is certain.
         *
         * Known limit, and it is Borealis': `glfw_input.cpp` only LOGS the pads
         * it finds at startup - `controllersCount` moves only in the joystick
         * callback. A pad connected BEFORE the app starts therefore reports as
         * zero and the bar names keys. That is the same blind spot K20 already
         * has in Borealis' own hint bar, so the two stay consistent; fixing it
         * belongs in the vendored input layer, not here. */
        auto *im = brls::Application::getPlatform()->getInputManager();
        const char *k = key_label_for(logical.c_str(),
                                      im ? im->getControllersConnectedCount() : 0,
                                      brls::Application::isSwapInputKeys() ? 1 : 0);
        if (k) return std::string(k);
    }
#endif

#if defined(__vita__) || defined(__psp2__)
    /* THE FOOTER READ "LR / B / A" ON A REAL VITA, 2026-09-13. Everything above
     * is `__GLFW__`-only, so on this console `hintGlyph` returned the logical id
     * untouched and the bar painted the Switch's letters on a machine that has
     * none of them.
     *
     * Unlike the desktop case, the condition is NOT the number of gamepads: the
     * console IS the gamepad, always, and its buttons are always these. Nothing
     * to detect. */
    if (const char *g = psv_label_for(logical.c_str())) return std::string(g);
#endif

    return logical;
}

}  // namespace ui
