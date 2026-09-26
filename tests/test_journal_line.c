/* test_journal_line.c - splitting a log line into columns
 * (core/services/journal_line.h).
 *
 * The log viewer screen (S86) is useless if it does not separate the severity
 * from the rest: it is the colour of that letter that makes a page readable at a
 * glance. But this splitting reads a format ANOTHER file writes (`journal.c`,
 * `fprintf(g_f, "%s%c/%-8s %s\n", ...)`), and nothing in the compiler ties the
 * two together. This test is that tie.
 *
 * Two families of checks, and the second is the file's real reason for existing:
 *   - the nominal format splits exactly, column by column;
 *   - a line that IS NOT IN THE FORMAT does not lose a byte. That is the case of
 *     an archive written by an earlier version - the very one we open to
 *     understand a regression.
 *
 * Each check names its COUNTER-CASE: what would be displayed crooked if the rule
 * were undone.
 */
#include "../core/services/journal_line.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* Compares a column (position + length within `s`) against the expected
 * string. */
static bool col(const char *s, size_t pos, size_t len, const char *expected)
{
    const size_t n = strlen(expected);
    return len == n && strncmp(s + pos, expected, n) == 0;
}

/* ── Le format nominal ────────────────────────────────────────────────────── */

static void nominal_format(void)
{
    /* Exactly what journal.c writes: timestamp in brackets, severity letter,
     * slash, category padded to eight characters, space, message. */
    const char *s = "[12.345] I/video    frame 42 decoded";
    const journal_line_t r = journal_line_split(s);

    CHECK(col(s, r.temps_pos, r.temps_len, "12.345"),
            "the timestamp is returned WITHOUT its brackets (the column replaces them)");
    CHECK(r.sev == 'I', "the severity letter is extracted");
    CHECK(col(s, r.cat_pos, r.cat_len, "video"),
            "the category without the eight-character padding");
    CHECK(strcmp(s + r.text_pos, "frame 42 decoded") == 0,
            "the message starts after the padding, not in the middle of the spaces");

    /* COUNTER-CASE - THE PADDING SPACES LEFT IN THE MESSAGE. `%-8s` pads the
     * category to eight characters: "video" leaves three. Keeping them would
     * shift every message by an amount that DEPENDS on its category, and the
     * log's left column would be jagged - the exact opposite of what this
     * splitting is for. */
    CHECK(s[r.text_pos] != ' ',
            "COUNTER-CASE: the message never starts with a padding space");

    /* A category that fills all eight characters has no padding left: a single
     * space separates it from the message. */
    {
        const char *l = "[0.001] E/heritage message";
        const journal_line_t q = journal_line_split(l);
        CHECK(q.sev == 'E', "a full-width category: the severity is read all the same");
        CHECK(col(l, q.cat_pos, q.cat_len, "heritage"),
                "an eight-character category: nothing is truncated");
        CHECK(strcmp(l + q.text_pos, "message") == 0,
                "a full-width category: the message follows the single space");
    }

    /* The five letters journal.c can produce, and only those. */
    {
        const char *lettres = "EAIDT";
        int i;
        for (i = 0; i < 5; i++) {
            char l[40];
            journal_line_t q;
            snprintf(l, sizeof l, "[1.000] %c/audio    ok", lettres[i]);
            q = journal_line_split(l);
            CHECK(q.sev == lettres[i], "each of the five severities is recognised");
        }
    }
}

/* -- What is not in the format ------------------------------------------- */

static void malformed(void)
{
    /* COUNTER-CASE - "1/3 tentatives".
     *
     * This is THE trap of the splitting, and it is silent. Accepting any
     * character followed by a slash would read severity "1", category "3", and
     * the first two characters of the message would disappear from the screen
     * with no error message to say so. So we recognise ONLY journal.c's five
     * letters. */
    {
        const char *s = "1/3 tentatives";
        const journal_line_t r = journal_line_split(s);
        CHECK(r.sev == '?', "COUNTER-CASE: \u00ab 1/ \u00bb is not a severity");
        CHECK(r.cat_len == 0, "COUNTER-CASE: \u00ab 3 \u00bb is not a category");
        CHECK(strcmp(s + r.text_pos, "1/3 tentatives") == 0,
                "COUNTER-CASE: the line shows IN FULL, not shorn of \u00ab 1/ \u00bb");
    }

    /* A valid letter but WITHOUT the slash: that is not a column. */
    {
        const char *s = "[2.000] Erreur de lecture";
        const journal_line_t r = journal_line_split(s);
        CHECK(r.sev == '?', "\u00ab Erreur \u00bb starts with E but is not followed by /");
        CHECK(strcmp(s + r.text_pos, "Erreur de lecture") == 0,
                "the whole word stays in the message");
        CHECK(col(s, r.temps_pos, r.temps_len, "2.000"),
                "the timestamp is read even when the severity is not");
    }

    /* COUNTER-CASE - THE UNCLOSED BRACKET. A naive split looking "after the
     * first space" would swallow "[12.345" as a timestamp and display the rest
     * truncated. With no `]`, we consume nothing. */
    {
        const char *s = "[12.345 message with no closing bracket";
        const journal_line_t r = journal_line_split(s);
        CHECK(r.temps_len == 0, "COUNTER-CASE: no \u00ab ] \u00bb, no timestamp");
        CHECK(strcmp(s + r.text_pos, s) == 0,
                "COUNTER-CASE: the whole line goes into the message");
    }

    /* A line from an earlier version: neither brackets nor columns. That is
     * exactly the content of an archive one opens to compare. */
    {
        const char *s = "[VIDEO] 42 chunks recus";
        const journal_line_t r = journal_line_split(s);
        CHECK(col(s, r.temps_pos, r.temps_len, "VIDEO"),
                "a closed bracket is read as a timestamp, for want of anything better");
        CHECK(r.sev == '?', "no severity is invented");
        CHECK(strcmp(s + r.text_pos, "42 chunks recus") == 0,
                "the rest is displayed without loss");
    }

    /* Severity and category WITHOUT a timestamp: the two steps are
     * independent, and the second must not depend on the first. */
    {
        const char *s = "A/reseau   socket fermee";
        const journal_line_t r = journal_line_split(s);
        CHECK(r.temps_len == 0, "no timestamp: zero length");
        CHECK(r.sev == 'A', "the severity is read with no timestamp in front");
        CHECK(col(s, r.cat_pos, r.cat_len, "reseau"), "the category is read with no timestamp");
        CHECK(strcmp(s + r.text_pos, "socket fermee") == 0, "the message is read with no timestamp");
    }
}

/* ── Les entrees degenerees ───────────────────────────────────────────────── */

static void degenerees(void)
{
    /* COUNTER-CASE - THE EMPTY STRING. `journal_last_lines` skips empty lines,
     * but this module must not ASSUME its caller: an out-of-range read here
     * would crash the screen at the moment one comes to it looking for the cause
     * of a crash. */
    {
        const journal_line_t r = journal_line_split("");
        CHECK(r.temps_len == 0 && r.cat_len == 0 && r.text_pos == 0,
                "COUNTER-CASE: an empty string -> everything zero, no out-of-range read");
        CHECK(r.sev == '?', "an empty string: no severity is invented");
    }

    /* A null pointer returns a neutral struct rather than dereferencing.
     * `journal_last_lines` may leave entries at NULL beyond the count it filled,
     * and a careless caller would walk them. */
    {
        const journal_line_t r = journal_line_split(NULL);
        CHECK(r.sev == '?' && r.text_pos == 0,
                "COUNTER-CASE: a null pointer -> a neutral struct, no dereference");
    }

    /* A lone opening bracket, then the end of the string. */
    {
        const char *s = "[";
        const journal_line_t r = journal_line_split(s);
        CHECK(r.temps_len == 0, "a lone \u00ab [ \u00bb: no timestamp");
        CHECK(strcmp(s + r.text_pos, "[") == 0, "a lone \u00ab [ \u00bb: returned as is");
    }

    /* Empty brackets: a zero-length timestamp, not an overflow. */
    {
        const char *s = "[] I/ihm      ok";
        const journal_line_t r = journal_line_split(s);
        CHECK(r.temps_len == 0, "crochets vides : longueur nulle");
        CHECK(r.sev == 'I', "empty brackets: what follows still splits");
        CHECK(strcmp(s + r.text_pos, "ok") == 0, "crochets vides : message intact");
    }

    /* A severity at the end of the string, with no category and no message. */
    {
        const char *s = "[1.000] D/";
        const journal_line_t r = journal_line_split(s);
        CHECK(r.sev == 'D', "the severity is read even with no category behind it");
        CHECK(r.cat_len == 0, "an empty category: zero length");
        CHECK(s[r.text_pos] == '\0', "an empty message: we point at the terminating zero");
    }
}

/* -- A property that holds for any input --------------------------------- */

static void never_out_of_range(void)
{
    /* The rule that holds everything together: `text_pos` NEVER goes past the
     * string's length, and every column stays inside it. The caller builds
     * `std::string`s from these positions - one overrun and the screen reads
     * memory that is not its own.
     *
     * We check it on every PREFIX of a nominal line: that is the simplest way to
     * sweep every intermediate state of the parse, including those where the
     * string stops right in the middle of a column. */
    const char *modele = "[12.345] I/video    frame 42 decoded";
    const size_t n = strlen(modele);
    size_t k;

    for (k = 0; k <= n; k++) {
        char buf[64];
        journal_line_t r;
        memcpy(buf, modele, k);
        buf[k] = '\0';
        r = journal_line_split(buf);

        CHECK(r.text_pos <= k, "prefix: the message starts INSIDE the string");
        CHECK(r.temps_pos + r.temps_len <= k, "prefix: the timestamp stays inside the string");
        CHECK(r.cat_pos + r.cat_len <= k, "prefix: the category stays inside the string");
    }
}

int main(void)
{
    printf("== splitting a log line into columns ==\n");
    nominal_format();
    malformed();
    degenerees();
    never_out_of_range();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
