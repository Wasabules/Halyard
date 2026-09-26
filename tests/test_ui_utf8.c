/* test_ui_utf8.c - cutting UTF-8.
 *
 * This computation was written THREE times in this repo before being filed
 * away: truncating a long text, the monogram of a tile, the pause menu's
 * advance. Each check therefore names what it stops us from breaking again.
 */
#include "../clients/borealis/ui/utf8.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

int main(void)
{
    printf("== UTF-8 cutting ==\n");

    /* ASCII: one byte per character. */
    const char *neo = "Neo";
    CHECK(ui_utf8_len(neo, 3, 0) == 1, "ASCII: one letter = one byte");
    CHECK(ui_utf8_next(neo, 3, 0) == 1, "ASCII: the next one is at +1");

    /* COUNTER-CASE - THE ACCENTED LETTER.
     * "Émile" starts with an E-acute, which is TWO bytes. Cutting at the first
     * one displays a replacement character - and that goes unnoticed as long as
     * you try with unaccented names. This repo is written in French: there is no
     * unaccented name that holds. */
    const char *emile = "\xC3\x89mile";
    CHECK(ui_utf8_len(emile, strlen(emile), 0) == 2,
            "COUNTER-CASE: \u00ab E acute \u00bb is TWO bytes, not one");
    CHECK(ui_utf8_next(emile, strlen(emile), 0) == 2,
            "COUNTER-CASE: the next character starts at +2");

    /* Three and four bytes: an arrow and an emoji. The virtual keyboard's
     * labels carry arrows. */
    const char *fleche = "\xE2\x86\x90";               /* U+2190 */
    CHECK(ui_utf8_len(fleche, 3, 0) == 3, "an arrow is three bytes");
    const char *emoji = "\xF0\x9F\x8E\xAE";            /* U+1F3AE */
    CHECK(ui_utf8_len(emoji, 4, 0) == 4, "an emoji is four bytes");

    /* COUNTER-CASE - THE INFINITE LOOP.
     * An invalid sequence must still ADVANCE: returning 0 would spin the
     * calling loop forever, which is far worse than a badly displayed
     * character - the application freezes instead of being ugly. */
    const char *casse = "\x80\x80\x41";                /* continuations orphelines */
    CHECK(ui_utf8_len(casse, 3, 0) >= 1,
            "COUNTER-CASE: an invalid sequence still advances");
    size_t i = 0, tours = 0;
    while (i < 3 && tours < 10) { i = ui_utf8_next(casse, 3, i); tours++; }
    CHECK(i >= 3, "COUNTER-CASE: the loop reaches the end, it does not spin");

    /* Bornes. */
    CHECK(ui_utf8_len(NULL, 3, 0) == 0, "a null pointer returns 0");
    CHECK(ui_utf8_len(neo, 3, 3) == 0, "an index at the end of the string returns 0");
    CHECK(ui_utf8_len(neo, 3, 9) == 0, "an out-of-range index returns 0");
    CHECK(ui_utf8_next(neo, 3, 3) == 3, "the next after the end stays the end");

    /* A string TRUNCATED mid-character: we do not read past it. */
    CHECK(ui_utf8_len(emile, 1, 0) == 1,
            "COUNTER-CASE: a string cut in the middle of a character - we do not read "
            "never past the given length");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
