/* session_host.c - see session_host.h (DNS1, 2026-09-11). */
#include "session_host.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef AI_NUMERICHOST
#define AI_NUMERICHOST 0   /* a numeric string is parsed locally either way */
#endif

/* Session state, reset by session_host_reset() - never a function static. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char     g_host[256];   /* the name, without an "ipv6-" prefix */
static char     g_ip[64];      /* its numeric address; "" = nothing learned */
static unsigned g_hits;        /* lookups answered from g_ip */
static unsigned g_lookups;     /* real lookups */

/* Experiment switches, read once: process configuration, not session state. */
static int resolve_once_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("SHADOW_RESOLVE_ONCE");
        v = e ? atoi(e) : 1;
    }
    return v;
}

static int diag_delay_ms(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("SHADOW_DIAG_DNS_DELAY_MS");
        v = e ? atoi(e) : 0;
        if (v < 0) v = 0;
    }
    return v;
}

/* Some channels strip the launcher's "ipv6-" prefix and others keep it: one
 * host, one name. */
static const char *bare(const char *h)
{
    return (h && strncmp(h, "ipv6-", 5) == 0) ? h + 5 : h;
}

void session_host_reset(void)
{
    pthread_mutex_lock(&g_mtx);
    g_host[0] = 0;
    g_ip[0]   = 0;
    g_hits = g_lookups = 0;
    pthread_mutex_unlock(&g_mtx);
}

int session_host_learn(const char *host, const struct sockaddr *sa,
                       char *ip_out, size_t ip_cap)
{
    if (!host || !sa || !resolve_once_on()) return 0;
    char ip[64] = "";
    if (sa->sa_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr, ip, sizeof ip);
    else if (sa->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)sa)->sin6_addr, ip, sizeof ip);
    const char *h = bare(host);
    if (!ip[0] || strlen(h) >= sizeof g_host) return 0;

    int learned = 0;
    pthread_mutex_lock(&g_mtx);
    if (!g_ip[0]) {
        snprintf(g_host, sizeof g_host, "%s", h);
        snprintf(g_ip, sizeof g_ip, "%s", ip);
        learned = 1;
    }
    pthread_mutex_unlock(&g_mtx);
    if (learned && ip_out && ip_cap) snprintf(ip_out, ip_cap, "%s", ip);
    return learned;
}

int session_getaddrinfo(const char *host, const char *port,
                        const struct addrinfo *hints, struct addrinfo **res)
{
    if (host && resolve_once_on()) {
        char ip[64] = "";
        pthread_mutex_lock(&g_mtx);
        if (g_ip[0] && strcmp(bare(host), g_host) == 0)
            snprintf(ip, sizeof ip, "%s", g_ip);
        pthread_mutex_unlock(&g_mtx);
        if (ip[0]) {
            struct addrinfo h;
            memset(&h, 0, sizeof h);
            if (hints) h = *hints;
            h.ai_flags |= AI_NUMERICHOST;
            if (getaddrinfo(ip, port, &h, res) == 0 && *res) {
                pthread_mutex_lock(&g_mtx);
                g_hits++;
                pthread_mutex_unlock(&g_mtx);
                return 0;
            }
            /* The learned address does not fit this caller (another family):
             * the normal lookup below, as before. */
        }
    }
    const int d = diag_delay_ms();
    if (d > 0) {
        struct timespec ts = { d / 1000, (long)(d % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&g_mtx);
    g_lookups++;
    pthread_mutex_unlock(&g_mtx);
    return getaddrinfo(host, port, hints, res);
}

unsigned session_host_hits(void)
{
    pthread_mutex_lock(&g_mtx);
    const unsigned v = g_hits;
    pthread_mutex_unlock(&g_mtx);
    return v;
}

unsigned session_host_lookups(void)
{
    pthread_mutex_lock(&g_mtx);
    const unsigned v = g_lookups;
    pthread_mutex_unlock(&g_mtx);
    return v;
}
