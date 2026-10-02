/* local_clipboard - see local_clipboard.h, in particular the loop this must not
 * create. */
#include "local_clipboard.h"

#include <string.h>

#if defined(_WIN32)

#include <windows.h>

/* The token our own last write produced. Module state and not session state:
 * it tracks the OS clipboard, which outlives any session. */
static uint64_t g_self_token;

bool local_clipboard_available(void) { return true; }

uint64_t local_clipboard_token(void)
{
    return (uint64_t)GetClipboardSequenceNumber();
}

/* Opening the clipboard fails while another process holds it - a browser or an
 * editor mid-copy. That is ordinary, so we retry briefly rather than report a
 * failure the user cannot act on. */
static bool open_clipboard_retrying(void)
{
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(NULL)) return true;
        Sleep(10);
    }
    return false;
}

bool local_clipboard_get(char *out, size_t cap, size_t *n)
{
    if (n) *n = 0;
    if (!out || cap == 0) return false;
    out[0] = '\0';

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    if (!open_clipboard_retrying()) return false;

    bool ok = false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w) {
            /* -1 so the terminator is not converted: the VM converts exactly
             * `length` bytes, so a NUL in the payload would become a U+0000
             * inside the pasted text. */
            const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if (need > 1 && (size_t)need <= cap) {
                const int got = WideCharToMultiByte(CP_UTF8, 0, w, -1, out,
                                                    (int)cap, NULL, NULL);
                if (got > 1) {
                    if (n) *n = (size_t)(got - 1);   /* drop the NUL */
                    ok = true;
                }
            }
            /* `need > cap` falls through as a refusal: half a pasted document
             * is worse than none. */
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (!ok) out[0] = '\0';
    return ok;
}

bool local_clipboard_set(const char *text, size_t n, uint64_t *out_token)
{
    if (out_token) *out_token = 0;
    if (!text && n > 0) return false;

    const int wn = (n == 0) ? 0
                 : MultiByteToWideChar(CP_UTF8, 0, text, (int)n, NULL, 0);
    if (n > 0 && wn <= 0) return false;      /* not valid UTF-8 */

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)wn + 1) * sizeof(wchar_t));
    if (!h) return false;

    wchar_t *w = (wchar_t *)GlobalLock(h);
    if (!w) { GlobalFree(h); return false; }
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text, (int)n, w, wn);
    w[wn] = L'\0';
    GlobalUnlock(h);

    if (!open_clipboard_retrying()) { GlobalFree(h); return false; }
    EmptyClipboard();
    /* On success the clipboard OWNS `h`: it must not be freed here. On failure
     * it does not, and it must. */
    const bool ok = SetClipboardData(CF_UNICODETEXT, h) != NULL;
    CloseClipboard();
    if (!ok) { GlobalFree(h); return false; }

    g_self_token = (uint64_t)GetClipboardSequenceNumber();
    if (out_token) *out_token = g_self_token;
    return true;
}

bool local_clipboard_changed(uint64_t *token)
{
    if (!token) return false;
    const uint64_t now = (uint64_t)GetClipboardSequenceNumber();
    if (now == *token) return false;
    *token = now;
    /* Our own write is not the user copying something. Without this the first
     * text from the VM would be echoed straight back to it. */
    if (now == g_self_token) return false;
    return true;
}

#else  /* no clipboard: Switch, PS Vita */

bool     local_clipboard_available(void) { return false; }
uint64_t local_clipboard_token(void)     { return 0; }

bool local_clipboard_get(char *out, size_t cap, size_t *n)
{ (void)cap; if (n) *n = 0; if (out && cap) out[0] = '\0'; return false; }

bool local_clipboard_set(const char *text, size_t n, uint64_t *out_token)
{ (void)text; (void)n; if (out_token) *out_token = 0; return false; }

bool local_clipboard_changed(uint64_t *token) { (void)token; return false; }

#endif
