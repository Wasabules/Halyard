/* qt_input_map - a Qt key event to an evdev scancode, the way core wants it.
 *
 * === IN1 2026-10-03 — WHY PHYSICAL, NOT THE CHARACTER ======================
 *
 * The VM applies its OWN keyboard layout to whatever scancode it receives, so
 * the client must send the PHYSICAL key that was pressed and let the VM decide
 * the letter. Sending the character instead is wrong even when the two layouts
 * agree: `Qt::Key_A` is the letter A, whose evdev code 30 is the PHYSICAL
 * A-position key - which on a French AZERTY VM is 'Q'. A French user pressing
 * the key labelled A on a French keyboard would type Q. The Borealis client
 * learned this and forwards GLFW physical keys; this does the same from Qt.
 *
 * TWO SOURCES, and which is authoritative depends on the key:
 *
 *   - NAMED keys - arrows, F-keys, modifiers, Enter/Esc/Tab/Space, the
 *     navigation block - have a layout-independent `Qt::Key` and a fixed evdev
 *     code. `evdevForNamedKey()` is the table, copied value-for-value from
 *     Borealis's `glfw_to_evdev` so a report of "key N does nothing" means the
 *     same key in both clients.
 *
 *   - CHARACTER keys - letters, digits, punctuation - are where the physical
 *     position matters, and `QKeyEvent::nativeScanCode()` carries it. On Windows
 *     that is the AT set-1 make code, and the Linux evdev table was built so the
 *     base block (1..88) is IDENTICAL to it: set-1 0x1E == KEY_A == 30. So on
 *     Windows the native scancode IS the evdev code for these keys, with no
 *     table at all. (X11 reports evdev+8 and macOS its own set; those are for
 *     the day this client is measured there - IN1 is Windows-first, like every
 *     measurement in this repo.)
 *
 * `evdevForKeyEvent()` ties the two: a named key wins (its scancode would be
 * ambiguous - Right Ctrl and Left Ctrl share none of it cleanly across the
 * 0xE0 prefix), otherwise the native scancode passes through.
 *
 * Pure: Qt key constants only, no widgets, no I/O. Tested by
 * tests/test_qt_input_map.cpp.
 */
#pragma once

#include <Qt>

namespace halyard {

/* evdev (linux/input-event-codes.h) for a layout-independent named key, or 0
 * when `qtKey` is not one of them (so the caller falls back to the scancode). */
inline int evdevForNamedKey(int qtKey)
{
    switch (qtKey) {
    /* editing / whitespace */
    case Qt::Key_Return:    case Qt::Key_Enter:     return 28;  /* KEY_ENTER */
    case Qt::Key_Escape:                            return 1;   /* KEY_ESC */
    case Qt::Key_Backspace:                         return 14;  /* KEY_BACKSPACE */
    case Qt::Key_Tab:                               return 15;  /* KEY_TAB */
    case Qt::Key_Space:                             return 57;  /* KEY_SPACE */
    case Qt::Key_CapsLock:                          return 58;  /* KEY_CAPSLOCK */

    /* navigation block */
    case Qt::Key_Delete:                            return 111;
    case Qt::Key_Insert:                            return 110;
    case Qt::Key_Home:                              return 102;
    case Qt::Key_End:                               return 107;
    case Qt::Key_PageUp:                            return 104;
    case Qt::Key_PageDown:                          return 109;
    case Qt::Key_Left:                              return 105;
    case Qt::Key_Right:                             return 106;
    case Qt::Key_Up:                                return 103;
    case Qt::Key_Down:                              return 108;

    /* modifiers - Qt collapses left/right into one Key_*; the generic one maps
     * to the LEFT evdev code, which is what a single modifier press should be.
     * Distinguishing the right-hand ones needs the event's native data and is
     * not worth a wrong guess here. */
    case Qt::Key_Shift:                             return 42;  /* KEY_LEFTSHIFT */
    case Qt::Key_Control:                           return 29;  /* KEY_LEFTCTRL */
    case Qt::Key_Alt:                               return 56;  /* KEY_LEFTALT */
    case Qt::Key_AltGr:                             return 100; /* KEY_RIGHTALT */
    case Qt::Key_Meta:                              return 125; /* KEY_LEFTMETA */
    case Qt::Key_Super_L:                           return 125;
    case Qt::Key_Super_R:                           return 126; /* KEY_RIGHTMETA */

    /* function row */
    case Qt::Key_F1:  return 59; case Qt::Key_F2:  return 60;
    case Qt::Key_F3:  return 61; case Qt::Key_F4:  return 62;
    case Qt::Key_F5:  return 63; case Qt::Key_F6:  return 64;
    case Qt::Key_F7:  return 65; case Qt::Key_F8:  return 66;
    case Qt::Key_F9:  return 67; case Qt::Key_F10: return 68;
    case Qt::Key_F11: return 87; case Qt::Key_F12: return 88;

    default:                                        return 0;
    }
}

/* The evdev code to send for a key event, or 0 when none applies.
 *
 * `qtKey` is `QKeyEvent::key()`, `nativeScanCode` is
 * `QKeyEvent::nativeScanCode()`. On a platform where the native scancode is not
 * the evdev base code, pass it already adjusted (X11: subtract 8).
 *
 * Auto-repeat is the caller's to drop: the VM runs its own repeat from the
 * initial press, so a held key must send ONE down, as Borealis does. */
inline int evdevForKeyEvent(int qtKey, unsigned nativeScanCode)
{
    const int named = evdevForNamedKey(qtKey);
    if (named) return named;
    /* Windows AT set-1 base block == evdev; only the base block is a real key,
     * so anything past it is refused rather than sent as a wrong code. */
    if (nativeScanCode >= 1 && nativeScanCode <= 88) return (int)nativeScanCode;
    return 0;
}

}  // namespace halyard
