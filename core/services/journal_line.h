/* journal_line - splitting a log line into its COLUMNS. PURE.
 *
 * === WHY THIS FILE IS HERE AND NOT IN THE SCREEN THAT USES IT ===
 *
 * It describes the format `journal.c` writes, twenty lines from here:
 *
 *     fprintf(g_f, "%s%c/%-8s %s\n", g_horodatage, lettre(sev),
 *             journal_categorie_nom(cat), ligne);
 *
 * that is, `[12.345] I/video    message`. Writer and reader of the same format
 * are therefore neighbours: the day one changes, the other is RIGHT THERE. Filed
 * under `activity/`, this splitting would have followed the screen that uses it
 * and nobody would have thought of it while touching the `fprintf` - the log
 * would have started displaying crooked with no compiler complaining.
 *
 * === PURE, AND ALLOCATION-FREE ===
 *
 * No global state, no I/O, no `getenv`, no `malloc`, no graphics type. The
 * function returns POSITIONS inside the string it is given, not copies: the
 * caller does what it likes with them (the log viewer screen makes
 * `std::string`s out of them). That is also what makes it checkable offline by
 * tests/test_journal_line.c - no console, no SD card, no session.
 *
 * Created 2026-08-29 (S86).
 */
#ifndef SHADOW_JOURNAL_LIGNE_H
#define SHADOW_JOURNAL_LIGNE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The four columns, as positions and lengths within the original string. A
 * missing column has a ZERO length - its position then means nothing and must
 * not be read. The message always runs to the end of the string, so it has no
 * length. */
typedef struct {
    size_t temps_pos, temps_len;   /* "12.345", WITHOUT the brackets */
    size_t cat_pos,   cat_len;     /* « video » */
    size_t text_pos;              /* start of the message, to the end */
    char   sev;                    /* E A I D T, or '?' when unrecognised */
} journal_line_t;

/* The letters `lettre()` can produce in journal.c, and only those.
 *
 * COUNTER-CASE - THE MESSAGE THAT STARTS WITH "x/". Accepting any character
 * followed by a slash would split "1/3 tentatives", on a line with no timestamp,
 * into severity "1" and category "3": the first two characters of the message
 * would DISAPPEAR from the display, silently. A log that loses bytes without
 * saying so is worse than a raw log. */
static inline bool journal_line_is_severity(char c)
{
    return c == 'E' || c == 'A' || c == 'I' || c == 'D' || c == 'T';
}

/* Splits `s`. Never reads past the terminating zero.
 *
 * Both steps - the timestamp, then the severity and category - are OPTIONAL and
 * hand over without consuming anything when they do not recognise what they see.
 *
 * COUNTER-CASE - THE LINE THAT IS NOT IN THE FORMAT. It ends up ENTIRELY in
 * `text_pos`, severity '?', and is therefore displayed in full. That is the case
 * of an archive written by an earlier version - and precisely the one we open to
 * understand a regression. Throwing away what we do not understand would amount
 * to hiding the only interesting line.
 *
 * COUNTER-CASE - THE UNCLOSED BRACKET. "[12.345 message": without the explicit
 * search for the `]`, a naive split would consume the rest of the line as a
 * timestamp. We consume nothing and everything goes into the message. */
static inline journal_line_t journal_line_split(const char *s)
{
    journal_line_t r;
    size_t i = 0;

    r.temps_pos = 0; r.temps_len = 0;
    r.cat_pos   = 0; r.cat_len   = 0;
    r.text_pos = 0;
    r.sev       = '?';

    if (!s) return r;

    /* « [12.345] » */
    if (s[0] == '[') {
        size_t j = 1;
        while (s[j] && s[j] != ']') j++;
        if (s[j] == ']') {
            r.temps_pos = 1;
            r.temps_len = j - 1;
            i = j + 1;
            while (s[i] == ' ') i++;
        }
    }

    /* "I/video   " - the category is padded to eight characters by journal.c:
     * we stop at the first space, then swallow the rest of the padding. */
    if (journal_line_is_severity(s[i]) && s[i + 1] == '/') {
        size_t j;
        r.sev = s[i];
        i += 2;
        j = i;
        while (s[j] && s[j] != ' ') j++;
        r.cat_pos = i;
        r.cat_len = j - i;
        i = j;
        while (s[i] == ' ') i++;
    }

    r.text_pos = i;
    return r;
}

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_JOURNAL_LIGNE_H */
