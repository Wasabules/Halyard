/* authz.h - who is allowed to drive this console, decided by its owner.
 *
 * WHY. Until now the only protection on the [devlink] channel was the PRESENCE
 * of `logsink.txt`: drop that file and you can press buttons, read the screen,
 * change experiment toggles and relaunch the application. That is a reasonable
 * guard on a bench and a poor one on a console someone else can reach - and it
 * asks nothing of the person actually holding the device.
 *
 * So: the Android model. The first time a machine opens the channel, the console
 * ASKS, naming it, and nothing passes until someone standing in front of the
 * screen says yes. Neither the log nor the commands - refusing commands while
 * still mirroring the log would protect the console and leak the session.
 *
 * THE IDENTITY IS THE ADDRESS WE DIALLED, not a name the other end claims. The
 * application connects OUTWARD to what `logsink.txt` names, so `host:port` is
 * the one identity we know first-hand. A machine could announce itself as
 * anything; keying the allow-list on its own claim would make the prompt a
 * formality.
 *
 * PURE - no I/O, no clock, no allocation. The persistence and the prompt live in
 * authz.cpp; what is decided lives here and is checked by tests/test_authz.c.
 *
 * Created 2026-09-12 (AUTH-1).
 */
#ifndef DEVLINK_AUTHZ_H
#define DEVLINK_AUTHZ_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "255.255.255.255:65535" is 21; a host name can be longer. 80 is ample and
 * still far from a landing buffer - an over-long peer is REFUSED, like every
 * over-long token in this channel. */
#define AUTHZ_PEER_MAX 80

typedef enum {
    AUTHZ_UNKNOWN = 0,   /* never seen: ask */
    AUTHZ_ALLOWED,       /* the owner said yes */
    AUTHZ_DENIED         /* the owner said no - do not ask again this session */
} authz_verdict_t;

/* Is this a peer string we are willing to store and display?
 *
 * It is read from OUR OWN file, not from the network, so this is not a defence
 * against an attacker - it is a guard against a mangled file producing a prompt
 * that says nothing, or a stored line that cannot be matched back.
 *
 *   - "host:port", both non-empty;
 *   - printable ASCII without space, since the stored form is one peer per line;
 *   - a port made of digits only.
 */
static inline bool authz_peer_ok(const char *p, size_t len)
{
    size_t i, colon = (size_t)-1;

    if (!p || len == 0 || len >= AUTHZ_PEER_MAX) return false;
    for (i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)p[i];
        if (c <= 0x20 || c >= 0x7f) return false;
        if (c == ':') colon = i;          /* the LAST colon: IPv6 has several */
    }
    if (colon == (size_t)-1 || colon == 0 || colon + 1 >= len) return false;
    for (i = colon + 1; i < len; i++)
        if (p[i] < '0' || p[i] > '9') return false;
    return true;
}

/* Looks `peer` up in a stored list, given as the raw file contents.
 *
 * The format is one peer per line; a line starting with '-' is a REFUSAL kept on
 * purpose, so that a machine turned away once is not asked about at every
 * launch. Blank lines and '#' comments are ignored, because a human may well
 * open this file and annotate it.
 *
 * Matching is exact and whole-line: a prefix match would let "192.168.1.1"
 * authorise "192.168.1.10". */
static inline authz_verdict_t authz_lookup(const char *list, size_t list_len,
                                           const char *peer)
{
    const size_t plen = peer ? strlen(peer) : 0;
    size_t i = 0;

    if (!list || plen == 0) return AUTHZ_UNKNOWN;

    while (i < list_len) {
        size_t b = i, e;
        while (i < list_len && list[i] != '\n' && list[i] != '\r') i++;
        e = i;
        while (i < list_len && (list[i] == '\n' || list[i] == '\r')) i++;

        while (b < e && (list[b] == ' ' || list[b] == '\t')) b++;
        while (e > b && (list[e - 1] == ' ' || list[e - 1] == '\t')) e--;
        if (b >= e || list[b] == '#') continue;

        if (list[b] == '-') {
            if ((size_t)(e - b - 1) == plen && memcmp(list + b + 1, peer, plen) == 0)
                return AUTHZ_DENIED;
            continue;
        }
        if ((size_t)(e - b) == plen && memcmp(list + b, peer, plen) == 0)
            return AUTHZ_ALLOWED;
    }
    return AUTHZ_UNKNOWN;
}

#ifdef __cplusplus
}
#endif

#endif /* DEVLINK_AUTHZ_H */
