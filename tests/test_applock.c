/* test_applock.c - the application lock's pure core (core/services/applock.c).
 *
 * A lock has two ways to fail and they are not symmetric. Letting the wrong
 * person in is the one everybody thinks of; refusing the right person is the
 * one that actually happens, and it is unrecoverable in a way the first is not
 * - the owner has no card to wipe, they just lose the application.
 *
 * So the counter-cases here run in both directions, and each names the exact
 * symptom it prevents:
 *
 *   - the throttle only ever LENGTHENS. A console whose clock is moved back
 *     between two wrong guesses must not shorten a deadline already set, and
 *     the failure counter must not wrap at UINT32_MAX and hand out four fresh
 *     attempts.
 *
 *   - a half-written record means NO LOCK, never a lock nobody can open. A
 *     `methods` bit whose hash is all-zero demands a secret that cannot exist;
 *     honouring it would brick the application with no way back.
 *
 *   - the pattern's canonical form is the Android rule, and it is pinned to the
 *     byte. Dragging 0 to 2 crosses node 1, and if that insertion is ever lost
 *     the same finger movement stops matching the stored pattern - a lock that
 *     "sometimes" refuses the right gesture, which is the worst failure of all
 *     because the user blames themselves.
 *
 *   - the encoding that gets hashed is pinned too. Change it and every existing
 *     pattern silently becomes wrong, with no error anywhere - the user simply
 *     cannot get in any more.
 */
#include "../core/services/applock.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                   \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* ── L'etranglement ───────────────────────────────────────────────────────── */

static void the_throttle(void)
{
    applock_record_t r;
    int64_t left;

    /* Four free attempts, then a real wait. Both halves matter: no wait at all
     * makes a four-digit PIN free to enumerate, and a wait from the first slip
     * makes the lock hateful enough to be turned off. */
    CHECK(applock_penalty_seconds(0) == 0, "[etranglement] no penalty at zero failures");
    CHECK(applock_penalty_seconds(4) == 0, "[etranglement] the fourth attempt is still free");
    CHECK(applock_penalty_seconds(5) > 0,  "[etranglement] the fifth costs a wait");
    CHECK(applock_penalty_seconds(6) > applock_penalty_seconds(5),
          "[etranglement] the wait grows");
    /* Capped: an uncapped schedule protects nothing further and turns a
     * forgotten PIN into a dead application. */
    CHECK(applock_penalty_seconds(100) == applock_penalty_seconds(9),
          "[etranglement] the wait is capped");
    CHECK(applock_penalty_seconds(0xFFFFFFFFu) == applock_penalty_seconds(9),
          "[etranglement] capped even at the counter's maximum");
    {
        uint32_t f;
        for (f = 1; f < 40; f++)
            CHECK(applock_penalty_seconds(f) >= applock_penalty_seconds(f - 1),
                  "[etranglement] the schedule never decreases");
    }

    applock_record_init(&r);
    CHECK(!applock_locked_out(&r, 1000, NULL), "[etranglement] a fresh record locks nobody out");

    /* Four failures: still no lockout. */
    applock_note_failure(&r, 1000);
    applock_note_failure(&r, 1000);
    applock_note_failure(&r, 1000);
    applock_note_failure(&r, 1000);
    CHECK(r.fails == 4, "[etranglement] the failures are counted");
    CHECK(!applock_locked_out(&r, 1000, NULL), "[etranglement] four failures do not lock out");

    applock_note_failure(&r, 1000);
    CHECK(applock_locked_out(&r, 1000, &left), "[etranglement] the fifth locks out");
    CHECK(left == 30, "[etranglement] the remaining time is the penalty");
    CHECK(!applock_locked_out(&r, 1030, NULL), "[etranglement] the lockout expires");

    /* THE COUNTER-CASE. The console's clock is user-settable. Moving it BACK
     * between two wrong guesses must not shorten a deadline already set -
     * otherwise the throttle, which is the only thing making a short PIN mean
     * anything, is defeated with the date screen. */
    applock_record_init(&r);
    r.fails = 8;
    applock_note_failure(&r, 100000);          /* deadline at 100000 + 300 */
    {
        const int64_t deadline = r.lock_until;
        applock_note_failure(&r, 1);           /* the clock jumped back */
        CHECK(r.lock_until >= deadline,
              "[horloge] a clock moved backwards never shortens the lockout");
    }

    /* The counter saturates instead of wrapping: at UINT32_MAX a wrap would put
     * `fails` back to 0 and hand out four free attempts. */
    applock_record_init(&r);
    r.fails = 0xFFFFFFFFu;
    applock_note_failure(&r, 1000);
    CHECK(r.fails == 0xFFFFFFFFu, "[etranglement] the counter saturates, it does not wrap");
    CHECK(applock_locked_out(&r, 1000, NULL), "[etranglement] and it still locks out");

    /* THE COUNTER-CASE for the deadline. The penalty is capped, but the
     * DEADLINE it produces is an absolute instant and the console's clock is
     * set by hand. Set the clock a year ahead, guess wrong once, set it back:
     * without the clamp on READ, the record holds a deadline a year away and
     * the "never shortens" rule - which is right - would keep the OWNER out of
     * their own console until then. */
    applock_record_init(&r);
    r.lock_until = 100000000;         /* as if written under a clock far ahead */
    CHECK(applock_locked_out(&r, 1000, &left), "[horloge] a far deadline still locks out");
    CHECK(left <= (int64_t)applock_penalty_seconds(0xFFFFFFFFu),
          "[horloge] but never for longer than the longest penalty");
    {
        /* And nothing is written back, so a clock that moves again still cannot
         * shorten what is stored - both properties at once. */
        const int64_t stored = r.lock_until;
        applock_locked_out(&r, 1000, &left);
        CHECK(r.lock_until == stored, "[horloge] reading does not rewrite the deadline");
    }

    /* Never "0 s remaining" on a screen that still refuses input. */
    applock_record_init(&r);
    r.lock_until = 1001;
    CHECK(applock_locked_out(&r, 1000, &left) && left >= 1,
          "[etranglement] the remaining time is never displayed as zero");

    /* Success clears everything - otherwise the next slip starts from a
     * counter the user has already paid off. */
    applock_note_success(&r);
    CHECK(r.fails == 0 && r.lock_until == 0, "[etranglement] a success clears the counter");
    CHECK(!applock_locked_out(&r, 0, NULL), "[etranglement] and the lockout with it");
}

/* ── Le dossier a moitie ecrit ────────────────────────────────────────────── */

static void a_half_written_record(void)
{
    applock_record_t r;

    applock_record_init(&r);
    CHECK(!applock_is_armed(&r), "[a-moitie] a blank record arms nothing");

    /* THE COUNTER-CASE: the bit says "a PIN is required" and no hash exists to
     * satisfy it. Honouring that would demand a secret nobody can produce, and
     * the only way out would be deleting the file - which is the same as having
     * no lock, minus the owner's ability to find that out. */
    r.methods = APPLOCK_PIN;
    CHECK(!applock_is_armed(&r), "[a-moitie] a method with no secret does not arm the lock");
    CHECK(applock_usable_methods(&r) == 0, "[a-moitie] and it is not reported as usable");

    r.pin.hash[7] = 0x01;
    CHECK(applock_is_armed(&r), "[a-moitie] a method with a secret does arm it");
    CHECK(applock_usable_methods(&r) == APPLOCK_PIN, "[a-moitie] and only that one");

    /* One armed method plus one half-written one: the usable one still works
     * and the broken one is simply not offered. */
    r.methods = APPLOCK_PIN | APPLOCK_PATTERN;
    CHECK(applock_usable_methods(&r) == APPLOCK_PIN,
          "[a-moitie] a broken method does not disable the working one");
}

/* ── La forme, regle Android ──────────────────────────────────────────────── */

static void the_pattern(void)
{
    uint8_t out[APPLOCK_PATTERN_MAX];
    char enc[APPLOCK_PATTERN_MAX + 1];
    int n;

    /* The grid, for reading what follows:   0 1 2
     *                                       3 4 5
     *                                       6 7 8   */

    /* THE COUNTER-CASE. Dragging from 0 to 2 passes over 1, and Android counts
     * it. Lose this insertion and the same finger movement stops matching the
     * stored pattern - the lock refuses the right gesture, and the user blames
     * their own memory. */
    { const uint8_t t[] = { 0, 2 };
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 3 && out[0] == 0 && out[1] == 1 && out[2] == 2,
            "[forme] 0->2 crosses 1 and inserts it"); }

    { const uint8_t t[] = { 0, 6 };   /* a column */
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 3 && out[1] == 3, "[forme] 0->6 crosses 3"); }
    { const uint8_t t[] = { 0, 8 };   /* the main diagonal */
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 3 && out[1] == 4, "[forme] 0->8 crosses the centre"); }
    { const uint8_t t[] = { 2, 6 };   /* the other diagonal */
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 3 && out[1] == 4, "[forme] 2->6 crosses the centre too"); }
    { const uint8_t t[] = { 3, 5 };
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 3 && out[1] == 4, "[forme] 3->5 crosses 4"); }
    { const uint8_t t[] = { 1, 7 };
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 3 && out[1] == 4, "[forme] 1->7 crosses 4"); }

    /* === THE RULE, ENUMERATED ===
     *
     * The nine checks above are examples; this is the proof. All 72 ordered
     * pairs are walked and the set that inserts a node is compared against
     * AOSP's, which for a 3x3 grid is exactly sixteen: the six edge midpoints
     * taken both ways, and the four lines through the centre taken both ways.
     *
     * It is written as an enumeration and not as more examples because the
     * failure this guards against is not "one pair is wrong" - it is a rule
     * that is wrong in a whole FAMILY of pairs, which examples chosen by the
     * same person who wrote the rule would miss in exactly the same way. */
    {
        static const struct { int a, b, mid; } AOSP[] = {
            {0,2,1},{2,0,1},{3,5,4},{5,3,4},{6,8,7},{8,6,7},   /* rows */
            {0,6,3},{6,0,3},{1,7,4},{7,1,4},{2,8,5},{8,2,5},   /* columns */
            {0,8,4},{8,0,4},{2,6,4},{6,2,4},                   /* diagonals */
        };
        const int N = (int)(sizeof AOSP / sizeof AOSP[0]);
        int inserted = 0, a, b, k;
        for (a = 0; a < 9; a++) for (b = 0; b < 9; b++) {
            uint8_t t[2], o[APPLOCK_PATTERN_MAX];
            int n, expect_mid = -1;
            if (a == b) continue;
            t[0] = (uint8_t)a; t[1] = (uint8_t)b;
            n = applock_pattern_canonical(t, 2, o, sizeof o);
            for (k = 0; k < N; k++) if (AOSP[k].a == a && AOSP[k].b == b) expect_mid = AOSP[k].mid;
            if (expect_mid >= 0) {
                inserted++;
                CHECK(n == 3 && o[1] == (uint8_t)expect_mid,
                      "[forme] every AOSP pair inserts exactly its midpoint");
            } else {
                CHECK(n == 2, "[forme] and no other pair inserts anything");
            }
        }
        CHECK(inserted == N, "[forme] all sixteen AOSP pairs were reached");
    }

    /* The other direction of the same rule, and the one a naive "insert the
     * midpoint" would get wrong: a knight's move and an adjacent step cross
     * NOTHING. Inserting there would make two different gestures produce the
     * same pattern. */
    { const uint8_t t[] = { 0, 5 };
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 2 && out[0] == 0 && out[1] == 5, "[forme] 0->5 is a knight's move, crosses nothing"); }
    { const uint8_t t[] = { 0, 1 };
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 2, "[forme] two adjacent nodes cross nothing"); }
    { const uint8_t t[] = { 0, 4 };
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 2, "[forme] a diagonal step crosses nothing"); }
    { const uint8_t t[] = { 3, 1 };
      n = applock_pattern_canonical(t, 2, out, sizeof out);
      CHECK(n == 2, "[forme] 3->1 crosses nothing"); }

    /* Android again: a node ALREADY visited is not inserted a second time - the
     * line simply passes through it. 1,0,2 must stay three nodes, because 1 is
     * already down when 0->2 is drawn. */
    { const uint8_t t[] = { 1, 0, 2 };
      n = applock_pattern_canonical(t, 3, out, sizeof out);
      CHECK(n == 3 && out[0] == 1 && out[1] == 0 && out[2] == 2,
            "[forme] a node already visited is not inserted again"); }

    /* A full grid still fits: the insertions must not push it past nine. */
    { const uint8_t t[] = { 0, 1, 2, 5, 8, 7, 6, 3, 4 };
      n = applock_pattern_canonical(t, 9, out, sizeof out);
      CHECK(n == 9, "[forme] a nine-node gesture stays nine nodes"); }

    /* Bounds. A repeat and an out-of-range node are refused rather than
     * silently cleaned up: they can only come from a caller bug, and cleaning
     * them would hide it. */
    /* THE COUNTER-CASE this rule was got wrong on. Drawing 0 to 2 lights node 1
     * by the rule above; sliding back over node 1 is ordinary, and Android
     * ignores it. Refusing the gesture told the user "too short" and cleared a
     * pattern they had drawn correctly. */
    { const uint8_t t[] = { 0, 2, 1 };
      n = applock_pattern_canonical(t, 3, out, sizeof out);
      CHECK(n == 3 && out[0] == 0 && out[1] == 1 && out[2] == 2,
            "[forme] touching a node the RULE inserted is ignored, not refused"); }
    /* Seen from the other side: a literal repeat collapses rather than failing. */
    { const uint8_t t[] = { 0, 1, 0 };
      n = applock_pattern_canonical(t, 3, out, sizeof out);
      CHECK(n == 2 && out[0] == 0 && out[1] == 1,
            "[forme] a repeated node collapses to one"); }
    { const uint8_t t[] = { 0, 9 };
      CHECK(applock_pattern_canonical(t, 2, out, sizeof out) == -1,
            "[bornes] a node outside the grid is refused"); }
    { const uint8_t t[] = { 0, 2 };
      CHECK(applock_pattern_canonical(t, 2, out, 2) == -1,
            "[bornes] no truncated result when the buffer is too small"); }
    CHECK(applock_pattern_canonical(NULL, 2, out, sizeof out) == -1, "[bornes] NULL input");
    { const uint8_t t[] = { 0 };
      CHECK(applock_pattern_canonical(t, 0, out, sizeof out) == -1, "[bornes] an empty gesture"); }

    /* In place: `out` is allowed to be the input buffer, and the earlier
     * version that wrote as it read would have corrupted it. */
    { uint8_t io[APPLOCK_PATTERN_MAX] = { 0, 2 };
      n = applock_pattern_canonical(io, 2, io, sizeof io);
      CHECK(n == 3 && io[0] == 0 && io[1] == 1 && io[2] == 2,
            "[forme] canonicalising in place gives the same answer"); }

    /* Validity is asked of the CANONICAL form, and the caller must canonicalise
     * first. The order matters: three taps 0->2->8 become five nodes and make a
     * perfectly good pattern, so a caller that validated the RAW taps would
     * reject a gesture Android accepts - and the user would be told their
     * pattern is "too short" while drawing across the whole grid. */
    { const uint8_t taps[] = { 0, 2, 8 };
      uint8_t can[APPLOCK_PATTERN_MAX];
      const int m = applock_pattern_canonical(taps, 3, can, sizeof can);
      CHECK(m == 5, "[forme] three taps across the grid canonicalise to five nodes");
      CHECK(!applock_pattern_valid(taps, 3), "[forme] the raw taps look too short...");
      CHECK(applock_pattern_valid(can, (size_t)m), "[forme] ...but the canonical form is valid"); }
    { const uint8_t four[] = { 0, 1, 2, 5 };
      CHECK(applock_pattern_valid(four, 4), "[forme] four nodes is the minimum"); }
    { const uint8_t three[] = { 0, 1, 2 };
      CHECK(!applock_pattern_valid(three, 3), "[forme] three nodes is too few"); }
    { const uint8_t dup[] = { 0, 1, 2, 1 };
      CHECK(!applock_pattern_valid(dup, 4), "[forme] a repeated node is not a valid pattern"); }

    /* THE ENCODING, pinned. Change it and every stored pattern becomes
     * unmatchable, with no error anywhere: the user simply cannot get in. */
    { const uint8_t p[] = { 0, 1, 2, 5 };
      CHECK(applock_pattern_bytes(p, 4, enc, sizeof enc) == 4, "[encodage] length");
      CHECK(strcmp(enc, "0125") == 0, "[encodage] one ASCII digit per node, in order"); }
    { const uint8_t p[] = { 8, 7, 6, 3 };
      applock_pattern_bytes(p, 4, enc, sizeof enc);
      CHECK(strcmp(enc, "8763") == 0, "[encodage] and the order is the gesture's"); }
    { const uint8_t p[] = { 0, 1 };
      CHECK(applock_pattern_bytes(p, 2, enc, 2) == -1, "[encodage] no write past the buffer"); }
}

/* ── La comparaison ───────────────────────────────────────────────────────── */

static void the_comparison(void)
{
    uint8_t a[APPLOCK_HASH_LEN], b[APPLOCK_HASH_LEN];
    size_t i;

    memset(a, 0xA5, sizeof a);
    memcpy(b, a, sizeof b);
    CHECK(applock_equal_ct(a, b, sizeof a), "[comparaison] equal buffers compare equal");

    /* Every single-byte difference is caught, at every position. A comparison
     * that stops early would still pass a test that only differs in the first
     * byte - which is why this walks all of them. */
    for (i = 0; i < sizeof a; i++) {
        b[i] ^= 0x01;
        CHECK(!applock_equal_ct(a, b, sizeof a), "[comparaison] a one-bit difference is caught");
        b[i] ^= 0x01;
    }
    CHECK(applock_equal_ct(a, b, sizeof a), "[comparaison] and the buffer was restored");
    CHECK(!applock_equal_ct(NULL, b, sizeof a), "[comparaison] NULL is never equal");
    CHECK(applock_equal_ct(a, b, 0), "[comparaison] zero bytes compare equal");
}

/* ── L'hexadecimal ────────────────────────────────────────────────────────── */

static void the_hex(void)
{
    uint8_t in[APPLOCK_SALT_LEN], out[APPLOCK_SALT_LEN];
    char hex[2 * APPLOCK_SALT_LEN + 1];
    size_t i;

    for (i = 0; i < sizeof in; i++) in[i] = (uint8_t)(i * 17 + 3);
    applock_to_hex(in, sizeof in, hex);
    CHECK(strlen(hex) == 2 * sizeof in, "[hex] the length is two characters per byte");
    CHECK(applock_from_hex(hex, sizeof in, out), "[hex] it reads back");
    CHECK(memcmp(in, out, sizeof in) == 0, "[hex] and gives the same bytes");

    /* THE COUNTER-CASE: a field one character short must be REFUSED, not
     * decoded into a half-filled buffer whose tail is whatever was there
     * before - that tail would then be compared against a real hash. */
    hex[2 * sizeof in - 1] = 0;
    CHECK(!applock_from_hex(hex, sizeof in, out), "[hex] a truncated field is refused");
    applock_to_hex(in, sizeof in, hex);
    hex[3] = 'z';
    CHECK(!applock_from_hex(hex, sizeof in, out), "[hex] a non-hex character is refused");
    applock_to_hex(in, sizeof in, hex);
    CHECK(!applock_from_hex(hex, sizeof in - 1, out), "[hex] a field too LONG is refused too");
    CHECK(!applock_from_hex(NULL, sizeof in, out), "[hex] NULL");

    /* Upper case is accepted on read: the file can be hand-edited, and
     * refusing "AB" while writing "ab" is a trap with no upside. */
    { char up[] = "00112233445566778899aabbccddeeff";
      uint8_t lo[16], hi[16];
      size_t k;
      CHECK(applock_from_hex(up, 16, lo), "[hex] lower case reads");
      for (k = 0; up[k]; k++) if (up[k] >= 'a' && up[k] <= 'f') up[k] = (char)(up[k] - 32);
      CHECK(applock_from_hex(up, 16, hi), "[hex] upper case reads too");
      CHECK(memcmp(lo, hi, 16) == 0, "[hex] and gives the same bytes"); }
}

/* ── Les saisies ──────────────────────────────────────────────────────────── */

static void the_inputs(void)
{
    CHECK(applock_pin_valid("1234"), "[saisie] four digits is a PIN");
    CHECK(applock_pin_valid("000000"), "[saisie] leading zeroes are digits like any other");
    CHECK(!applock_pin_valid("123"), "[saisie] three digits is too short");
    CHECK(!applock_pin_valid("1234567890123"), "[saisie] thirteen digits is too long");
    CHECK(!applock_pin_valid("12a4"), "[saisie] a letter is not a digit");
    CHECK(!applock_pin_valid(""), "[saisie] the empty PIN");
    CHECK(!applock_pin_valid(NULL), "[saisie] NULL");

    CHECK(applock_password_valid("hunter2"), "[saisie] an ordinary password");
    CHECK(applock_password_valid("a b!~"), "[saisie] spaces and punctuation are printable");
    CHECK(!applock_password_valid("abc"), "[saisie] three characters is too short");
    CHECK(!applock_password_valid("ab\tcd"), "[saisie] a control character is refused");
    /* THE COUNTER-CASE, and it was a real defect: this used to be REFUSED, and
     * refused with the message "too short". A French speaker typing their own
     * name was told something both false and impossible to act on. The console's
     * keyboard produces accented letters repeatably, so they are a perfectly
     * good password. */
    CHECK(applock_password_valid("caf\xc3\xa9 xx"), "[saisie] an accented password is accepted");
    CHECK(applock_password_valid("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"),
          "[saisie] and a non-Latin one");
    /* What IS refused: bytes no keyboard emits as a character. */
    CHECK(!applock_password_valid("ab\x7f" "cd"), "[saisie] DEL is refused");
    { char too_long[APPLOCK_PASSWORD_MAX + 2];
      memset(too_long, 'x', sizeof too_long - 1);
      too_long[sizeof too_long - 1] = 0;
      CHECK(!applock_password_valid(too_long), "[saisie] one character past the maximum");
      too_long[APPLOCK_PASSWORD_MAX] = 0;
      CHECK(applock_password_valid(too_long), "[saisie] exactly the maximum is accepted"); }
}

/* ── Lecture et ecriture ──────────────────────────────────────────────────── */

static void the_record(void)
{
    applock_record_t r, back;
    char buf[APPLOCK_TEXT_MAX];
    size_t i;
    int n;

    applock_record_init(&r);
    r.methods = APPLOCK_PIN | APPLOCK_PATTERN;
    r.iterations = 12345;
    r.fails = 3;
    r.lock_until = 1767225600;
    for (i = 0; i < APPLOCK_SALT_LEN; i++) { r.pin.salt[i] = (uint8_t)i; r.pattern.salt[i] = (uint8_t)(255 - i); }
    for (i = 0; i < APPLOCK_HASH_LEN; i++) { r.pin.hash[i] = (uint8_t)(i * 3 + 1); r.pattern.hash[i] = (uint8_t)(i ^ 0x5A); }

    n = applock_serialize(&r, buf, sizeof buf);
    CHECK(n > 0, "[dossier] it serialises");
    CHECK((size_t)n < APPLOCK_TEXT_MAX, "[dossier] APPLOCK_TEXT_MAX is big enough");
    CHECK(applock_parse(buf, (size_t)n, &back), "[dossier] and parses back");
    CHECK(back.methods == r.methods, "[dossier] the methods survive");
    CHECK(back.iterations == r.iterations, "[dossier] the iteration count survives");
    CHECK(back.fails == r.fails, "[dossier] the failure counter survives - a throttle a "
                                 "restart resets is not a throttle");
    CHECK(back.lock_until == r.lock_until, "[dossier] the deadline survives, for the same reason");
    CHECK(memcmp(back.pin.salt, r.pin.salt, APPLOCK_SALT_LEN) == 0, "[dossier] the salt survives");
    CHECK(memcmp(back.pin.hash, r.pin.hash, APPLOCK_HASH_LEN) == 0, "[dossier] the hash survives");
    CHECK(memcmp(back.pattern.hash, r.pattern.hash, APPLOCK_HASH_LEN) == 0, "[dossier] and the second one");

    /* THE COUNTER-CASE for serialisation: a buffer one byte short must be an
     * ERROR, never a shorter file. A truncated record still parses - as a
     * DIFFERENT lock, missing whichever field the cut removed. */
    CHECK(applock_serialize(&r, buf, (size_t)n) == -1, "[dossier] no truncated record");
    CHECK(applock_serialize(&r, buf, 1) == -1, "[dossier] nor into one byte");
    CHECK(applock_serialize(&r, buf, 0) == -1, "[dossier] nor into none");

    /* No version line: not our file. NO LOCK - see the header on why open and
     * not closed. */
    { const char *s = "methods=1\niterations=60000\n";
      CHECK(!applock_parse(s, strlen(s), &back), "[dossier] a record with no version is refused");
      CHECK(!applock_is_armed(&back), "[dossier] and it leaves no lock behind"); }
    { const char *s = "version=99\nmethods=1\n";
      CHECK(!applock_parse(s, strlen(s), &back), "[dossier] a version we do not know is refused");
      CHECK(!applock_is_armed(&back), "[dossier] and leaves no lock either"); }
    CHECK(!applock_parse(NULL, 10, &back), "[dossier] NULL text");
    CHECK(!applock_is_armed(&back), "[dossier] which is also no lock");

    /* === COMPATIBILITY WITH A RECORD WRITTEN BEFORE ENCRYPTION ===
     *
     * Version 1 has two fields per secret and no sealed key. It must still OPEN
     * THE APPLICATION - refusing it would lock out anyone who armed the lock
     * before this build, and the message they would get is "no lock", i.e. the
     * application silently unprotected. */
    {
        char v1[APPLOCK_TEXT_MAX];
        char salt_hex[2 * APPLOCK_SALT_LEN + 1], hash_hex[2 * APPLOCK_HASH_LEN + 1];
        uint8_t salt[APPLOCK_SALT_LEN], hash[APPLOCK_HASH_LEN];
        for (i = 0; i < APPLOCK_SALT_LEN; i++) salt[i] = (uint8_t)(i + 1);
        for (i = 0; i < APPLOCK_HASH_LEN; i++) hash[i] = (uint8_t)(i + 40);
        applock_to_hex(salt, APPLOCK_SALT_LEN, salt_hex);
        applock_to_hex(hash, APPLOCK_HASH_LEN, hash_hex);
        snprintf(v1, sizeof v1, "version=1\nmethods=1\npin=%s:%s\n", salt_hex, hash_hex);
        CHECK(applock_parse(v1, strlen(v1), &back), "[v1] a record from before encryption reads");
        CHECK(applock_usable_methods(&back) == APPLOCK_PIN, "[v1] and still opens the application");
        CHECK(memcmp(back.pin.hash, hash, APPLOCK_HASH_LEN) == 0, "[v1] its hash is intact");
        CHECK(!applock_has_wrap(&back.pin), "[v1] it seals nothing, which is correct");

        /* The third field, when present. A record carrying one must not lose it
         * on a read-write round trip - that would orphan the token. */
        {
            char v2[APPLOCK_TEXT_MAX], wrap_hex[2 * APPLOCK_WRAP_LEN + 1], out2[APPLOCK_TEXT_MAX];
            uint8_t wrap[APPLOCK_WRAP_LEN];
            applock_record_t again;
            for (i = 0; i < APPLOCK_WRAP_LEN; i++) wrap[i] = (uint8_t)(i * 7 + 5);
            applock_to_hex(wrap, APPLOCK_WRAP_LEN, wrap_hex);
            snprintf(v2, sizeof v2, "version=2\nmethods=1\npin=%s:%s:%s\n",
                     salt_hex, hash_hex, wrap_hex);
            CHECK(applock_parse(v2, strlen(v2), &back), "[v2] a record with a sealed key reads");
            CHECK(applock_has_wrap(&back.pin), "[v2] the sealed key is there");
            CHECK(memcmp(back.pin.wrap, wrap, APPLOCK_WRAP_LEN) == 0, "[v2] and intact");
            CHECK(applock_serialize(&back, out2, sizeof out2) > 0, "[v2] it writes back");
            CHECK(applock_parse(out2, strlen(out2), &again), "[v2] and reads again");
            CHECK(memcmp(again.pin.wrap, wrap, APPLOCK_WRAP_LEN) == 0,
                  "[v2] the sealed key survives a round trip - losing it would orphan the token");

            /* A DAMAGED sealed key drops the sealing ONLY. The secret must still
             * open the application: refusing the whole line would turn a
             * corrupted third field into a lost lock. */
            snprintf(v2, sizeof v2, "version=2\nmethods=1\npin=%s:%s:zzzz\n", salt_hex, hash_hex);
            CHECK(applock_parse(v2, strlen(v2), &back), "[v2] a damaged sealed key is survivable");
            CHECK(applock_usable_methods(&back) == APPLOCK_PIN, "[v2] the secret still opens");
            CHECK(!applock_has_wrap(&back.pin), "[v2] and nothing pretends to be sealed");
        }
    }
    { const char *s = "version=3\nmethods=1\n";
      CHECK(!applock_parse(s, strlen(s), &back), "[v2] a version from the FUTURE is refused"); }

    /* An unknown key is IGNORED, not fatal: a record written by a later version
     * must not lock this one out of its own settings. */
    { const char *s = "version=1\nmethods=1\nsomething_new=42\n";
      CHECK(applock_parse(s, strlen(s), &back), "[dossier] an unknown key is ignored");
      CHECK(back.methods == APPLOCK_PIN, "[dossier] and the rest still reads"); }

    /* A malformed secret drops THAT method, not the whole record: the other
     * method must keep working. */
    { const char *s = "version=1\nmethods=3\npin=zzzz:zzzz\n";
      CHECK(applock_parse(s, strlen(s), &back), "[dossier] a malformed secret is survivable");
      CHECK(applock_usable_methods(&back) == 0, "[dossier] and that method is not offered"); }

    /* Comments and blank lines, because the file says on its first line that
     * deleting it removes the lock - so it is meant to be opened and read. */
    { const char *s = "# a comment\n\nversion=1\nmethods=4\n\n";
      CHECK(applock_parse(s, strlen(s), &back), "[dossier] comments and blank lines are fine");
      CHECK(back.methods == APPLOCK_PATTERN, "[dossier] and do not disturb the reading"); }

    /* An iteration count outside the range goes back to the default. Too low
     * makes the derivation free; too high freezes the screen for minutes with
     * nothing the user can press. */
    { const char *s = "version=1\nmethods=1\niterations=1\n";
      applock_parse(s, strlen(s), &back);
      CHECK(back.iterations == APPLOCK_ITER_DEFAULT, "[dossier] too few iterations is put back"); }
    { const char *s = "version=1\nmethods=1\niterations=999999999\n";
      applock_parse(s, strlen(s), &back);
      CHECK(back.iterations == APPLOCK_ITER_DEFAULT, "[dossier] too many, likewise"); }
    { const char *s = "version=1\nmethods=1\niterations=99999999999999999999\n";
      applock_parse(s, strlen(s), &back);
      CHECK(back.iterations == APPLOCK_ITER_DEFAULT,
            "[dossier] a number that overflows does not become a small one"); }

    /* A method bit we do not know is masked off rather than carried: it would
     * otherwise make `methods` non-zero forever and could not be turned off
     * from the interface. */
    { const char *s = "version=1\nmethods=255\n";
      applock_parse(s, strlen(s), &back);
      CHECK(back.methods == APPLOCK_ALL, "[dossier] unknown method bits are dropped"); }

    /* A line longer than the read buffer is SKIPPED, not truncated: a truncated
     * hash field would still be valid hex, and the record would silently lose a
     * method - or worse, keep one with a hash nobody can produce. */
    { char big[600];
      int k = snprintf(big, sizeof big, "version=1\nmethods=1\npin=");
      memset(big + k, 'a', 400);
      big[k + 400] = '\n'; big[k + 401] = 0;
      CHECK(applock_parse(big, strlen(big), &back), "[dossier] an overlong line is survivable");
      CHECK(applock_usable_methods(&back) == 0, "[dossier] and it does not arm a method"); }

    /* A file with no trailing newline - what a hand edit leaves behind. */
    { const char *s = "version=1\nmethods=2";
      CHECK(applock_parse(s, strlen(s), &back), "[dossier] the last line needs no newline");
      CHECK(back.methods == APPLOCK_PASSWORD, "[dossier] and is read"); }

    /* CRLF, because the file may be edited from Windows. */
    { const char *s = "version=1\r\nmethods=1\r\n";
      CHECK(applock_parse(s, strlen(s), &back), "[dossier] CRLF is accepted");
      CHECK(back.methods == APPLOCK_PIN, "[dossier] and the value is not left with a \\r"); }
}

int main(void)
{
    printf("test_applock - verrou de l'application (coeur pur)\n");
    the_throttle();
    a_half_written_record();
    the_pattern();
    the_comparison();
    the_hex();
    the_inputs();
    the_record();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
