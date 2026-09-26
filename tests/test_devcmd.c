/* test_devcmd.c - the parser for the development machine's commands
 * (clients/borealis/devlink/devcmd.h).
 *
 * This module is the ONLY place in the [devlink] channel where we read bytes we
 * did not produce: a line arriving over the log socket, read into a 256-byte
 * STACK buffer by the drain thread (webrtc/log.c:56-68). Everything that follows
 * therefore checks two things at once: that we understand the legitimate
 * commands, and that no hostile line makes us read, write or act beyond what was
 * asked.
 *
 * THE FILE'S METHOD - every parse goes through `parse()`, which copies the text
 * into a heap block of the EXACT SIZE and passes it WITHOUT a null terminator.
 * Under ASan, reading a single byte too many therefore lands in the red zone and
 * fails the suite, whatever check happens to be running. That is not one test
 * among the others: it is the property the other ~150 checks verify alongside
 * themselves.
 *
 * Each check names its COUNTER-CASE: the exact input that would produce a real
 * defect. A failure here therefore says what has just been undone.
 */
#include "../clients/borealis/devlink/devcmd.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* Parses `text` from a block of the exact size, WITHOUT a null terminator.
 * See the file header: it is the whole suite's overflow net. */
static bool parse(const char *texte, devcmd_t *c)
{
    const size_t n = strlen(texte);
    char *buf = (char *)malloc(n > 0 ? n : 1);
    bool r;

    if (!buf) { printf("  malloc a echoue\n"); exit(2); }
    memcpy(buf, texte, n);
    r = devcmd_parse(buf, n, c);
    free(buf);
    return r;
}

/* A shortcut: is the line REFUSED? Checks at the same time that the returned
 * struct is entirely zeroed. */
static bool refused(const char *texte)
{
    devcmd_t c;
    memset(&c, 0xAA, sizeof(c));            /* pre-dirtied: a field left as-is shows up */
    if (parse(texte, &c)) return false;
    return c.kind == DEVCMD_UNKNOWN && c.a == 0 && c.b == 0 && c.name[0] == '\0';
}

/* -- The three argument-less verbs --------------------------------------- */

static void simple_verbs(void)
{
    devcmd_t c;

    CHECK(parse("shot", &c) && c.kind == DEVCMD_SHOT, "shot");
    CHECK(parse("state", &c) && c.kind == DEVCMD_STATE, "state");

    /* COUNTER-CASE - THE ONLY COMMAND THAT ALREADY EXISTED.
     * `quit` (S45) is what makes it possible to send a new .nro without touching
     * the console: the file stays locked as long as the application runs.
     * Breaking it would not break a feature to come, it would break the CURRENT
     * iteration loop - and the symptom would be "the upload fails", a thousand
     * miles from a command parser. */
    CHECK(parse("quit", &c) && c.kind == DEVCMD_QUIT,
            "COUNTER-CASE: `quit` still works (the S45 tooling)");

    /* An argument-less verb fills neither the numbers nor the name: a caller
     * that read `a` by mistake must find 0 there, not a leftover. */
    CHECK(parse("shot", &c) && c.a == 0 && c.b == 0 && c.name[0] == '\0',
            "shot leaves neither a coordinate nor a name behind");
}

/* ── Case, whitespace, line endings ───────────────────────────────────────── */

static void case_and_spacing(void)
{
    devcmd_t c;

    /* DECISION - CASE IS IGNORED, both ways, verb AND argument.
     * These lines are typed by hand in a terminal. Refusing a capital protects
     * nothing (the vocabulary stays closed, and that is what protects) and would
     * produce an incomprehensible "err" for someone who just typed the right
     * command. The returned name is ALWAYS the lowercase form, so the caller has
     * only one form to compare against. */
    CHECK(parse("SHOT", &c) && c.kind == DEVCMD_SHOT, "DECISION: \u00ab SHOT \u00bb accepted");
    CHECK(parse("shot", &c) && c.kind == DEVCMD_SHOT, "DECISION: \u00ab shot \u00bb accepted too");
    CHECK(parse("ShOt", &c) && c.kind == DEVCMD_SHOT, "casse melangee acceptee");
    CHECK(parse("QUIT", &c) && c.kind == DEVCMD_QUIT, "\u00ab QUIT \u00bb accepted");
    CHECK(parse("BTN A", &c) && c.kind == DEVCMD_BTN && c.a == DEVCMD_BTN_A,
            "the argument is folded to lowercase too");
    CHECK(parse("BTN A", &c) && strcmp(c.name, "a") == 0,
            "DECISION: the returned name is the lowercase form, never the received one");
    CHECK(parse("Nav Left", &c) && c.a == DEVCMD_NAV_LEFT, "« Nav Left »");

    /* Multiple spaces, tabs, edges: a hand-typed line has them. */
    CHECK(parse("   shot", &c) && c.kind == DEVCMD_SHOT, "spaces before the verb");
    CHECK(parse("shot   ", &c) && c.kind == DEVCMD_SHOT, "spaces after the verb");
    CHECK(parse("btn    a", &c) && c.a == DEVCMD_BTN_A, "multiple spaces between the words");
    CHECK(parse("\tbtn\ta\t", &c) && c.a == DEVCMD_BTN_A, "tabulations");
    CHECK(parse("  tap   12    34   ", &c) && c.a == 12 && c.b == 34,
            "multiple spaces everywhere in a tap");

    /* COUNTER-CASE - THE \r\n LEFT ATTACHED.
     * TCP is a stream: nothing guarantees the upstream splitting removed the end
     * of line, and a Windows machine sends two characters. A parser that takes
     * them for bytes of the last word refuses EVERY command from that machine,
     * without ever saying why. */
    CHECK(parse("shot\r\n", &c) && c.kind == DEVCMD_SHOT, "CONTRE-CAS : « shot\\r\\n »");
    CHECK(parse("quit\n", &c) && c.kind == DEVCMD_QUIT, "« quit\\n »");
    CHECK(parse("quit\r", &c) && c.kind == DEVCMD_QUIT, "« quit\\r » seul");
    CHECK(parse("tap 5 6\r\n", &c) && c.a == 5 && c.b == 6, "« tap 5 6\\r\\n »");
    CHECK(parse("btn zl \r\n", &c) && c.a == DEVCMD_BTN_ZL, "espace puis \\r\\n");

    /* A null in the middle ends the line like a \n: the caller is allowed to
     * pass a C string with an `n` wider than its contents. */
    {
        devcmd_t d;
        CHECK(devcmd_parse("shot\0zzzz", 9, &d) && d.kind == DEVCMD_SHOT,
                "a null in the middle ends the line");
    }
}

/* ── Ligne empty et bruit ──────────────────────────────────────────────────── */

static void empty_and_noise(void)
{
    /* COUNTER-CASE - THE EMPTY LINE.
     * The drain thread re-reads the socket every 100 ms and splits on "\r\n": a
     * lone "\r\n" frame, or a space left by an `echo`, does arrive for real.
     * Nothing must trigger, and above all nothing must read the first byte of an
     * empty buffer. */
    CHECK(refused(""), "COUNTER-CASE: an empty line -> UNKNOWN, without reading a byte");
    CHECK(refused("\r\n"), "CONTRE-CAS : « \\r\\n » seul -> INCONNUE");
    CHECK(refused("\n"), "« \\n » seul -> INCONNUE");
    CHECK(refused("   "), "CONTRE-CAS : espaces seuls -> INCONNUE");
    CHECK(refused("\t\t"), "tabulations seules -> INCONNUE");
    CHECK(refused("   \r\n"), "spaces then end of line -> UNKNOWN");

    /* Vocabulaire ferme : ni prefixe, ni suffixe, ni voisin. */
    CHECK(refused("sho"), "a truncated verb is refused");
    CHECK(refused("shotx"), "a verb with one extra character is refused");
    CHECK(refused("shoot"), "a typo is refused");
    CHECK(refused("s"), "a single letter is refused");
    CHECK(refused("ping"), "a command belonging to ANOTHER handler is not ours");
    CHECK(refused("autotest 1 0 0"), "the same for `autotest` (autotest.cpp)");

    /* COUNTER-CASE - THE RECOGNISED VERB WITH LEFTOVERS.
     * The old handler compared `strncmp(line, "quit", 4)`: "quittez la piece"
     * therefore stopped the application mid-session. A recognised verb whose line
     * continues is not the command you think it is - it is a command we do not
     * understand. */
    CHECK(refused("quitter"), "CONTRE-CAS : « quitter » n'arrete PLUS l'application");
    CHECK(refused("quit now"), "COUNTER-CASE: \u00ab quit now \u00bb is refused");
    CHECK(refused("shot screen"), "COUNTER-CASE: an extra word after shot -> refused");
    CHECK(refused("state 1"), "an extra argument after state -> refused");

    /* Binary noise: the channel can receive anything, including bytes that are
     * negative in a signed char (0x80..0xFF) - the very ones that would make
     * tolower() undefined. */
    CHECK(refused("\x01\x02\x03"), "control bytes -> UNKNOWN");
    CHECK(refused("\xff\xfe"), "high bytes (negative in a signed char) -> UNKNOWN");
    CHECK(refused("btn \xff"), "a name of high bytes is not a button");
}

/* -- The buffer with no null terminator ----------------------------------- */

static void without_a_null_terminator(void)
{
    /* COUNTER-CASE - THE LINE THAT EXACTLY FILLS THE BUFFER.
     * log.c reads into a STACK `char buf[256]` and guarantees nothing past the
     * bytes received. A parser calling strlen/strcmp/sscanf directly on those
     * bytes reads the drain thread's stack up to the first zero it meets - at
     * best it refuses a valid command, at worst it turns it into another one. ALL
     * of this file already goes through blocks of the exact size; these few cases
     * say so explicitly, with the lengths that matter (verb alone, complete line,
     * giant word). */
    devcmd_t c;
    size_t   i;
    char    *bloc;

    {
        char *exact = (char *)malloc(4);
        memcpy(exact, "shot", 4);
        CHECK(devcmd_parse(exact, 4, &c) && c.kind == DEVCMD_SHOT,
                "COUNTER-CASE: \u00ab shot \u00bb in exactly 4 bytes, with no null");
        free(exact);
    }
    {
        char *exact = (char *)malloc(4);
        memcpy(exact, "quit", 4);
        CHECK(devcmd_parse(exact, 4, &c) && c.kind == DEVCMD_QUIT,
                "COUNTER-CASE: \u00ab quit \u00bb in exactly 4 bytes, with no null");
        free(exact);
    }
    {
        char *exact = (char *)malloc(11);
        memcpy(exact, "tap 123 456", 11);
        CHECK(devcmd_parse(exact, 11, &c) && c.a == 123 && c.b == 456,
                "COUNTER-CASE: a tap in exactly 11 bytes - the last digit counts");
        free(exact);
    }
    {
        /* The same block, announced one byte too short: the command changes
         * meaning and must show it, not overrun to find what it expected. */
        char *exact = (char *)malloc(11);
        memcpy(exact, "tap 123 456", 11);
        CHECK(devcmd_parse(exact, 10, &c) && c.a == 123 && c.b == 45,
                "a shorter n: we stop at n, we do not go looking for the 6");
        CHECK(!devcmd_parse(exact, 3, &c), "\u00ab tap \u00bb alone (n=3) is refused");
        CHECK(devcmd_parse(exact, 0, &c) == false, "n = 0 is refused without reading");
        free(exact);
    }

    /* A giant word with no null: the read loop must stop on `end`, not on a zero
     * it will never find. */
    bloc = (char *)malloc(200);
    for (i = 0; i < 200; i++) bloc[i] = 'a';
    CHECK(!devcmd_parse(bloc, 200, &c), "200 'a' with no null: refused, without overrunning");
    memcpy(bloc, "btn ", 4);
    CHECK(!devcmd_parse(bloc, 200, &c),
            "COUNTER-CASE: \u00ab btn \u00bb + 196 characters - refused, `name` intact");
    free(bloc);

    /* COUNTER-CASE - THE LINE LONGER THAN THE BOUND.
     * We REFUSE instead of truncating: cutting the line back to the bound would
     * fabricate commands nobody sent - "tap 1 99999..." cut becomes "tap 1 999",
     * a valid touch at an arbitrary point on the screen. */
    bloc = (char *)malloc(DEVCMD_LINE_MAX + 1);
    memset(bloc, ' ', DEVCMD_LINE_MAX + 1);
    memcpy(bloc, "shot", 4);
    CHECK(devcmd_parse(bloc, DEVCMD_LINE_MAX, &c) && c.kind == DEVCMD_SHOT,
            "a line exactly at the bound is accepted");
    CHECK(!devcmd_parse(bloc, DEVCMD_LINE_MAX + 1, &c),
            "COUNTER-CASE: one byte past the bound -> REFUSED, never truncated");
    free(bloc);
}

/* ── Boutons ──────────────────────────────────────────────────────────────── */

static void buttons(void)
{
    static const struct { const char *name; int idx; } table[] = {
        { "a", DEVCMD_BTN_A }, { "b", DEVCMD_BTN_B }, { "x", DEVCMD_BTN_X },
        { "y", DEVCMD_BTN_Y }, { "l", DEVCMD_BTN_L }, { "r", DEVCMD_BTN_R },
        { "zl", DEVCMD_BTN_ZL }, { "zr", DEVCMD_BTN_ZR },
        { "plus", DEVCMD_BTN_PLUS }, { "minus", DEVCMD_BTN_MINUS },
    };
    devcmd_t c;
    char     line[64];
    size_t   i;
    int      tous_ok = 1, noms_ok = 1;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        snprintf(line, sizeof(line), "btn %s", table[i].name);
        if (!parse(line, &c) || c.kind != DEVCMD_BTN || c.a != table[i].idx)
            tous_ok = 0;
        if (strcmp(c.name, table[i].name) != 0) noms_ok = 0;
    }
    CHECK(tous_ok, "the 10 buttons each return their index");
    CHECK(noms_ok, "each button also returns its canonical name");

    /* The returned index is used as an index into a mapping table in the
     * caller: it must ALWAYS be within the enum's bounds, otherwise we fall right
     * back onto the defect ui/nav.h was written to kill. */
    CHECK(parse("btn minus", &c) && c.a >= 0 && c.a < (int)DEVCMD_BTN_COUNT,
            "the returned index fits inside the enum");

    /* COUNTER-CASE - THE 200-CHARACTER NAME.
     * `devcmd_t.name` is 16 bytes. A parser that copies first and bounds
     * afterwards writes 200 bytes into 16 - on the drain thread's stack, hence
     * over its return address. What prevents it is devcmd_token()'s write bound:
     * removed, that single line makes the suite fall over with a
     * "stack-buffer-overflow" under ASan (mutation verified). Comparing the REAL
     * length against the field's size is what makes the refusal an explicit
     * decision rather than a happy consequence of the vocabulary being short. */
    {
        char geant[240];
        memset(geant, 'a', sizeof(geant));
        memcpy(geant, "btn ", 4);
        geant[sizeof(geant) - 1] = '\0';
        CHECK(refused(geant), "COUNTER-CASE: \u00ab btn \u00bb + 200 characters -> refused, no overflow");
    }
    /* The same thing just above the field's bound (16): that is the exact
     * boundary, the one a "<=" instead of a "<" would miss. */
    {
        char juste_dedans[4 + DEVCMD_NAME_MAX];   /* "btn " + 15 lettres + nul */
        char just_outside[5 + DEVCMD_NAME_MAX];   /* "btn " + 16 lettres + nul */
        memset(juste_dedans, 'a', sizeof(juste_dedans)); memcpy(juste_dedans, "btn ", 4);
        memset(just_outside, 'a', sizeof(just_outside)); memcpy(just_outside, "btn ", 4);
        juste_dedans[sizeof(juste_dedans) - 1] = '\0';
        just_outside[sizeof(just_outside) - 1] = '\0';
        CHECK(refused(juste_dedans),
                "a name that FITS in the field (15) is refused: it is not in the vocabulary");
        CHECK(refused(just_outside),
                "a name of exactly 16 characters is refused: that is the field's boundary");
    }

    CHECK(refused("btn"), "\u00ab btn \u00bb with no argument -> UNKNOWN");
    CHECK(refused("btn "), "\u00ab btn \u00bb followed by a space -> UNKNOWN");
    CHECK(refused("btn c"), "a button that does not exist -> UNKNOWN");
    CHECK(refused("btn start"), "\u00ab start \u00bb is not in the vocabulary (it is plus/minus)");
    CHECK(refused("btn a b"), "COUNTER-CASE: two buttons on one line -> refused, not the first one");
    CHECK(refused("btn 0"), "a digit is not a button name");
}

/* ── Navigation ───────────────────────────────────────────────────────────── */

static void navigation(void)
{
    devcmd_t c;

    CHECK(parse("nav up", &c) && c.kind == DEVCMD_NAV && c.a == DEVCMD_NAV_UP, "nav up");
    CHECK(parse("nav down", &c) && c.a == DEVCMD_NAV_DOWN, "nav down");
    CHECK(parse("nav left", &c) && c.a == DEVCMD_NAV_LEFT, "nav left");
    CHECK(parse("nav right", &c) && c.a == DEVCMD_NAV_RIGHT, "nav right");
    CHECK(parse("nav right", &c) && strcmp(c.name, "right") == 0, "nav returns its name");

    /* COUNTER-CASE - THE ENGLISH VOCABULARY.
     * The protocol is in French (haut/bas/gauche/droite). Accepting "up" just in
     * case means opening two vocabularies of which only one is tested; the
     * refusal tells the other end at once that it is speaking the wrong
     * dialect. */
    CHECK(refused("nav haut"), "COUNTER-CASE: the OLD French word is refused -"
            " the vocabulary is closed, and changing it must break loudly");
    CHECK(refused("nav"), "\u00ab nav \u00bb with no direction -> UNKNOWN");
    CHECK(refused("nav up down"), "two directions on one line -> refused");
    CHECK(refused("nav hau"), "a truncated direction is refused");
}

/* ── The numbers `tap` takes ──────────────────────────────────────────────── */

static void tap_and_overflow(void)
{
    devcmd_t c;

    CHECK(parse("tap 0 0", &c) && c.kind == DEVCMD_TAP && c.a == 0 && c.b == 0,
            "the origin is a valid position");
    CHECK(parse("tap 640 360", &c) && c.a == 640 && c.b == 360, "the middle of a 1280x720 screen");
    CHECK(parse("tap 1919 1079", &c) && c.a == 1919 && c.b == 1079, "the corner of a 1080p screen");
    CHECK(parse("tap 16384 16384", &c) && c.a == 16384 && c.b == 16384,
            "the bound itself is accepted");
    CHECK(parse("tap 007 042", &c) && c.a == 7 && c.b == 42, "leading zeros are tolerated");

    /* COUNTER-CASE - THE INTEGER OVERFLOW IN THE PARSE.
     * Twenty digits. The naive form (accumulate everything into an int then
     * compare against the bound) OVERFLOWS, and a signed overflow is undefined
     * behaviour: the compiler is free to delete the comparison that follows. The
     * repo has already paid for this exact mistake - `pb_skip_field` returned a
     * DECREASING offset and a 12-byte message hung the control thread forever
     * (KB §9, 2026-08-25). Here the price would be a touch at an arbitrary
     * coordinate, hence an automated run that "clicks" at random and whose log
     * nobody will understand.
     * The bound is applied ON EVERY DIGIT: the refusal comes on the fifth, well
     * before any possible overflow. */
    CHECK(refused("tap 99999999999999999999 0"),
            "COUNTER-CASE: 20 digits -> REFUSED, never a valid coordinate by truncation");
    CHECK(refused("tap 0 99999999999999999999"), "the same on the second coordinate");
    CHECK(refused("tap 4294967296 0"), "2^32 is refused (it does not wrap to 0)");
    CHECK(refused("tap 2147483648 0"), "INT_MAX + 1 is refused");
    CHECK(refused("tap 16385 0"), "one step past the bound -> refused");
    CHECK(refused("tap 0 16385"), "the same on y");

    /* The PROPERTY that makes the digit-by-digit loop incapable of overflowing:
     * the bound must leave room for one last "x10 + 9" without reaching INT_MAX.
     * Raising DEVCMD_COORD_MAX towards INT_MAX would silently reintroduce the
     * overflow - verified: taken to 2147483000, UBSan trips on the same "signed
     * integer overflow" as the naive version. */
    CHECK(DEVCMD_COORD_MAX <= (INT_MAX - 9) / 10,
            "COUNTER-CASE: the coordinate bound stays far from INT_MAX");

    /* COUNTER-CASE - THE NEGATIVE COORDINATE.
     * An accepted `-5` travels down to the caller, which uses it to designate a
     * list item: that is a negative index, the very mistake ui/nav.h was written
     * to make impossible. The sign is not a digit: it is refused by construction,
     * not by a check one could forget. */
    CHECK(refused("tap -5 10"), "COUNTER-CASE: \u00ab tap -5 10 \u00bb -> REFUSED (no negative index)");
    CHECK(refused("tap 10 -5"), "COUNTER-CASE: negative on y too");
    CHECK(refused("tap -0 0"), "even \u00ab -0 \u00bb is refused");
    CHECK(refused("tap +5 10"), "an explicit plus is refused too (a single format)");

    /* What looks like a number without being one. */
    CHECK(refused("tap 1a 2"), "digits followed by a letter -> refused");
    CHECK(refused("tap a1 2"), "a letter followed by digits -> refused");
    CHECK(refused("tap 0x10 2"), "hexadecimal is not accepted");
    CHECK(refused("tap 1.5 2"), "a decimal point is not accepted");
    CHECK(refused("tap 1e3 2"), "nor is scientific notation");
    CHECK(refused("tap  12"), "a single coordinate -> refused");
    CHECK(refused("tap"), "« tap » seul -> refus");
    CHECK(refused("tap 1 2 3"), "COUNTER-CASE: three numbers -> refused, not \u00ab the first two \u00bb");
    CHECK(refused("tap 1 2 x"), "an extra word after two numbers -> refused");

    /* A refused coordinate must leave nothing in the struct: a caller that
     * forgot to look at the return value must not find a usable half-tap. */
    {
        devcmd_t sale;
        memset(&sale, 0x5A, sizeof(sale));
        CHECK(!parse("tap 12 -5", &sale) && sale.a == 0 && sale.b == 0
                && sale.kind == DEVCMD_UNKNOWN,
                "COUNTER-CASE: refused on the 2nd number -> NO leftover of the 1st in the struct");
    }
}

/* ── No default behaviour at all ──────────────────────────────────────────── */

static void no_default_behaviour(void)
{
    devcmd_t c;

    /* Absurd arguments: the caller is our own code, but a regression elsewhere
     * must not cost a null dereference in the log thread - that is, a crash of
     * the application being observed. */
    CHECK(!devcmd_parse(NULL, 10, &c), "a NULL line -> refused");
    CHECK(!devcmd_parse("shot", 4, NULL), "a NULL output -> refused, nothing written");
    CHECK(!devcmd_parse(NULL, 0, NULL), "both NULL -> refused");

    /* COUNTER-CASE - THE ABSURD `n`.
     * `line + n` with a gigantic n is a POINTER overflow, hence undefined
     * behaviour before even the first read. The bound is checked before the
     * cursor is built. */
    CHECK(!devcmd_parse("shot", (size_t)-1, &c),
            "COUNTER-CASE: n = SIZE_MAX -> refused BEFORE any address computation");

    /* The return value and the kind always say the same thing: that is what
     * allows the caller to test only one of the two. */
    {
        static const char *const lines[] = {
            "shot", "state", "quit", "btn a", "nav down", "tap 1 2",
            "", "  ", "\r\n", "shoot", "btn zzz", "tap -1 0", "quitter", "nav up"
        };
        size_t i; int coherent = 1;
        for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
            devcmd_t d;
            const bool r = parse(lines[i], &d);
            if (r != (d.kind != DEVCMD_UNKNOWN)) coherent = 0;
        }
        CHECK(coherent, "the return value and the kind always agree");
    }
}

/* -- Safe rendering for the [devlink] lines -------------------------------- */

static void drawn_on(void)
{
    devcmd_t c;
    char     texte[64];

    CHECK(parse("SHOT\r\n", &c) && devcmd_render(&c, texte, sizeof(texte))
            && strcmp(texte, "shot") == 0, "the canonical rendering of a shot");
    CHECK(parse("btn  ZL", &c) && devcmd_render(&c, texte, sizeof(texte))
            && strcmp(texte, "btn zl") == 0, "the canonical rendering of a btn");
    CHECK(parse("nav RIGHT", &c) && devcmd_render(&c, texte, sizeof(texte))
            && strcmp(texte, "nav right") == 0, "the canonical rendering of a nav");
    CHECK(parse("tap  0012   0034 ", &c) && devcmd_render(&c, texte, sizeof(texte))
            && strcmp(texte, "tap 12 34") == 0, "the canonical rendering of a tap");

    /* COUNTER-CASE - THE TRUNCATED RENDERING.
     * "tap 1280 720" cut short becomes "tap 1280 7": a perfectly plausible line
     * in the log, which would send someone looking for a coordinate bug where
     * there is none. We return false and an EMPTY string rather than half an
     * echo. */
    {
        char petit[8];
        CHECK(parse("tap 1280 720", &c) && !devcmd_render(&c, petit, sizeof(petit))
                && petit[0] == '\0',
                "COUNTER-CASE: a too-short buffer -> false and an empty string, never half a rendering");
        CHECK(!devcmd_render(&c, petit, 0), "zero size -> false, without writing");
        CHECK(!devcmd_render(NULL, texte, sizeof(texte)) && texte[0] == '\0',
                "commande NULL -> false");
    }
    {
        devcmd_t inconnue;
        devcmd_clear(&inconnue);
        CHECK(!devcmd_render(&inconnue, texte, sizeof(texte)) && texte[0] == '\0',
                "an UNKNOWN command is not rendered (nothing to echo)");
    }
    /* A hand-built struct whose name is not terminated: the rendering must not
     * read past the field's 16 bytes. */
    {
        devcmd_t bricolee;
        devcmd_clear(&bricolee);
        bricolee.kind = DEVCMD_BTN;
        memset(bricolee.name, 'z', sizeof(bricolee.name));
        CHECK(devcmd_render(&bricolee, texte, sizeof(texte))
                && strncmp(texte, "btn ", 4) == 0
                && strlen(texte) == 4 + DEVCMD_NAME_MAX - 1,
                "an unterminated name: the rendering stops at the field, it does not go past it");
    }
    CHECK(strcmp(devcmd_kind_name(DEVCMD_QUIT), "quit") == 0, "kind name");
    CHECK(strcmp(devcmd_kind_name(DEVCMD_UNKNOWN), "inconnue") == 0, "the unknown kind's name");

    /* COUNTER-CASE - LINE INJECTION INTO THE LOG.
     * The other end tells a reply from a trace ONLY by the line prefix. Copying a
     * refused command back verbatim to write "err <command>" therefore injects
     * everything it contains, newlines included: the line below would fabricate
     * an end-of-capture nobody sent, and the tool on the other end would glue
     * together a truncated image without a word. */
    {
        char word[32];
        CHECK(devcmd_first_word_is("bidon\n[devlink] shot-end", 24, word, sizeof(word))
                && strcmp(word, "bidon") == 0,
                "COUNTER-CASE: the first word stops at the end of line - no injection");
        CHECK(devcmd_first_word_is("BiDon", 5, word, sizeof(word))
                && strcmp(word, "bidon") == 0, "the safe word is folded to lowercase");
        CHECK(devcmd_first_word_is("a\x01\x02z", 4, word, sizeof(word))
                && strcmp(word, "a??z") == 0, "the control bytes become '?'");
        CHECK(devcmd_first_word_is("\xff\xfe", 2, word, sizeof(word))
                && strcmp(word, "??") == 0, "and so do the high bytes");
        CHECK(!devcmd_first_word_is("   ", 3, word, sizeof(word)) && word[0] == '\0',
                "no first word -> false and an empty string");
        CHECK(!devcmd_first_word_is("", 0, word, sizeof(word)), "an empty line -> false");
        CHECK(!devcmd_first_word_is(NULL, 4, word, sizeof(word)), "a NULL line -> false");
        CHECK(!devcmd_first_word_is("shot", 4, word, 0), "zero size -> false");
        {
            char court[4];
            CHECK(!devcmd_first_word_is("state", 5, court, sizeof(court))
                    && court[0] == '\0',
                    "a word longer than the buffer -> false and an empty string, no truncation");
        }
    }
}

/* -- Arbitrary noise: the property, not the cases ------------------------- */

static void arbitrary_noise(void)
{
    /* One cannot enumerate what a socket may receive. So we check a PROPERTY
     * over thousands of lines: whatever happens, either the line is refused and
     * the struct is zeroed, or it is accepted and the struct is usable WITHOUT
     * any further check in the caller (a known kind, an index within its enum,
     * bounded coordinates, a terminated `name`).
     *
     * Two sources mixed, because they do not look for the same fault:
     *   - noise drawn from an arbitrary alphabet, for the plain overrun;
     *   - VALID commands then mutilated (a byte replaced, added, removed),
     *     because that is where the real parsing faults hide: "almost valid" is
     *     what a too-permissive parser accepts.
     * Every draw is parsed from a block of the EXACT size and with no null
     * terminator: under ASan, every iteration of the loop is also an overflow
     * test.
     *
     * The draw is deterministic (a linear congruential generator with a fixed
     * seed): a failure here is reproducible, otherwise it would be unusable. */
    static const char alphabet[] =
        "tapbnvshoquiegdmlrzxy0123456789 \t\r\n-+.\xff\x01";
    static const char *const modeles[] = {
        "shot", "state", "quit", "btn a", "btn zr", "btn minus",
        "nav up", "nav right", "tap 0 0", "tap 1280 720", "tap 16384 16384"
    };
    const size_t alphabet_size = sizeof(alphabet) - 1;
    const size_t n_templates = sizeof(modeles) / sizeof(modeles[0]);
    unsigned long graine = 20260827UL;
    int    invariants_ok = 1, accepted = 0;
    size_t attempt;

    for (attempt = 0; attempt < 20000; attempt++) {
        char     source[64];
        char    *bloc;
        size_t   n, i;
        devcmd_t c;

        graine = graine * 1103515245UL + 12345UL;
        if (((graine >> 16) & 1u) == 0u) {
            /* Pure noise: 1 to 24 bytes drawn from the alphabet. */
            n = (size_t)((graine >> 17) % 24u) + 1u;
            for (i = 0; i < n; i++) {
                graine = graine * 1103515245UL + 12345UL;
                source[i] = alphabet[(graine >> 16) % alphabet_size];
            }
        } else {
            /* A valid template, then 0 to 3 mutations. Zero mutations therefore does
             * happen for real: that is what guarantees the loop does not simply
             * refuse everything. */
            const char *m = modeles[(graine >> 17) % n_templates];
            size_t mutations;
            n = strlen(m);
            memcpy(source, m, n);
            graine = graine * 1103515245UL + 12345UL;
            mutations = (graine >> 16) % 4u;
            for (i = 0; i < mutations; i++) {
                size_t pos;
                graine = graine * 1103515245UL + 12345UL;
                pos = (size_t)((graine >> 16) % (n + 1u));
                graine = graine * 1103515245UL + 12345UL;
                switch ((graine >> 16) % 3u) {
                    case 0:                                   /* remplacer */
                        if (n > 0) {
                            graine = graine * 1103515245UL + 12345UL;
                            source[pos % n] = alphabet[(graine >> 16) % alphabet_size];
                        }
                        break;
                    case 1:                                   /* inserer */
                        if (n + 1 < sizeof(source)) {
                            memmove(source + pos + 1, source + pos, n - pos);
                            graine = graine * 1103515245UL + 12345UL;
                            source[pos] = alphabet[(graine >> 16) % alphabet_size];
                            n++;
                        }
                        break;
                    default:                                  /* retirer */
                        if (n > 0) {
                            const size_t q = pos % n;
                            memmove(source + q, source + q + 1, n - q - 1);
                            n--;
                        }
                        break;
                }
            }
            if (n == 0) n = 1, source[0] = ' ';
        }

        bloc = (char *)malloc(n);
        if (!bloc) { printf("  malloc a echoue\n"); exit(2); }
        memcpy(bloc, source, n);

        memset(&c, 0xC3, sizeof(c));
        if (devcmd_parse(bloc, n, &c)) {
            accepted++;
            if (c.kind <= DEVCMD_UNKNOWN || c.kind > DEVCMD_RELEASE) invariants_ok = 0;
            if (c.name[DEVCMD_NAME_MAX - 1] != '\0') invariants_ok = 0;
            if (c.path[DEVCMD_PATH_MAX - 1] != '\0') invariants_ok = 0;
            if (c.env_key[DEVCMD_ENV_KEY_MAX - 1] != '\0') invariants_ok = 0;
            if (c.env_val[DEVCMD_ENV_VAL_MAX - 1] != '\0') invariants_ok = 0;
            if (c.env_key[0] && !devcmd_env_key_ok(c.env_key, strlen(c.env_key)))
                invariants_ok = 0;
            /* An accepted path is one `devcmd_path_ok` would accept again -
             * the parser may never hand out something it would itself refuse. */
            if (c.path[0] && !devcmd_path_ok(c.path, strlen(c.path)))
                invariants_ok = 0;
            if (c.kind == DEVCMD_BTN && (c.a < 0 || c.a >= (int)DEVCMD_BTN_COUNT))
                invariants_ok = 0;
            if (c.kind == DEVCMD_NAV && (c.a < 0 || c.a >= (int)DEVCMD_NAV_COUNT))
                invariants_ok = 0;
            if (c.kind == DEVCMD_TAP
                && (c.a < 0 || c.a > DEVCMD_COORD_MAX || c.b < 0 || c.b > DEVCMD_COORD_MAX))
                invariants_ok = 0;
            if (c.kind != DEVCMD_BTN && c.kind != DEVCMD_NAV
                && c.kind != DEVCMD_HOLD && c.kind != DEVCMD_STICK
                && c.name[0] != '\0')
                invariants_ok = 0;
            if (c.kind == DEVCMD_HOLD && (c.a < 0 || c.a >= (int)DEVCMD_BTN_COUNT
                                          || c.b < 0 || c.b > DEVCMD_MS_MAX))
                invariants_ok = 0;
            if (c.kind == DEVCMD_STICK && (c.a < -100 || c.a > 100
                                           || c.b < -100 || c.b > 100))
                invariants_ok = 0;
            if (c.kind == DEVCMD_SWIPE
                && (c.a < 0 || c.a > DEVCMD_COORD_MAX || c.b < 0 || c.b > DEVCMD_COORD_MAX
                    || c.c < 0 || c.c > DEVCMD_COORD_MAX || c.d < 0 || c.d > DEVCMD_COORD_MAX
                    || c.e < 0 || c.e > DEVCMD_MS_MAX))
                invariants_ok = 0;
            if (c.kind != DEVCMD_RELAUNCH && c.path[0] != '\0') invariants_ok = 0;
            if (c.kind != DEVCMD_ENV && c.env_key[0] != '\0') invariants_ok = 0;
        } else if (c.kind != DEVCMD_UNKNOWN || c.a != 0 || c.b != 0 || c.name[0] != '\0'
                   || c.path[0] != '\0' || c.env_key[0] != '\0'
                   || c.env_val[0] != '\0') {
            invariants_ok = 0;   /* a refusal must have zeroed the struct */
        }
        free(bloc);
    }
    CHECK(invariants_ok,
            "20000 arbitrary or mutilated lines: the invariants hold, no overrun");

    /* Without this, the previous check would be satisfied by a parser that
     * refuses EVERYTHING - the worst kind of false green. */
    CHECK(accepted > 100,
            "the draw does reach accepted commands (otherwise it tests nothing)");
}

/* --- relaunch: the only command that names something to EXECUTE ------------
 *
 * Everything else in this vocabulary presses a button or reads the screen. This
 * one hands a file name to `envSetNextLoad`, and hbloader runs what it points
 * at - so the refusals below are the feature, not the edge cases. */
static void relaunch_paths(void)
{
    devcmd_t c;

    CHECK(parse("relaunch", &c) && c.kind == DEVCMD_RELAUNCH && c.path[0] == '\0',
            "relaunch bare: accepted, empty path (= reload this NRO)");
    CHECK(parse("relaunch /switch/halyard.b.nro", &c)
            && c.kind == DEVCMD_RELAUNCH
            && strcmp(c.path, "/switch/halyard.b.nro") == 0,
            "relaunch <path>: the path arrives verbatim");

    /* THE CASE IS KEPT. Folding it, as every other token is folded, would refuse
     * a file that exists on the card with an error naming a path nobody typed. */
    CHECK(parse("relaunch /switch/Halyard.NRO", &c) == 0,
            "an uppercase extension is refused (the suffix test is exact)");
    CHECK(parse("relaunch /switch/Halyard.nro", &c)
            && strcmp(c.path, "/switch/Halyard.nro") == 0,
            "COUNTER-CASE: the case of the NAME is preserved, not folded");

    /* The allow-list, refusal by refusal. */
    CHECK(parse("relaunch /atmosphere/x.nro", &c) == 0, "outside /switch/: refused");
    CHECK(parse("relaunch /switch/x.bin", &c) == 0, "not a .nro: refused");
    CHECK(parse("relaunch switch/x.nro", &c) == 0, "relative path: refused");
    CHECK(parse("relaunch /switch/../atmosphere/x.nro", &c) == 0,
            "COUNTER-CASE: `..` escapes /switch/ - refused");
    CHECK(parse("relaunch /switch/a..b.nro", &c) == 0,
            "`..` refused WHEREVER it sits, not only after a slash");
    CHECK(parse("relaunch /switch//x.nro", &c) == 0, "empty segment `//`: refused");
    CHECK(parse("relaunch /switch/mon fichier.nro", &c) == 0,
            "a space splits the token: the second word makes the line invalid");
    CHECK(parse("relaunch /switch/x;reboot.nro", &c) == 0,
            "a character outside the allow-list: refused");
    CHECK(parse("relaunch /switch/\x80.nro", &c) == 0,
            "a byte above 0x7F: refused (and the answer does not depend on"
            " whether `char` is signed)");
    CHECK(parse("relaunch /switch/.nro", &c) == 0,
            "too short to be a real name: refused");
    CHECK(parse("relaunch /switch/x.nro extra", &c) == 0,
            "a trailing word is refused, never ignored");

    /* A path that fills the field exactly, and one byte too long. The second
     * must be REFUSED rather than truncated - a truncated path names a
     * different file, which is the whole reason this repo refuses instead of
     * cutting. */
    {
        char juste[DEVCMD_LINE_MAX];
        char trop[DEVCMD_LINE_MAX];
        size_t i, milieu;

        /* "relaunch " + "/switch/" + <fill> + ".nro" == DEVCMD_PATH_MAX - 1 */
        milieu = (size_t)DEVCMD_PATH_MAX - 1 - 8 - 4;
        strcpy(juste, "relaunch /switch/");
        for (i = 0; i < milieu; i++) strcat(juste, "a");
        strcat(juste, ".nro");
        CHECK(parse(juste, &c) && c.kind == DEVCMD_RELAUNCH
                && strlen(c.path) == (size_t)DEVCMD_PATH_MAX - 1,
                "a path filling the field exactly is accepted");

        strcpy(trop, "relaunch /switch/");
        for (i = 0; i < milieu + 1; i++) strcat(trop, "a");
        strcat(trop, ".nro");
        CHECK(parse(trop, &c) == 0 && c.path[0] == '\0',
                "COUNTER-CASE: one byte too long is REFUSED, not truncated");
    }

    /* What goes back to the dev machine is rebuilt from the struct. */
    {
        char rendu[DEVCMD_PATH_MAX + 32];
        CHECK(parse("relaunch /switch/halyard.b.nro", &c)
                && devcmd_render(&c, rendu, sizeof(rendu))
                && strcmp(rendu, "relaunch /switch/halyard.b.nro") == 0,
                "the reply names the path that was UNDERSTOOD");
        CHECK(parse("relaunch", &c) && devcmd_render(&c, rendu, sizeof(rendu))
                && strcmp(rendu, "relaunch") == 0,
                "bare, the reply carries no path");
        CHECK(parse("relaunch /switch/x.nro", &c)
                && devcmd_render(&c, rendu, 12) == 0 && rendu[0] == '\0',
                "too small a buffer gives an empty string, never a cut path");
    }

    CHECK(strcmp(devcmd_kind_name(DEVCMD_RELAUNCH), "relaunch") == 0,
            "the kind has its name (the log quotes it)");
}

/* --- version: which build is running --------------------------------------
 *
 * COUNTER-CASE FROM A REAL SESSION (2026-09-12). The channel could not say what
 * it was talking to: the build banner went out by `fprintf(stderr, ...)`, which
 * reaches neither the journal nor its mirror. A whole iteration was spent
 * supposing which build was on the console - the answer was "one that predates
 * the command being sent to it", and a single reply would have said so. */
static void version_verb(void)
{
    devcmd_t c;

    CHECK(parse("version", &c) && c.kind == DEVCMD_VERSION, "version");
    CHECK(parse("VERSION", &c) && c.kind == DEVCMD_VERSION, "VERSION (case ignored)");
    CHECK(parse("version", &c) && c.a == 0 && c.b == 0
            && c.name[0] == '\0' && c.path[0] == '\0',
            "version leaves no argument behind");
    CHECK(parse("version 2", &c) == 0, "a trailing word is refused, never ignored");
    CHECK(strcmp(devcmd_kind_name(DEVCMD_VERSION), "version") == 0, "the kind has its name");
}

/* --- env: the only command that WRITES an experiment setting ---------------
 *
 * The ~120 SHADOW_* toggles are this repo's experiment mechanism; until now,
 * changing one on the console meant dropping an env.txt over FTP. What the verb
 * must guarantee: never write a line the READER (`shadow_load_env_file`) would
 * ignore in silence - that would be an experiment we believe we ran and did
 * not. */
static void env_verb(void)
{
    devcmd_t c;

    CHECK(parse("env", &c) && c.kind == DEVCMD_ENV && c.a == 0
            && c.env_key[0] == '\0' && c.env_val[0] == '\0',
            "env bare: list, no key and no value");
    CHECK(parse("env SHADOW_FPS=60", &c) && c.kind == DEVCMD_ENV && c.a == 1
            && strcmp(c.env_key, "SHADOW_FPS") == 0
            && strcmp(c.env_val, "60") == 0,
            "env KEY=VALUE: the two halves arrive separated");
    CHECK(parse("env SHADOW_FPS=", &c) && c.kind == DEVCMD_ENV && c.a == 1
            && strcmp(c.env_key, "SHADOW_FPS") == 0 && c.env_val[0] == '\0',
            "env KEY= : an empty value is valid and means REMOVE");

    /* COUNTER-CASE - THE PREFIX IS NOT DECORATION. `shadow_load_env_file`
     * refuses every key that is not SHADOW_*, so writing one would produce a
     * line the reader drops without a word. */
    CHECK(parse("env PATH=/tmp", &c) == 0,
            "COUNTER-CASE: a key without the SHADOW_ prefix is refused"
            " (the reader would ignore it in silence)");
    CHECK(parse("env SHADOW_=1", &c) == 0, "the prefix alone is not a key");
    CHECK(parse("env shadow_fps=60", &c) == 0,
            "lowercase refused, not folded: it is a DIFFERENT variable,"
            " and one nothing reads");
    CHECK(parse("env SHADOW_FPS", &c) == 0,
            "without `=` it reads like a query: refused rather than guessed");
    CHECK(parse("env SHADOW-FPS=1", &c) == 0, "a dash is not allowed in a key");
    CHECK(parse("env SHADOW_A=b=c", &c) == 0,
            "a second `=` is refused: one separator per line");
    CHECK(parse("env SHADOW_A=a b", &c) == 0,
            "a space splits the token: the trailing word makes the line invalid");
    CHECK(parse("env SHADOW_A=\x80", &c) == 0, "a byte above 0x7F is refused");

    /* Bounds, refused rather than truncated - a truncated key names another
     * variable, a truncated value runs another experiment. */
    {
        char ligne[DEVCMD_LINE_MAX];
        size_t i;
        strcpy(ligne, "env SHADOW_");
        for (i = 0; i < (size_t)DEVCMD_ENV_KEY_MAX; i++) strcat(ligne, "A");
        strcat(ligne, "=1");
        CHECK(parse(ligne, &c) == 0 && c.env_key[0] == '\0',
                "COUNTER-CASE: an over-long key is REFUSED, not cut");

        strcpy(ligne, "env SHADOW_A=");
        for (i = 0; i < (size_t)DEVCMD_ENV_VAL_MAX; i++) strcat(ligne, "9");
        CHECK(parse(ligne, &c) == 0 && c.env_val[0] == '\0',
                "COUNTER-CASE: an over-long value is REFUSED, not cut");
    }

    {
        char rendu[256];
        CHECK(parse("env SHADOW_FPS=60", &c)
                && devcmd_render(&c, rendu, sizeof(rendu))
                && strcmp(rendu, "env SHADOW_FPS=60") == 0,
                "the reply names the toggle that was UNDERSTOOD");
        CHECK(parse("env", &c) && devcmd_render(&c, rendu, sizeof(rendu))
                && strcmp(rendu, "env") == 0, "bare, the reply carries no toggle");
    }
    CHECK(strcmp(devcmd_kind_name(DEVCMD_ENV), "env") == 0, "the kind has its name");
}

/* --- hold / stick / swipe / release: the injection engine ------------------
 *
 * THE FOUNDING COUNTER-CASE: `btn`, `nav` and `tap` were documented, parsed and
 * sent from 2026-08-27 onwards, and NOTHING served them on the application side
 * (checked by grep and against the history). The channel could observe, not
 * act. These verbs round out the vocabulary - but the fact that matters is that
 * the three older ones finally do something. */
static void injection_verbs(void)
{
    devcmd_t c;

    CHECK(parse("hold a 3000", &c) && c.kind == DEVCMD_HOLD
            && c.a == DEVCMD_BTN_A && c.b == 3000 && strcmp(c.name, "a") == 0,
            "hold <button> <ms>");
    CHECK(parse("hold a", &c) == 0, "hold with no duration: refused, never guessed");
    CHECK(parse("hold start 100", &c) == 0, "a button that does not exist is refused");
    CHECK(parse("hold a -5", &c) == 0, "a negative duration does not exist");
    CHECK(parse("hold a 999999", &c) == 0,
            "an absurd duration is REFUSED here (the engine would cap it, but"
            " saying so beats correcting it in silence)");

    CHECK(parse("release", &c) && c.kind == DEVCMD_RELEASE, "release");

    CHECK(parse("stick left 100 -100", &c) && c.kind == DEVCMD_STICK
            && c.a == 100 && c.b == -100 && strcmp(c.name, "left") == 0,
            "stick <side> <x> <y>, SIGNED values");
    CHECK(parse("stick right 0 0 500", &c) && c.kind == DEVCMD_STICK && c.e == 500,
            "the duration is optional");
    CHECK(parse("stick middle 0 0", &c) == 0, "a side that does not exist is refused");
    CHECK(parse("stick left 101 0", &c) == 0, "beyond 100: refused");
    CHECK(parse("stick left -101 0", &c) == 0, "below -100: refused");
    CHECK(parse("stick left - 0", &c) == 0, "a lone minus is not a number");
    CHECK(parse("stick left 0", &c) == 0, "both axes are required");

    CHECK(parse("swipe 100 200 300 400", &c) && c.kind == DEVCMD_SWIPE
            && c.a == 100 && c.b == 200 && c.c == 300 && c.d == 400 && c.e == 0,
            "swipe <x1> <y1> <x2> <y2>");
    CHECK(parse("swipe 1 2 3 4 250", &c) && c.kind == DEVCMD_SWIPE && c.e == 250,
            "swipe with a duration");
    CHECK(parse("swipe 1 2 3", &c) == 0, "three numbers do not make a swipe");
    CHECK(parse("swipe 1 2 3 4 5 6", &c) == 0, "one word too many is refused");
    CHECK(parse("swipe -1 2 3 4", &c) == 0, "a negative coordinate is refused");
    CHECK(parse("swipe 99999 2 3 4", &c) == 0, "off screen: refused");

    /* The reply restates what was UNDERSTOOD, never the line received. */
    {
        char r[128];
        CHECK(parse("hold zr 250", &c) && devcmd_render(&c, r, sizeof r)
                && strcmp(r, "hold zr 250") == 0, "rendering a hold");
        CHECK(parse("stick right -50 25", &c) && devcmd_render(&c, r, sizeof r)
                && strcmp(r, "stick right -50 25") == 0, "rendering a stick");
        CHECK(parse("swipe 10 20 30 40", &c) && devcmd_render(&c, r, sizeof r)
                && strcmp(r, "swipe 10 20 30 40") == 0, "rendering a swipe");
        CHECK(parse("release", &c) && devcmd_render(&c, r, sizeof r)
                && strcmp(r, "release") == 0, "rendering a release");
    }
}

int main(void)
{
    printf("== [devlink] channel: parsing the commands received from the network ==\n");
    simple_verbs();
    case_and_spacing();
    empty_and_noise();
    without_a_null_terminator();
    buttons();
    navigation();
    tap_and_overflow();
    no_default_behaviour();
    drawn_on();
    relaunch_paths();
    version_verb();
    env_verb();
    injection_verbs();
    arbitrary_noise();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
