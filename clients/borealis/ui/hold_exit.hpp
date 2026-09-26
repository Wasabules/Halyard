/* HOLD-1 2026-09-12 - the platform half of `hold_exit.h`.
 *
 * The timing lives next door, pure and tested. What is here is the one thing
 * that genuinely differs between platforms: how you read the "go back" button.
 *
 * On console that is libnx, unchanged - the three testers have measured their
 * hold that way since they were written, and there is no reason to move a
 * working console path in order to fix a desktop one. Everywhere else it is
 * Borealis' unified controller state, which is the RIGHT answer rather than a
 * convenient one: `glfw_input.cpp` adds the keyboard on top of the gamepad
 * buttons (Escape feeds BUTTON_B) and it honours the A/B swap while doing so.
 * Reading the Escape key directly would have exited on the wrong key one time
 * in two, and would have ignored a gamepad plugged into a desktop.
 */
#pragma once

#include <cstdlib>

#include <borealis.hpp>
#include "../devlink/inject.hpp"   /* INJ-1: synthetic input is read here too */

#include "hold_exit.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace ui {

/* One slot per tester. MODULE scope, not a function `static`: this repo names
 * that family of defects first, and a tester resuming a half-finished hold from
 * its previous opening is exactly the shape it takes. */
enum HoldSlot { HOLD_NET = 0, HOLD_MOUSE = 1, HOLD_PAD = 2, HOLD_SLOTS = 3 };
inline hold_exit_t g_hold_exit[HOLD_SLOTS] = {};

#ifdef __SWITCH__
inline PadState g_hold_pad;
inline bool     g_hold_pad_ready = false;
#endif

/* Is the button that means "go back" down right now? */
inline bool backButtonDown()
{
#ifdef __SWITCH__
    if (!g_hold_pad_ready) {
        padConfigureInput(8, HidNpadStyleSet_NpadStandard);
        padInitializeAny(&g_hold_pad);
        g_hold_pad_ready = true;
    }
    padUpdate(&g_hold_pad);
    /* INJ-1: the injected hold is ADDED to the real button. Holding is the
     * ONLY way out of these pages, so without this line a script gets stuck in
     * one - while the command channel keeps answering perfectly, which makes
     * the symptom incomprehensible. */
    const uint64_t held = padGetButtons(&g_hold_pad) | devlink::injectedNpadMask();
    return (held & HidNpadButton_B) != 0;
#else
    auto *im = brls::Application::getPlatform()->getInputManager();
    if (!im) return false;
    brls::ControllerState st {};
    im->updateUnifiedControllerState(&st);
    /* Borealis also feeds BUTTON_B from the RIGHT MOUSE BUTTON. That is its
     * mapping everywhere in the application, so a tester that disagreed would
     * be the odd one out - but it is worth knowing in the pointer tester, where
     * holding the right button for the full duration does leave the page. */
    return st.buttons[brls::BUTTON_B];
#endif
}

/* How long the button must be held, in seconds.
 *
 * SHADOW_UI_HOLD_EXIT, in MILLISECONDS. `0` disables the hold entirely, which
 * restores the pre-HOLD-1 desktop behaviour exactly - a page with no way out.
 * It is a revert path, not a suggestion. The default of 800 ms is the value the
 * three testers already used on console; nothing measured says to change it. */
inline double holdExitMs()
{
    static int g_ms = -1;
    if (g_ms < 0) {
        const char *e = getenv("SHADOW_UI_HOLD_EXIT");
        g_ms = e ? atoi(e) : 800;
        if (g_ms < 0) g_ms = 800;
    }
    return (double)g_ms;
}

/* One frame of the hold for `slot`. True exactly once, when it completes. */
inline bool holdExitStep(HoldSlot slot, double t, float *progress)
{
    const double ms = holdExitMs();
    if (ms <= 0.0) {                    /* disabled: never exits, empty bar */
        if (progress) *progress = 0.0f;
        return false;
    }
    return hold_exit_step(&g_hold_exit[slot], backButtonDown() ? 1 : 0, t,
                          ms / 1000.0, progress) != 0;
}

}  // namespace ui
