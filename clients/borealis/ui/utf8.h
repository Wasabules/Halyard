/* ui::utf8 - the only two UTF-8 operations the interface needs.
 *
 * === WHY THIS FILE EXISTS ===
 *
 * The same computation had been written THREE times in this repo, in three
 * places that knew nothing of each other: truncating an over-long label, the
 * monogram on a thumbnail, and the character-by-character advance in the pause
 * menu. Each time the same loop over continuation bytes, each time with the same
 * chance of getting it wrong.
 *
 * And getting it wrong does NOT show up immediately: cutting "Emile" after its
 * first byte yields a replacement character, which goes unnoticed as long as you
 * only test with unaccented names. This application's reference locale is French
 * - there is no such thing as an accent-free name here.
 *
 * Pure, no allocation, no dependency: checkable offline.
 */
#ifndef UI_UTF8_H
#define UI_UTF8_H

#include <stddef.h>

/* Length IN BYTES of the character starting at `s[i]`.
 *
 * Returns at least 1, even on an invalid sequence: a malformed string must still
 * advance the caller's loop, otherwise it spins forever. That is the classic
 * failure of this kind of function, and it is far worse than one badly rendered
 * character. */
static inline size_t ui_utf8_len(const char *s, size_t n, size_t i)
{
    if (!s || i >= n) return 0;
    size_t j = i + 1;
    while (j < n && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
    return j - i;
}

/* Index of the character FOLLOWING the one that starts at `i`. */
static inline size_t ui_utf8_next(const char *s, size_t n, size_t i)
{
    const size_t l = ui_utf8_len(s, n, i);
    return l ? i + l : n;
}

#endif /* UI_UTF8_H */
