/* kbd_hook_win - see kbd_hook_win.h for why this exists and what it refuses
 * to do. */
#include "kbd_hook_win.h"

#if defined(_WIN32)

#include <stdlib.h>
#include <windows.h>

#include "shadow_input.h"
#include "../common/log.h"

/* JOURNAL_CAT_INPUT is "mouse, keyboard, touch" - this is the keyboard. */
#define klog(...) JOURNAL_INFO_(JOURNAL_CAT_INPUT, __VA_ARGS__)

/* Published by stream_view.cpp: a session is up AND no overlay (pause menu,
 * on-screen keyboard) is open. Weak so a build without the stream view - or a
 * unit test - links and simply never swallows. */
extern bool halyard_ui_keys_blocked(void) __attribute__((weak));

static HHOOK    g_hook;
static unsigned g_swallowed;
static int      g_enabled = -1;      /* -1 = toggle not read yet */

/* === THE KEYS WE TAKE, AND ONLY THESE ======================================
 *
 * Each entry is a key Windows would act on locally, with the evdev scancode the
 * Shadow input channel expects (the same table `stream_view.cpp` uses for the
 * GLFW path - they must agree, so the values are repeated here with their
 * names rather than hidden behind a shared helper that would hide a mismatch).
 *
 * Alt+Tab, Ctrl+Esc and Alt+Esc are not separate keys: they are Tab and Escape
 * pressed while a modifier is down. We therefore own Tab and Escape ONLY while
 * Alt or Ctrl is held - otherwise Escape would stop closing our own pause menu,
 * which is the one key a stuck user reaches for first. */
#define EVDEV_LEFTMETA   125
#define EVDEV_RIGHTMETA  126
#define EVDEV_TAB         15
#define EVDEV_ESC          1

static int owned_scancode(DWORD vk, bool *conditional)
{
    *conditional = false;
    switch (vk) {
        case VK_LWIN: return EVDEV_LEFTMETA;
        case VK_RWIN: return EVDEV_RIGHTMETA;
        /* Only with a modifier down - see above. */
        case VK_TAB:    *conditional = true; return EVDEV_TAB;
        case VK_ESCAPE: *conditional = true; return EVDEV_ESC;
        default: return -1;
    }
}

static bool modifier_down(void)
{
    /* GetAsyncKeyState's high bit = currently down. ALT or CTRL is what turns
     * Tab and Escape into shell shortcuts. */
    return (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0
        || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

static LRESULT CALLBACK ll_proc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code != HC_ACTION) return CallNextHookEx(g_hook, code, wparam, lparam);

    const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lparam;
    if (!k) return CallNextHookEx(g_hook, code, wparam, lparam);

    /* An injected event is one WE or another program synthesised. Swallowing it
     * would make this hook fight whatever produced it, and forwarding it would
     * double every key the VM already received. */
    if (k->flags & LLKHF_INJECTED)
        return CallNextHookEx(g_hook, code, wparam, lparam);

    /* The escape hatch, evaluated on EVERY keystroke and never cached: the
     * moment the stream stops owning the keyboard, Windows gets its keys back. */
    if (!halyard_ui_keys_blocked || !halyard_ui_keys_blocked())
        return CallNextHookEx(g_hook, code, wparam, lparam);

    bool conditional = false;
    const int sc = owned_scancode(k->vkCode, &conditional);
    if (sc < 0 || (conditional && !modifier_down()))
        return CallNextHookEx(g_hook, code, wparam, lparam);

    const bool pressed = (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN);
    const bool released = (wparam == WM_KEYUP || wparam == WM_SYSKEYUP);
    if (!pressed && !released)
        return CallNextHookEx(g_hook, code, wparam, lparam);

    /* We are the only route for this key now, so we post it ourselves. */
    shadow_input_post_scancode((uint16_t)sc, pressed);

    if (pressed) {
        g_swallowed++;
        /* Logged for the first few only: a user hunting a missing Windows key
         * needs to see that it went to the VM, and a stream of lines at the
         * typing rate would bury the session's real events. */
        if (g_swallowed <= 5)
            klog("[WINKEY] vk=0x%02x -> evdev %d sent to the VM, swallowed locally "
                 "(%u so far, SHADOW_WIN_KBD_HOOK=0 to disable)",
                 (unsigned)k->vkCode, sc, g_swallowed);
    }
    return 1;   /* eaten: neither the shell nor our own window sees it */
}

bool kbd_hook_win_install(void)
{
    if (g_enabled < 0) {
        const char *e = getenv("SHADOW_WIN_KBD_HOOK");
        g_enabled = e ? atoi(e) : 1;
    }
    if (!g_enabled) {
        klog("[WINKEY] hook disabled (SHADOW_WIN_KBD_HOOK=0): Windows keeps the "
             "Windows key, Alt+Tab, Ctrl+Esc and Alt+Esc");
        return false;
    }
    if (g_hook) return true;

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_proc, NULL, 0);
    if (!g_hook) {
        klog("[WINKEY] SetWindowsHookEx FAILED (err=%lu) - the Windows key will "
             "keep opening the Start menu instead of reaching the VM",
             (unsigned long)GetLastError());
        return false;
    }
    klog("[WINKEY] low-level keyboard hook installed: Windows key, and Tab/Escape "
         "while Alt or Ctrl is held, go to the VM while the stream has the focus");
    return true;
}

void kbd_hook_win_remove(void)
{
    if (!g_hook) return;
    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    klog("[WINKEY] hook removed (%u keystroke(s) swallowed this session)",
         g_swallowed);
}

bool kbd_hook_win_active(void)
{
    return g_hook != NULL
        && halyard_ui_keys_blocked && halyard_ui_keys_blocked();
}

unsigned kbd_hook_win_swallowed(void) { return g_swallowed; }

#else  /* !_WIN32 */

bool     kbd_hook_win_install(void)   { return false; }
void     kbd_hook_win_remove(void)    { }
bool     kbd_hook_win_active(void)    { return false; }
unsigned kbd_hook_win_swallowed(void) { return 0; }

#endif
