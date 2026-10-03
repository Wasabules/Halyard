/* test_qt_input_map - the Qt key -> evdev mapping (IN1).
 *
 * The danger this pins is the one in the header: sending the CHARACTER instead
 * of the PHYSICAL key, which types the wrong letter on any non-US layout. The
 * named-key table is checked value-for-value against Borealis's glfw_to_evdev
 * (the two clients must mean the same key), and the character path is checked to
 * pass the native scancode through unchanged on the base block and refuse
 * anything outside it.
 *
 * Pure: Qt6Core only (the Qt:: key constants), no widgets.
 */
#include <QtGlobal>

#include <cstdio>

#include "../clients/qt/qt_input_map.hpp"

using halyard::evdevForNamedKey;
using halyard::evdevForKeyEvent;

static int checks = 0, failures = 0;

static void eq(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL %-46s got %d, expected %d\n", what, got, want);
    }
}

int main()
{
    std::printf("== Qt key -> evdev (IN1 2026-10-03) ==\n");

    /* Named keys, against Borealis's glfw_to_evdev. */
    eq(evdevForNamedKey(Qt::Key_Return), 28, "Enter");
    eq(evdevForNamedKey(Qt::Key_Enter),  28, "keypad Enter folds to Enter");
    eq(evdevForNamedKey(Qt::Key_Escape),  1, "Esc");
    eq(evdevForNamedKey(Qt::Key_Space),  57, "Space");
    eq(evdevForNamedKey(Qt::Key_Tab),    15, "Tab");
    eq(evdevForNamedKey(Qt::Key_Backspace), 14, "Backspace");
    eq(evdevForNamedKey(Qt::Key_Left),  105, "Left");
    eq(evdevForNamedKey(Qt::Key_Right), 106, "Right");
    eq(evdevForNamedKey(Qt::Key_Up),    103, "Up");
    eq(evdevForNamedKey(Qt::Key_Down),  108, "Down");
    eq(evdevForNamedKey(Qt::Key_Home),  102, "Home");
    eq(evdevForNamedKey(Qt::Key_End),   107, "End");
    eq(evdevForNamedKey(Qt::Key_PageUp),   104, "PageUp");
    eq(evdevForNamedKey(Qt::Key_PageDown), 109, "PageDown");
    eq(evdevForNamedKey(Qt::Key_Insert), 110, "Insert");
    eq(evdevForNamedKey(Qt::Key_Delete), 111, "Delete");
    eq(evdevForNamedKey(Qt::Key_Shift),   42, "Shift -> left");
    eq(evdevForNamedKey(Qt::Key_Control), 29, "Ctrl -> left");
    eq(evdevForNamedKey(Qt::Key_Alt),     56, "Alt -> left");
    eq(evdevForNamedKey(Qt::Key_AltGr),  100, "AltGr -> right alt");
    eq(evdevForNamedKey(Qt::Key_Meta),   125, "Meta -> left super");
    eq(evdevForNamedKey(Qt::Key_Super_R),126, "right super");
    eq(evdevForNamedKey(Qt::Key_F1),  59, "F1");
    eq(evdevForNamedKey(Qt::Key_F12), 88, "F12");
    eq(evdevForNamedKey(Qt::Key_CapsLock), 58, "CapsLock");

    /* A character key is NOT in the named table - it must fall through to the
     * scancode, which is the whole point of IN1. */
    eq(evdevForNamedKey(Qt::Key_A), 0, "a letter is not a named key");
    eq(evdevForNamedKey(Qt::Key_1), 0, "a digit is not a named key");

    /* The event path: a named key wins over whatever scancode came with it. */
    eq(evdevForKeyEvent(Qt::Key_Left, 999), 105,
       "a named key ignores its (possibly extended) scancode");

    /* A character key passes its physical scancode through on the base block.
     * On Windows set-1: A=0x1E=30, Q=0x10=16, 1=0x02=2. */
    eq(evdevForKeyEvent(Qt::Key_A, 30), 30, "physical A-position passes through");
    eq(evdevForKeyEvent(Qt::Key_Q, 16), 16, "physical Q-position passes through");
    eq(evdevForKeyEvent(Qt::Key_1, 2),   2, "physical 1 passes through");

    /* The AZERTY case the header exists for: a French user presses the key
     * labelled A (physical Q-position, scancode 16). We must send 16, NOT the
     * letter A's 30 - the VM's layout turns 16 into 'A'. */
    eq(evdevForKeyEvent(Qt::Key_A, 16), 16,
       "character comes from the PHYSICAL position, not the letter");

    /* Out of the base block is refused rather than sent as a wrong code. */
    eq(evdevForKeyEvent(Qt::Key_unknown, 0),   0, "no scancode -> nothing");
    eq(evdevForKeyEvent(Qt::Key_unknown, 200), 0, "past the base block -> nothing");

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
