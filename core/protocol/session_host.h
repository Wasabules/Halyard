/* session_host.h - resolve the VM's name ONCE per session (DNS1, 2026-09-11).
 *
 * Every channel looked the same host name up for itself: the control channel,
 * then each UDP register (video, audio, gamepad), the cursor channel, input.
 * Measured on desktop (CONC-1 A/B, 2026-09-11): in one session the cursor
 * channel's lookup of a name the control channel had resolved 0.6 s earlier
 * took 1.21 s - and the video socket, registered just before, collected 1.43 s
 * of pictures that reached the decoder in one burst (57 dropped at depth 8).
 *
 * Now the numeric address the first channel of the session actually CONNECTED
 * to (the control channel, through tls_chan) is learned, and every later
 * lookup of the same name is answered from it: a numeric lookup
 * (AI_NUMERICHOST), which never reaches a resolver. When that answer does not
 * fit the caller (an IPv4-only caller and an IPv6 address), the normal lookup
 * runs, as before. Reset at every session start and never carried over: a VM's
 * address can change between sessions. The bootstrap order is untouched - only
 * where each channel's address comes from.
 *
 * SHADOW_RESOLVE_ONCE=0 restores one real lookup per channel.
 * SHADOW_DIAG_DNS_DELAY_MS=<ms> (diagnostic, off): delays every REAL lookup, to
 * replay a slow resolver on demand. */
#ifndef SHADOW_SESSION_HOST_H
#define SHADOW_SESSION_HOST_H

#include <stddef.h>
#include "../services/sockets_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A new session: nothing learned, counters at zero. */
void session_host_reset(void);

/* Records `host`'s numeric address from `sa`, the address a socket just
 * connected to. The first call of a session wins. Returns 1 when it recorded
 * (and copies the numeric address into `ip_out` when given), 0 otherwise. */
int session_host_learn(const char *host, const struct sockaddr *sa,
                       char *ip_out, size_t ip_cap);

/* Drop-in for getaddrinfo(): same arguments, same return codes, and the result
 * is released with freeaddrinfo() as usual. */
int session_getaddrinfo(const char *host, const char *port,
                        const struct addrinfo *hints, struct addrinfo **res);

/* This session: lookups answered from the learned address / real lookups. */
unsigned session_host_hits(void);
unsigned session_host_lookups(void);

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_SESSION_HOST_H */
