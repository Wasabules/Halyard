/* tls_chan - opening and closing a Shadow TCP+TLS channel.
 *
 * The protocol's four TCP channels (`+11` control, `+14` input, `+20` video,
 * `+14` ComChan) each opened their connection with the same copied code:
 * AF_UNSPEC resolution, loop over the returned addresses, TCP_NODELAY, a
 * wolfSSL context with no SNI, no ALPN and no certificate validation, the same
 * cipher list, then the same shutdown sequence. A fix therefore only ever
 * reached a quarter of the code - and indeed, only `ctrl_tcp.c` had a TIMEOUT
 * on the connect: the other two blocked forever on an unreachable server.
 *
 * This module carries the mature version, the one from `ctrl_tcp.c`, moved
 * across unchanged. The bootstrap channel is thus untouched by construction;
 * the others gain the timeout they were missing.
 *
 * DELIBERATELY NOT COVERED:
 *   - `ctrl_audio_dtls.c`: DTLS, and its cipher list puts the ECDSA suites
 *     first to match the official client's ordering. That is an intended
 *     difference, not a divergence.
 *   - `ctrl_comchan.c`: sets NO cipher list at all, so aligning it would change
 *     its TLS fingerprint. That channel is off by default (D11) and never
 *     worked end to end; we do not change its behaviour in passing.
 */
#ifndef TLS_CHAN_H
#define TLS_CHAN_H

#include <stdbool.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/* An open channel. The three fields follow the channel's lifetime and are put
 * back to their neutral value by tls_chan_close(). */
typedef struct {
    int          sock;   /* -1 quand ferme */
    WOLFSSL_CTX *ctx;
    WOLFSSL     *ssl;
} tls_chan;

/* Establishes TCP then TLS to host:port.
 * `tag` prefixes the log lines ("ctrl_tcp", "vst", "input-tcp").
 * `timeout_ms` bounds the wait on connect; <= 0 means "no bound", which is the
 * old vst and input-tcp behaviour - avoid it.
 * Returns true and fills `out`; otherwise leaves `out` in the closed state. */
bool tls_chan_open(tls_chan *out, const char *host, int port,
                   int timeout_ms, const char *tag);

/* Establishes TCP only (same resolution, same timeout, same TCP_NODELAY).
 * Returns the socket, or -1. For the rare callers that bring up TLS themselves. */
int tls_chan_tcp_connect(const char *host, int port, int timeout_ms, const char *tag);

/* Same, but abandons the wait within 100 ms of `*abort` becoming true. NULL
 * for `abort` is exactly the call above. */
int tls_chan_tcp_connect_abortable(const char *host, int port, int timeout_ms,
                                   const char *tag, const volatile int *abort);

/* Brings up TLS on an already connected socket. On failure the socket is NOT
 * closed: the caller is the one who opened it. */
bool tls_chan_handshake(tls_chan *out, int sock, const char *tag);

/* Closes cleanly, TLS first then the socket. Idempotent, tolerates NULL. */
void tls_chan_close(tls_chan *c);

#endif /* TLS_CHAN_H */
