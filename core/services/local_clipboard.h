/* local_clipboard - this machine's clipboard, as UTF-8.
 *
 * === CLIP3 2026-10-02 — WHY THIS IS A MODULE AND NOT THREE LINES ===========
 *
 * To make copy/paste work both ways, something has to read and write the
 * clipboard of the machine the client runs ON. That is pure platform: Win32 on
 * the desktop, and NOTHING on Switch or PS Vita, which have no clipboard at
 * all. Keeping it behind this interface is what lets `ctrl_session.c` wire the
 * channel once, for every target, without an `#ifdef` per call site -
 * `device_caps.h` exists in this repo precisely because subtractive platform
 * conditions have broken the Vita port more than once.
 *
 * Off console every function reports "no clipboard here" and the channel simply
 * carries nothing.
 *
 * === THE LOOP THIS MUST NOT CREATE =========================================
 *
 * The hazard is a paste that echoes for ever: we set the local clipboard from
 * the VM, our own change detector sees it change, we send it back, the VM
 * announces an update, we ask for it, we set it again. `local_clipboard_set()`
 * therefore reports the change token its own write produced, and
 * `local_clipboard_changed()` ignores exactly that token. The VM side has the
 * mirror guard (`clip_chan_text_is_new`), so the loop is broken at both ends -
 * one guard would be enough only as long as nobody touched the other end.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when this platform has a clipboard we can read and write. */
bool local_clipboard_available(void);

/* Reads the clipboard as UTF-8 into `out` (`cap` bytes, NUL-terminated), and
 * the byte length into `*n`. Returns false when there is no clipboard, when it
 * holds no text, when it does not fit, or when another process holds it open -
 * all of which are ordinary and none of which is worth an error to the user.
 *
 * Refuses rather than truncates: half a pasted document is worse than none. */
bool local_clipboard_get(char *out, size_t cap, size_t *n);

/* Writes `text` (UTF-8, `n` bytes, no NUL needed) to the clipboard.
 *
 * `*out_token`, when not NULL, receives the change token this write produced.
 * Pass it to `local_clipboard_changed()` so our own write is not mistaken for
 * the user copying something. */
bool local_clipboard_set(const char *text, size_t n, uint64_t *out_token);

/* True when the clipboard has changed since `*token`, and updates `*token`.
 *
 * On Windows this is `GetClipboardSequenceNumber()`: a counter the OS bumps on
 * every change, readable without opening the clipboard and therefore without
 * fighting whatever application owns it. Polling it costs one call; polling by
 * reading the text would cost a lock and a conversion.
 *
 * Seed `*token` with `local_clipboard_token()` at startup, otherwise the first
 * poll reports a change that never happened and sends the user's current
 * clipboard to the VM unasked. */
bool local_clipboard_changed(uint64_t *token);

/* The current change token, for seeding. 0 when unavailable. */
uint64_t local_clipboard_token(void);

#ifdef __cplusplus
}
#endif
