/* test_ft_uri - the SFTP session as a URI a file manager can open.
 *
 * Every case here is one of the three ways the string gets silently wrong
 * (ft_uri.h): a base64 password whose `+ / =` change meaning in a URI, an IPv6
 * host that needs bracketing exactly once, and a buffer too small - which must
 * refuse, because a URI truncated mid-password looks valid and authenticates as
 * something else.
 *
 * The MUTATION CHECKS at the bottom are the point of the file: they assert that
 * a raw `+`, `/` or `=` never survives into the output, and that a bracketed
 * host is not bracketed twice. Those are the two edits that would compile,
 * produce a plausible URI, and fail at the far end with "authentication
 * failed".
 *
 * Compiled with -Wall -Wextra -Werror -O1 by tests/run_tests.sh.
 */
#include <stdio.h>
#include <string.h>

#include "../core/protocol/ft_uri.h"

static int checks = 0, failures = 0;

static void eq(const char *got, const char *want, const char *what)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL %-44s got \"%s\", expected \"%s\"\n",
               what, got ? got : "(null)", want);
    }
}

static void expect_uri(const char *host, int port, const char *user,
                       const char *secret, const char *want, const char *what)
{
    char out[2048];
    const size_t n = ft_uri_build(out, sizeof out, host, port, user,
                                  secret, secret ? strlen(secret) : 0);
    eq(out, want, what);
    checks++;
    if (n != strlen(want)) {
        failures++;
        printf("  FAIL %-44s returned %u, expected %u\n",
               what, (unsigned)n, (unsigned)strlen(want));
    }
}

static void expect_refused(char *out, size_t cap, const char *host, int port,
                           const char *secret, const char *what)
{
    checks++;
    const size_t n = ft_uri_build(out, cap, host, port, "shadow",
                                  secret, secret ? strlen(secret) : 0);
    if (n != 0 || (cap > 0 && out[0] != '\0')) {
        failures++;
        printf("  FAIL %-44s expected a refusal, got %u byte(s): \"%s\"\n",
               what, (unsigned)n, cap > 0 ? out : "");
    }
}

/* True when `s` contains `c` - used by the mutation checks to assert that a
 * character which MUST have been encoded is nowhere in the output. */
static int has(const char *s, char c)
{
    for (; *s; s++) if (*s == c) return 1;
    return 0;
}

int main(void)
{
    printf("== SFTP URI for a file manager (FT4 2026-10-02) ==\n");

    /* --- the encoder on its own ------------------------------------------- */
    {
        char b[64];
        eq((ft_uri_encode("abc", 3, b, sizeof b), b), "abc", "unreserved passes through");
        eq((ft_uri_encode("a-b._~", 6, b, sizeof b), b), "a-b._~", "the full unreserved set");
        eq((ft_uri_encode("+", 1, b, sizeof b), b), "%2B", "plus, read as a space otherwise");
        eq((ft_uri_encode("/", 1, b, sizeof b), b), "%2F", "slash, which would end the authority");
        eq((ft_uri_encode("=", 1, b, sizeof b), b), "%3D", "equals, the base64 padding");
        eq((ft_uri_encode("@", 1, b, sizeof b), b), "%40", "at, which would end the userinfo");
        eq((ft_uri_encode(":", 1, b, sizeof b), b), "%3A", "colon, which splits user and password");
        eq((ft_uri_encode("%", 1, b, sizeof b), b), "%25", "percent, or the escapes compound");
        eq((ft_uri_encode(" ", 1, b, sizeof b), b), "%20", "space");
        eq((ft_uri_encode("#", 1, b, sizeof b), b), "%23", "hash, which would start a fragment");
        eq((ft_uri_encode("?", 1, b, sizeof b), b), "%3F", "question mark, a query");

        /* High bytes: the field is base64 today, but an encoder that mangles
         * them is a trap for the day it is not. */
        eq((ft_uri_encode("\xC3\xA9", 2, b, sizeof b), b), "%C3%A9",
           "a non-ASCII byte pair, upper-case hex");

        /* The NUL is encodable: `secret_len` is explicit precisely because the
         * wire field is not NUL-terminated. */
        eq((ft_uri_encode("a\0b", 3, b, sizeof b), b), "a%00b", "an embedded NUL");

        /* Length accounting, and the refusal. "ab" needs 2+1; a 3-byte buffer
         * fits, a 2-byte one must not half-write. */
        checks++;
        if (ft_uri_encode("ab", 2, b, 3) != 2) { failures++; printf("  FAIL exact fit\n"); }
        checks++;
        if (ft_uri_encode("ab", 2, b, 2) != 0 || b[0] != '\0') {
            failures++; printf("  FAIL encode must refuse, not truncate\n");
        }
        checks++;
        if (ft_uri_encode("+", 1, b, 3) != 0 || b[0] != '\0') {
            failures++; printf("  FAIL an escape needs 3 bytes plus the NUL\n");
        }
        checks++;
        if (ft_uri_encode("", 0, b, sizeof b) != 0 || b[0] != '\0') {
            failures++; printf("  FAIL empty input is an empty string\n");
        }
    }

    /* --- bracketing ------------------------------------------------------- */
    checks++;
    if (ft_uri_needs_brackets("2001:db8:1:2::") != true) {
        failures++; printf("  FAIL an IPv6 literal needs brackets\n");
    }
    checks++;
    if (ft_uri_needs_brackets("[2001:db8::]") != false) {
        failures++; printf("  FAIL an already-bracketed host must be left alone\n");
    }
    checks++;
    if (ft_uri_needs_brackets("vm.compute.shadow.tech") != false) {
        failures++; printf("  FAIL a name has no colon\n");
    }
    checks++;
    if (ft_uri_needs_brackets("10.0.0.4") != false) {
        failures++; printf("  FAIL an IPv4 address has no colon\n");
    }
    checks++;
    if (ft_uri_needs_brackets("") != false || ft_uri_needs_brackets(NULL) != false) {
        failures++; printf("  FAIL an empty or absent host is not bracketed\n");
    }

    /* --- the whole URI ---------------------------------------------------- */
    expect_uri("vm.shadow.tech", 10015, "shadow", "plain",
               "sftp://shadow:plain@vm.shadow.tech:10015/", "a name and a plain secret");

    /* The real shape: the live session's host and port, with a base64 secret
     * carrying all three dangerous characters. */
    expect_uri("2001:db8:1:2::", 10015, "shadow", "ab+cd/ef=",
               "sftp://shadow:ab%2Bcd%2Fef%3D@[2001:db8:1:2::]:10015/",
               "IPv6 host, base64 secret");

    expect_uri("[2001:db8::]", 10015, "shadow", "x",
               "sftp://shadow:x@[2001:db8::]:10015/",
               "a host the caller already bracketed");

    /* NULL and empty user both mean "shadow": the VM does not look at the name,
     * so there is no reason to make a caller supply one. */
    expect_uri("h", 22, NULL, "s", "sftp://shadow:s@h:22/", "a NULL user defaults");
    expect_uri("h", 22, "",   "s", "sftp://shadow:s@h:22/", "an empty user defaults");
    expect_uri("h", 22, "a b", "s", "sftp://a%20b:s@h:22/", "the user is encoded too");

    expect_uri("h", 1,     "u", "s", "sftp://u:s@h:1/",     "the lowest port");
    expect_uri("h", 65535, "u", "s", "sftp://u:s@h:65535/", "the highest port");

    /* --- the refusals ----------------------------------------------------- */
    {
        char out[2048];
        expect_refused(out, sizeof out, NULL, 10015, "s", "no host");
        expect_refused(out, sizeof out, "",   10015, "s", "an empty host");
        expect_refused(out, sizeof out, "h",      0, "s", "port 0");
        expect_refused(out, sizeof out, "h",     -1, "s", "a negative port");
        expect_refused(out, sizeof out, "h",  65536, "s", "a port over 65535");
        expect_refused(out, sizeof out, "h",  10015, NULL, "no secret");
        expect_refused(out, sizeof out, "h",  10015, "",   "an empty secret");

        /* The one that matters: a buffer one byte short must produce NOTHING.
         * "sftp://shadow:s@h:22/" is 21 characters, so 21 bytes of buffer are
         * one short of the NUL. */
        char small[21];
        expect_refused(small, sizeof small, "h", 22, "s",
                       "one byte short of the NUL");
        checks++;
        {
            char exact[22];
            if (ft_uri_build(exact, sizeof exact, "h", 22, "shadow", "s", 1) != 21) {
                failures++; printf("  FAIL the exact fit must succeed\n");
            }
        }
        /* A zero-capacity buffer must not be written at all. */
        checks++;
        if (ft_uri_build(out, 0, "h", 22, "u", "s", 1) != 0) {
            failures++; printf("  FAIL cap 0 must refuse\n");
        }
    }

    /* === MUTATION CHECKS =================================================
     *
     * These are the counter-cases. Remove the encoding from ft_uri_build and
     * every assertion above still passes for a plain secret - these do not.
     * A long base64-shaped secret is used so that all three characters are
     * certain to appear. */
    {
        const char *b64 = "AAAA+BBBB/CCCC=DDDD+EEEE/FFFF=GGGG+HHHH/IIII=";
        char out[2048];
        const size_t n = ft_uri_build(out, sizeof out, "2001:db8::", 10015,
                                      "shadow", b64, strlen(b64));
        checks++;
        if (n == 0) { failures++; printf("  FAIL the mutation case did not build\n"); }

        /* The authority ends at the first `/`, so a raw one in the password
         * would make everything after it a PATH - and WinSCP would prompt for a
         * password it was already given. */
        checks++;
        {
            /* Exactly three: the two in "sftp://" and the trailing one. A
             * fourth is a raw '/' out of the password, and everything after it
             * becomes a PATH - WinSCP then asks for a password it has already
             * been given. */
            size_t slashes = 0;
            for (const char *q = out; *q; q++) if (*q == '/') slashes++;
            if (slashes != 3) {
                failures++;
                printf("  FAIL a raw '/' survived into the URI: %u slashes\n",
                       (unsigned)slashes);
            }
        }
        checks++;
        if (has(out, '+')) { failures++; printf("  FAIL a raw '+' survived\n"); }
        checks++;
        if (has(out, '=')) { failures++; printf("  FAIL a raw '=' survived\n"); }

        /* Bracketed exactly once. Two opening brackets is what a `needs` test
         * that ignores an existing bracket produces. */
        checks++;
        {
            size_t ob = 0, cb = 0;
            for (const char *p = out; *p; p++) { if (*p == '[') ob++; if (*p == ']') cb++; }
            if (ob != 1 || cb != 1) {
                failures++;
                printf("  FAIL brackets: %u open, %u close, expected 1 and 1\n",
                       (unsigned)ob, (unsigned)cb);
            }
        }
        /* And the port is still readable at the end, which is what bracketing
         * is FOR: without it, `...:e805:::10015/` has no parseable port. */
        checks++;
        if (!strstr(out, "]:10015/")) {
            failures++; printf("  FAIL the port is not where a parser looks\n");
        }
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
