/* kbd_hook_win - give the VM the keys Windows keeps for itself.
 *
 * THE PROBLEM. On Windows the stream view already forwards the physical
 * keyboard to the VM (`stream_view.cpp`, GLFW key callback, evdev scancodes),
 * and `GLFW_KEY_LEFT_SUPER` is mapped to 125 = KEY_LEFTMETA. But the OS acts on
 * some keys BEFORE and BESIDES any application: press Windows and the Start
 * menu opens locally while the keystroke also reaches the VM. The same goes for
 * Alt+Tab, Ctrl+Esc and Alt+Esc. A remote desktop that cannot send the Windows
 * key is a remote desktop you cannot use.
 *
 * WHAT THIS DOES. A `WH_KEYBOARD_LL` hook - the only mechanism that sees those
 * keys before the shell - which, while the stream owns the keyboard:
 *   1. posts the evdev scancode itself, and
 *   2. returns 1, so the key never reaches the shell NOR our own window.
 * Step 2 is why step 1 is necessary: a swallowed key is invisible to GLFW, so
 * the hook has to be the one that forwards it.
 *
 * WHAT IT DELIBERATELY DOES NOT DO.
 *   - It swallows ONLY the keys the OS would otherwise steal (see OWNED below).
 *     Everything else is passed through and handled by the existing GLFW path,
 *     so there is one keyboard route and not two.
 *   - Ctrl+Alt+Del and the Secure Attention Sequence cannot be hooked by
 *     design; nothing here pretends otherwise.
 *   - It is inert unless the stream owns the keyboard
 *     (`halyard_ui_keys_blocked()`, i.e. a session is up and no overlay is
 *     open). Lose focus, open the pause menu, end the session - the keys go
 *     back to Windows immediately. That is the escape hatch, and it is the
 *     reason this cannot lock a user out of their own desktop.
 *
 * `SHADOW_WIN_KBD_HOOK=0` disables it entirely and restores the previous
 * behaviour (Windows keeps its keys, the VM never sees them).
 *
 * Windows only; every function is a no-op elsewhere.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the hook. Safe to call more than once. Returns true if the hook is
 * in place (or already was); false if it is disabled by the toggle or the OS
 * refused it. Must be called from the thread that runs the message loop - GLFW's
 * main thread - because a low-level hook is dispatched to that thread's queue. */
bool kbd_hook_win_install(void);

/* Removes it. Idempotent. Called at shutdown; not calling it is survivable
 * (Windows drops the hook with the process) but leaves it live during a slow
 * exit, which is exactly when a stray swallowed key is hardest to explain. */
void kbd_hook_win_remove(void);

/* True when the hook is installed and currently swallowing. For the status
 * line and the developer menu: a user who has lost their Windows key wants to
 * see, in one glance, whether this is why. */
bool kbd_hook_win_active(void);

/* How many keystrokes it has swallowed this session, for the same reason. */
unsigned kbd_hook_win_swallowed(void);

#ifdef __cplusplus
}
#endif
