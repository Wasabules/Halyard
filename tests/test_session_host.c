/* test_session_host.c - one resolution of the VM's name per session (DNS1,
 * 2026-09-11), core/protocol/session_host.c.
 *
 * Only NUMERIC hosts are looked up (127.x.y.z, ::1): getaddrinfo parses them
 * locally, so the suite needs no DNS and no network. What proves the learned
 * address is used: the "name" is itself a numeric address (127.0.0.3), learned
 * as ANOTHER one (127.0.0.1) - a real lookup would answer 127.0.0.3.
 *
 * run_tests.sh runs it twice. The second run sets SHADOW_RESOLVE_ONCE=0, which
 * must restore one real lookup per call: that run asserts the previous
 * behaviour, the counter-case. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/protocol/session_host.h"

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                 \
    if (!(cond)) { printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, what);   \
                   failures++; }                                              \
} while (0)

/* Looks `host:port` up through the module; the first answer's address, port
 * and socket type. */
static int look(const char *host, const char *port, int family, int socktype,
                char *ip, size_t cap, int *p, int *st)
{
    struct addrinfo h, *res = NULL;
    memset(&h, 0, sizeof h);
    h.ai_family = family;
    h.ai_socktype = socktype;
    const int rc = session_getaddrinfo(host, port, &h, &res);
    ip[0] = 0; *p = -1; *st = 0;
    if (rc == 0 && res) {
        if (res->ai_family == AF_INET) {
            const struct sockaddr_in *s = (const struct sockaddr_in *)res->ai_addr;
            inet_ntop(AF_INET, &s->sin_addr, ip, cap);
            *p = ntohs(s->sin_port);
        } else if (res->ai_family == AF_INET6) {
            const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)res->ai_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, ip, cap);
            *p = ntohs(s->sin6_port);
        }
        *st = res->ai_socktype;
        freeaddrinfo(res);
    }
    return rc;
}

static struct sockaddr_in v4(const char *a)
{
    struct sockaddr_in s;
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    inet_pton(AF_INET, a, &s.sin_addr);
    return s;
}

int main(void)
{
    shadow_sockets_init();
    const char *e = getenv("SHADOW_RESOLVE_ONCE");
    const int on = !(e && atoi(e) == 0);
    printf("== session_host: one resolution of the VM name per session (DNS1), "
           "SHADOW_RESOLVE_ONCE=%s ==\n", e ? e : "(unset)");
    char ip[64], got[64] = "";
    int port, st;
    struct sockaddr_in a1 = v4("127.0.0.1"), a2 = v4("127.0.0.2");

    /* The control channel connects first: its address is learned. */
    session_host_reset();
    const int l = session_host_learn("127.0.0.3", (const struct sockaddr *)&a1, got, sizeof got);
    CHECK(on ? (l == 1 && strcmp(got, "127.0.0.1") == 0) : l == 0,
          on ? "the first connection of the session records its numeric address"
             : "revert: nothing is recorded");

    look("127.0.0.3", "8020", AF_UNSPEC, SOCK_STREAM, ip, sizeof ip, &port, &st);
    CHECK(strcmp(ip, on ? "127.0.0.1" : "127.0.0.3") == 0,
          on ? "a later lookup of the same name is answered from the learned address"
             : "COUNTER-CASE (revert): every lookup is a real one");
    CHECK(port == 8020 && st == SOCK_STREAM, "the caller's port and socket type are kept (TCP channel)");
    look("127.0.0.3", "8010", AF_UNSPEC, SOCK_DGRAM, ip, sizeof ip, &port, &st);
    CHECK(strcmp(ip, on ? "127.0.0.1" : "127.0.0.3") == 0 && port == 8010 && st == SOCK_DGRAM,
          "a UDP register gets the same address, as a datagram socket");
    CHECK(session_host_hits() == (on ? 2u : 0u) && session_host_lookups() == (on ? 0u : 2u),
          "counted: answered from the learned address, against real lookups");

    look("127.0.0.4", "8020", AF_UNSPEC, SOCK_STREAM, ip, sizeof ip, &port, &st);
    CHECK(strcmp(ip, "127.0.0.4") == 0, "another name is never answered with the learned address");

    /* The "ipv6-" prefix some channels strip and others keep. */
    session_host_reset();
    session_host_learn("ipv6-127.0.0.3", (const struct sockaddr *)&a1, NULL, 0);
    look("127.0.0.3", "8012", AF_UNSPEC, SOCK_DGRAM, ip, sizeof ip, &port, &st);
    CHECK(strcmp(ip, on ? "127.0.0.1" : "127.0.0.3") == 0,
          "the \"ipv6-\" prefix does not make two names of one host");

    /* The first connection of the session wins. */
    session_host_reset();
    session_host_learn("127.0.0.3", (const struct sockaddr *)&a1, NULL, 0);
    const int l2 = session_host_learn("127.0.0.3", (const struct sockaddr *)&a2, NULL, 0);
    look("127.0.0.3", "8011", AF_UNSPEC, SOCK_STREAM, ip, sizeof ip, &port, &st);
    CHECK(l2 == 0 && strcmp(ip, on ? "127.0.0.1" : "127.0.0.3") == 0,
          "a second connection changes nothing: the first one of the session wins");

    /* A learned address the caller cannot use: the normal lookup. */
    session_host_reset();
    {
        struct sockaddr_in6 six;
        memset(&six, 0, sizeof six);
        six.sin6_family = AF_INET6;
        inet_pton(AF_INET6, "::1", &six.sin6_addr);
        session_host_learn("127.0.0.5", (const struct sockaddr *)&six, NULL, 0);
    }
    look("127.0.0.5", "8013", AF_INET, SOCK_DGRAM, ip, sizeof ip, &port, &st);
    CHECK(strcmp(ip, "127.0.0.5") == 0,
          "an IPv6 address learned, an IPv4-only caller: the normal lookup runs");
    CHECK(session_host_hits() == 0 && session_host_lookups() == 1, "...and it counts as a real lookup");

    /* A new session starts from nothing. */
    session_host_reset();
    look("127.0.0.3", "8011", AF_UNSPEC, SOCK_STREAM, ip, sizeof ip, &port, &st);
    CHECK(strcmp(ip, "127.0.0.3") == 0 && session_host_hits() == 0,
          "after a reset nothing is learned: a VM's address is never carried over");

    CHECK(session_host_learn(NULL, (const struct sockaddr *)&a1, NULL, 0) == 0 &&
          session_host_learn("x", NULL, NULL, 0) == 0,
          "a NULL name or address records nothing");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
