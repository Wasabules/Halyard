/* test_authz.c - AUTH-1: who is allowed to drive this console (devlink/authz.h).
 *
 * The decision is pure, so it is checked here with no console, no file and no
 * network. What matters is not that a known machine is let through - it is that
 * the near-misses are NOT: a prefix, a different port, a line left by a human
 * with a stray space.
 */
#include "../clients/borealis/devlink/authz.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;

#define CHECK(cond, what) do {                                              \
    checks++;                                                               \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static authz_verdict_t look(const char *list, const char *peer)
{
    return authz_lookup(list, strlen(list), peer);
}

static void peer_shape(void)
{
    CHECK(authz_peer_ok("192.168.1.113:9999", 18), "an address and a port");
    CHECK(authz_peer_ok("dev-machine:9999", sizeof "dev-machine:9999" - 1), "a host name works too");
    CHECK(authz_peer_ok("fe80::1:9999", 12), "IPv6: the LAST colon separates the port");

    CHECK(!authz_peer_ok("192.168.1.113", 13), "no port: refused");
    CHECK(!authz_peer_ok(":9999", 5), "no host: refused");
    CHECK(!authz_peer_ok("192.168.1.113:", 14), "an empty port: refused");
    CHECK(!authz_peer_ok("192.168.1.113:abc", 17), "a port that is not a number: refused");
    CHECK(!authz_peer_ok("un poste:9999", 13), "a space: refused - one peer per line");
    CHECK(!authz_peer_ok("", 0), "empty: refused");
    CHECK(!authz_peer_ok(NULL, 4), "null: refused");
    {
        char big[AUTHZ_PEER_MAX + 8];
        memset(big, 'a', sizeof big);
        memcpy(big + sizeof(big) - 6, ":9999", 6);
        CHECK(!authz_peer_ok(big, strlen(big)),
                "over-long: REFUSED, not truncated - a cut address names another machine");
    }
}

static void lookup_rules(void)
{
    const char *list =
        "# machines de confiance\n"
        "192.168.1.113:9999\n"
        "\n"
        "  192.168.1.80:9999  \n"
        "-10.0.0.5:9999\n";

    CHECK(look(list, "192.168.1.113:9999") == AUTHZ_ALLOWED, "a listed machine passes");
    CHECK(look(list, "192.168.1.80:9999") == AUTHZ_ALLOWED,
            "surrounding whitespace is trimmed: a human may edit this file");
    CHECK(look(list, "10.0.0.5:9999") == AUTHZ_DENIED,
            "a refusal is KEPT, so a machine turned away is not asked about again");
    CHECK(look(list, "192.168.1.9:9999") == AUTHZ_UNKNOWN, "anything else is asked about");

    /* THE COUNTER-CASE THAT MATTERS. A prefix match would let "192.168.1.1"
     * authorise "192.168.1.113" - and the address that gets authorised is the
     * one a router hands out first. */
    CHECK(look("192.168.1.1:9999\n", "192.168.1.113:9999") == AUTHZ_UNKNOWN,
            "COUNTER-CASE: a prefix does NOT authorise a longer address");
    CHECK(look("192.168.1.113:9999\n", "192.168.1.113:9998") == AUTHZ_UNKNOWN,
            "COUNTER-CASE: a different port is a different peer");
    CHECK(look("192.168.1.1139999\n", "192.168.1.113:9999") == AUTHZ_UNKNOWN,
            "a mangled line authorises nothing");

    CHECK(look("# nothing but comments\n", "192.168.1.113:9999") == AUTHZ_UNKNOWN,
            "a file of comments authorises nothing");
    CHECK(look("", "192.168.1.113:9999") == AUTHZ_UNKNOWN, "an empty file: asked about");
    CHECK(authz_lookup(NULL, 0, "192.168.1.113:9999") == AUTHZ_UNKNOWN, "no file: asked about");
    CHECK(look("192.168.1.113:9999\n", NULL) == AUTHZ_UNKNOWN, "no peer: never allowed");

    /* Line endings: the card may have been written on Windows. */
    CHECK(look("192.168.1.113:9999\r\n", "192.168.1.113:9999") == AUTHZ_ALLOWED,
            "CRLF is read like LF");
    CHECK(look("192.168.1.113:9999", "192.168.1.113:9999") == AUTHZ_ALLOWED,
            "a last line with no newline still counts");

    /* A refusal and an authorisation for the same machine: the refusal is met
     * first only if it comes first. Order is the file's, and that is what a
     * human editing it would expect. */
    CHECK(look("-1.2.3.4:1\n1.2.3.4:1\n", "1.2.3.4:1") == AUTHZ_DENIED,
            "the first matching line decides");
}

int main(void)
{
    printf("== AUTH-1: who is allowed to drive this console ==\n");
    peer_shape();
    lookup_rules();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
