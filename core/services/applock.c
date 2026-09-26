/* See applock.h for the why. */
#include "applock.h"

#include <string.h>
#include <stdio.h>

/* ── Le dossier ───────────────────────────────────────────────────────────── */

void applock_record_init(applock_record_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof *r);
    r->version    = APPLOCK_VERSION;
    r->iterations = APPLOCK_ITER_DEFAULT;
}

static int secret_present(const applock_secret_t *s)
{
    size_t i;
    /* All-zero means "never written". A real PBKDF2 output is all-zero with
     * probability 2^-256, so this test costs nothing and catches the only case
     * that occurs in practice: a half-written or hand-edited file. */
    for (i = 0; i < APPLOCK_HASH_LEN; i++) if (s->hash[i]) return 1;
    return 0;
}

int applock_has_wrap(const applock_secret_t *s)
{
    size_t i;
    if (!s) return 0;
    for (i = 0; i < APPLOCK_WRAP_LEN; i++) if (s->wrap[i]) return 1;
    return 0;
}

uint32_t applock_usable_methods(const applock_record_t *r)
{
    uint32_t m = 0;
    if (!r) return 0;
    if ((r->methods & APPLOCK_PIN)      && secret_present(&r->pin))      m |= APPLOCK_PIN;
    if ((r->methods & APPLOCK_PASSWORD) && secret_present(&r->password)) m |= APPLOCK_PASSWORD;
    if ((r->methods & APPLOCK_PATTERN)  && secret_present(&r->pattern))  m |= APPLOCK_PATTERN;
    return m;
}

int applock_is_armed(const applock_record_t *r)
{
    return applock_usable_methods(r) != 0;
}

/* ── Hex ──────────────────────────────────────────────────────────────────── */

void applock_to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char D[] = "0123456789abcdef";
    size_t i;
    if (!in || !out) return;
    for (i = 0; i < n; i++) {
        out[2 * i]     = D[(in[i] >> 4) & 0xF];
        out[2 * i + 1] = D[in[i] & 0xF];
    }
    out[2 * n] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int applock_from_hex(const char *in, size_t n, uint8_t *out)
{
    size_t i;
    if (!in || !out) return 0;
    /* The length is checked FIRST and exactly. A shorter field must not decode
     * into a half-filled buffer whose tail is whatever was there before. */
    if (strlen(in) != 2 * n) return 0;
    for (i = 0; i < n; i++) {
        const int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/* ── Comparaison a temps constant ─────────────────────────────────────────── */

int applock_equal_ct(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    size_t i;
    if (!a || !b) return 0;
    /* No early exit, and the accumulator is not tested inside the loop: both
     * would reintroduce the timing dependency this function exists to remove. */
    for (i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* ── L'etranglement ───────────────────────────────────────────────────────── */

uint32_t applock_penalty_seconds(uint32_t fails)
{
    /* === THE SCHEDULE, AND THE ARITHMETIC BEHIND IT ===
     *
     * Four free attempts: a wrong PIN happens, and a lock that punishes the
     * first slip is one people turn off.
     *
     * From the fifth, at least 30 s each. A four-digit PIN has 10 000
     * combinations; at 30 s apiece that is 83 hours of someone standing over
     * the console pressing keys. Against the threat this lock is for - a person
     * who has the console in their hands for a while - that is enough, and it
     * is the whole reason the throttle exists rather than the hash.
     *
     * Capped at ten minutes. Beyond that it protects nothing further (the card
     * can be wiped in seconds) and turns a forgotten PIN into a dead app. */
    if (fails <= 4)  return 0;
    if (fails == 5)  return 30;
    if (fails == 6)  return 60;
    if (fails == 7)  return 120;
    if (fails == 8)  return 300;
    return 600;
}

void applock_note_failure(applock_record_t *r, int64_t now)
{
    uint32_t p;
    if (!r) return;
    /* Saturating: at UINT32_MAX the counter must not wrap back to 0, which
     * would hand out four free attempts again. */
    if (r->fails < 0xFFFFFFFFu) r->fails++;
    p = applock_penalty_seconds(r->fails);
    if (p == 0) { r->lock_until = 0; return; }
    /* `max` and not assignment: a clock that jumped backwards between two
     * failures must not shorten a deadline already set. */
    {
        const int64_t deadline = now + (int64_t)p;
        if (deadline > r->lock_until) r->lock_until = deadline;
    }
}

void applock_note_success(applock_record_t *r)
{
    if (!r) return;
    r->fails = 0;
    r->lock_until = 0;
}

int applock_locked_out(const applock_record_t *r, int64_t now, int64_t *remaining)
{
    int64_t left;
    if (remaining) *remaining = 0;
    if (!r || r->lock_until == 0) return 0;
    if (now >= r->lock_until) return 0;

    /* === THE DEADLINE IS CLAMPED WHEN IT IS READ, NOT ONLY WHEN IT IS SET ===
     *
     * The penalty is capped at ten minutes, but the DEADLINE it produces is an
     * absolute instant, and the console's clock is set by hand. Set the clock a
     * year ahead, make one wrong guess, set it back: the record now holds a
     * deadline a year away, and the "never shortens" rule - which is right, it
     * is what stops a backwards clock from cancelling a wait - would keep the
     * owner out of their own console until then.
     *
     * Clamping here keeps both properties. A deadline further away than the
     * longest penalty can only have come from a clock that moved, so it is
     * honoured for at most that penalty; and nothing is written, so a clock that
     * moves back again still cannot shorten what is stored. */
    left = r->lock_until - now;
    {
        const int64_t cap = (int64_t)applock_penalty_seconds(0xFFFFFFFFu);
        if (left > cap) left = cap;
    }
    if (left < 1) left = 1;   /* never display "0 s" on a screen still refusing */
    if (remaining) *remaining = left;
    return 1;
}

/* ── Les saisies ──────────────────────────────────────────────────────────── */

int applock_pin_valid(const char *s)
{
    size_t n, i;
    if (!s) return 0;
    n = strlen(s);
    if (n < (size_t)APPLOCK_PIN_MIN || n > (size_t)APPLOCK_PIN_MAX) return 0;
    for (i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

int applock_password_valid(const char *s)
{
    size_t n, i;
    if (!s) return 0;
    n = strlen(s);
    if (n < (size_t)APPLOCK_PASSWORD_MIN || n > (size_t)APPLOCK_PASSWORD_MAX) return 0;
    /* === WHAT IS REFUSED, AND WHAT WAS WRONGLY REFUSED ===
     *
     * This used to demand printable ASCII, on the reasoning that a password
     * must be RE-TYPABLE. The reasoning was right and the rule was wrong: the
     * console's software keyboard produces accented letters, and every other
     * script it offers, perfectly repeatably. A French speaker typing their own
     * name was told "too short", which is both false and impossible to act on.
     *
     * What is actually refused is the C0 control range and DEL - bytes no
     * keyboard emits as a character, and which would make the stored secret
     * depend on how a terminal or a log happened to handle them. Everything
     * above 0x7F is UTF-8 continuation or lead bytes and is accepted as-is: the
     * hash takes bytes, and the keyboard gives back the same bytes.
     *
     * Note the length is counted in BYTES, so an accented password reaches the
     * maximum sooner than its character count suggests. That is the right unit
     * here - it is what bounds the buffers - and the minimum is low enough that
     * it cannot bite. */
    for (i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return 0;
    }
    return 1;
}

int applock_pattern_valid(const uint8_t *nodes, size_t n)
{
    uint16_t seen = 0;
    size_t i;
    if (!nodes) return 0;
    if (n < (size_t)APPLOCK_PATTERN_MIN || n > (size_t)APPLOCK_PATTERN_MAX) return 0;
    for (i = 0; i < n; i++) {
        if (nodes[i] > 8) return 0;
        if (seen & (uint16_t)(1u << nodes[i])) return 0;   /* no node twice */
        seen |= (uint16_t)(1u << nodes[i]);
    }
    return 1;
}

int applock_pattern_canonical(const uint8_t *nodes, size_t n,
                              uint8_t *out, size_t cap)
{
    uint8_t tmp[APPLOCK_PATTERN_MAX];
    uint16_t seen = 0;
    size_t len = 0, i;

    if (!nodes || !out) return -1;
    if (n == 0 || n > (size_t)APPLOCK_PATTERN_MAX) return -1;

    for (i = 0; i < n; i++) {
        const uint8_t b = nodes[i];
        if (b > 8) return -1;
        /* === A NODE ALREADY IN THE SEQUENCE IS SKIPPED, NOT REFUSED ===
         *
         * This used to return -1, on the reasoning that a tap sequence never
         * repeats. It does not - but the CANONICAL sequence contains nodes the
         * finger never touched, and the finger can then touch one of them.
         *
         * Draw 0 to 2: node 1 is inserted by the rule below. Now slide back over
         * node 1, which is a perfectly ordinary thing to do and exactly what
         * Android allows (touching an already-lit node does nothing). The tap
         * sequence is then 0, 2, 1 - no literal repeat, so the caller's own
         * de-duplication lets it through - and this function refused the whole
         * gesture. On screen: "too short", entry cleared, on a pattern the user
         * drew correctly.
         *
         * Skipping is both correct and what Android does. A literal repeat now
         * collapses too, which is the same rule seen from the other side. */
        if (seen & (uint16_t)(1u << b)) continue;

        if (len > 0) {
            /* The crossed node. On a 3x3 grid the segment a->b passes over a
             * node exactly when the two rows have the same parity AND the two
             * columns do; the crossed node is then their midpoint. Any other
             * segment (a knight's move, an adjacent step) passes over nothing.
             *
             * This is the whole rule, and it is why 0->2 and 0->1->2 are the
             * same pattern. */
            const uint8_t a = tmp[len - 1];
            const int ra = a / 3, ca = a % 3, rb = b / 3, cb = b % 3;
            if (((ra + rb) % 2) == 0 && ((ca + cb) % 2) == 0) {
                const uint8_t mid = (uint8_t)(((ra + rb) / 2) * 3 + (ca + cb) / 2);
                /* `mid != a` excludes the degenerate case a == b, already
                 * rejected above, and costs nothing to keep explicit. */
                if (mid != a && !(seen & (uint16_t)(1u << mid))) {
                    if (len >= sizeof tmp) return -1;
                    tmp[len++] = mid;
                    seen |= (uint16_t)(1u << mid);
                }
            }
        }
        if (len >= sizeof tmp) return -1;
        tmp[len++] = b;
        seen |= (uint16_t)(1u << b);
    }

    if (len > cap) return -1;
    /* Copied at the end, through `tmp`: `out` is allowed to be the same buffer
     * as `nodes`, and writing as we went would overwrite input we still had to
     * read. */
    memcpy(out, tmp, len);
    return (int)len;
}

int applock_pattern_bytes(const uint8_t *nodes, size_t n, char *out, size_t cap)
{
    size_t i;
    if (!nodes || !out) return -1;
    if (n == 0 || n + 1 > cap) return -1;
    for (i = 0; i < n; i++) {
        if (nodes[i] > 8) return -1;
        out[i] = (char)('0' + nodes[i]);
    }
    out[n] = 0;
    return (int)n;
}

/* ── Reading and writing the directory ────────────────────────────────────── */

/* One `key=value` per line, like the neighbouring files in the data directory.
 * Keys in English: `settings.txt` is in French because it predates the
 * migration, but `env.txt` and `autotest.txt` are not, and a new format has no
 * legacy to keep. */
static int parse_u32(const char *v, uint32_t *out)
{
    uint32_t acc = 0;
    if (!v || !*v) return 0;
    for (; *v; v++) {
        if (*v < '0' || *v > '9') return 0;
        if (acc > (0xFFFFFFFFu - (uint32_t)(*v - '0')) / 10u) return 0;   /* overflow */
        acc = acc * 10u + (uint32_t)(*v - '0');
    }
    *out = acc;
    return 1;
}

static int parse_i64(const char *v, int64_t *out)
{
    int64_t acc = 0;
    int neg = 0;
    if (!v || !*v) return 0;
    if (*v == '-') { neg = 1; v++; if (!*v) return 0; }
    for (; *v; v++) {
        if (*v < '0' || *v > '9') return 0;
        if (acc > (0x7FFFFFFFFFFFFFFFll - (*v - '0')) / 10) return 0;
        acc = acc * 10 + (*v - '0');
    }
    *out = neg ? -acc : acc;
    return 1;
}

/* Copies one colon-separated field out of `v` into `dst` and decodes it.
 * `end` receives the character after the field. Returns 0 on any length or
 * character mismatch - a partial decode would leave half a buffer holding
 * whatever was there before, and that half would then be compared against a
 * real hash. */
static int take_hex_field(const char *v, size_t nbytes, uint8_t *dst,
                          const char **end)
{
    char buf[2 * APPLOCK_WRAP_LEN + 1];
    const char *colon = strchr(v, ':');
    const size_t len = colon ? (size_t)(colon - v) : strlen(v);
    if (len != 2 * nbytes || len >= sizeof buf) return 0;
    memcpy(buf, v, len);
    buf[len] = 0;
    if (!applock_from_hex(buf, nbytes, dst)) return 0;
    if (end) *end = colon ? colon + 1 : NULL;
    return 1;
}

static int parse_secret(const char *v, applock_secret_t *s)
{
    /* "<salt>:<hash>" or "<salt>:<hash>:<sealed master key>", all in hex.
     *
     * One field rather than two or three, so a record can never carry a salt
     * without its hash. The third part is OPTIONAL: a record written before
     * encryption existed has two, and must keep opening the application - it
     * simply unseals nothing, and the token stays in its older form until the
     * secret is set again. */
    const char *rest = NULL;
    if (!take_hex_field(v, APPLOCK_SALT_LEN, s->salt, &rest) || !rest) return 0;
    if (!take_hex_field(rest, APPLOCK_HASH_LEN, s->hash, &rest)) return 0;
    if (rest && *rest) {
        if (!take_hex_field(rest, APPLOCK_WRAP_LEN, s->wrap, NULL)) {
            /* A malformed sealed key drops ONLY the sealing: the secret still
             * opens the application. Refusing the whole line would turn a
             * corrupted third field into a lost lock. */
            memset(s->wrap, 0, sizeof s->wrap);
        }
    }
    return 1;
}

int applock_parse(const char *text, size_t len, applock_record_t *out)
{
    char line[256];
    size_t i = 0;
    int seen_version = 0;

    if (!out) return 0;
    applock_record_init(out);
    if (!text) return 0;

    while (i < len) {
        size_t j = i, n;
        char *eq;
        while (j < len && text[j] != '\n') j++;
        n = j - i;
        /* A line longer than the buffer is SKIPPED, not truncated: truncating
         * would turn a corrupt hash field into a shorter one that still parses
         * as hex, and the record would silently lose a method. */
        if (n > 0 && n < sizeof line) {
            memcpy(line, text + i, n);
            line[n] = 0;
            while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
            if (line[0] != '#' && (eq = strchr(line, '=')) != NULL) {
                const char *v = eq + 1;
                *eq = 0;
                if      (!strcmp(line, "version"))    seen_version = parse_u32(v, &out->version);
                else if (!strcmp(line, "methods"))    (void)parse_u32(v, &out->methods);
                else if (!strcmp(line, "iterations")) (void)parse_u32(v, &out->iterations);
                else if (!strcmp(line, "fails"))      (void)parse_u32(v, &out->fails);
                else if (!strcmp(line, "lock_until")) (void)parse_i64(v, &out->lock_until);
                else if (!strcmp(line, "pin"))        { if (!parse_secret(v, &out->pin))      memset(&out->pin, 0, sizeof out->pin); }
                else if (!strcmp(line, "password"))   { if (!parse_secret(v, &out->password)) memset(&out->password, 0, sizeof out->password); }
                else if (!strcmp(line, "pattern"))    { if (!parse_secret(v, &out->pattern))  memset(&out->pattern, 0, sizeof out->pattern); }
                /* anything else: ignored, see the header */
            }
        }
        i = j + 1;
    }

    /* A record with no version line is not one of ours - most likely a file
     * that happens to share the name. Refuse it, which means NO LOCK. */
    /* Version 1 is the shape before encryption: two fields per secret, no
     * sealed key. It is still read, and still opens the application - only the
     * token stays in its older form. Refusing it would lock out anyone who
     * armed the lock before this build. */
    if (!seen_version || out->version < 1 || out->version > APPLOCK_VERSION) {
        applock_record_init(out);
        return 0;
    }
    /* An iteration count outside the accepted range is put back to the default
     * rather than honoured: too low would make the derivation free, too high
     * would freeze the screen for minutes with no way to interrupt it. */
    if (out->iterations < APPLOCK_ITER_MIN || out->iterations > APPLOCK_ITER_MAX)
        out->iterations = APPLOCK_ITER_DEFAULT;
    out->methods &= APPLOCK_ALL;
    return 1;
}

static void write_secret(char *dst, const applock_secret_t *s)
{
    size_t at = 0;
    applock_to_hex(s->salt, APPLOCK_SALT_LEN, dst);
    at = 2 * APPLOCK_SALT_LEN;
    dst[at++] = ':';
    applock_to_hex(s->hash, APPLOCK_HASH_LEN, dst + at);
    at += 2 * APPLOCK_HASH_LEN;
    /* Written only when there is one, so a record with no encryption keeps the
     * two-field shape it had - and an old build reading it finds what it
     * expects. */
    if (applock_has_wrap(s)) {
        dst[at++] = ':';
        applock_to_hex(s->wrap, APPLOCK_WRAP_LEN, dst + at);
    }
}

int applock_serialize(const applock_record_t *r, char *out, size_t cap)
{
    char pin[2 * (APPLOCK_SALT_LEN + APPLOCK_HASH_LEN + APPLOCK_WRAP_LEN) + 3];
    char pwd[sizeof pin], pat[sizeof pin];
    int n;

    if (!r || !out) return -1;
    write_secret(pin, &r->pin);
    write_secret(pwd, &r->password);
    write_secret(pat, &r->pattern);

    n = snprintf(out, cap,
                 "# halyard application lock. Deleting this file removes the lock.\n"
                 "version=%u\n"
                 "methods=%u\n"
                 "iterations=%u\n"
                 "fails=%u\n"
                 "lock_until=%lld\n"
                 "pin=%s\n"
                 "password=%s\n"
                 "pattern=%s\n",
                 r->version, r->methods, r->iterations, r->fails,
                 (long long)r->lock_until, pin, pwd, pat);
    /* `snprintf` returns what it WOULD have written: a truncated record must be
     * an error, never a shorter file that parses as a different lock. */
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}
