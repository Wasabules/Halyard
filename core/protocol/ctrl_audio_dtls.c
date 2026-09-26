/* ctrl_audio_dtls - see header.
 *
 * I2 2026-05-18 phase 1: DTLS handshake + RX loop, NO Opus decode yet.
 * Phase 2 = wire audio_decoder in so there is actually sound.
 */

#include "ctrl_audio_dtls.h"
#include "../common/log.h"
/* S81 - this module's log category. See shadow/journal.h: the category is
 * declared here, never inferred from the text of the messages. */
#define adlog(...) JOURNAL_INFO_(JOURNAL_CAT_AUDIO, __VA_ARGS__)
#define addbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_AUDIO, __VA_ARGS__)

#include "../services/sockets_compat.h"
#include "session_host.h"   /* DNS1: one lookup of the VM name per session */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifndef _WIN32
#include <netdb.h>
#include <unistd.h>
#endif

#include <wolfssl/options.h>
#include <wolfssl/wolfio.h>   /* A1: EmbedReceiveFrom/EmbedSendTo for the DTLS trace */
#include <wolfssl/ssl.h>

#define alog(fmt, ...) adlog("[audio-dtls] " fmt, ##__VA_ARGS__)

struct ctrl_audio_dtls {
    int                 sock;
    WOLFSSL_CTX        *ctx;
    WOLFSSL            *ssl;
    pthread_t           rx_thread;
    volatile bool       abort_flag;
    volatile bool       rx_running;
    ctrl_audio_dtls_cb  cb;
    void               *user;
    ctrl_audio_dtls_stats_t stats;
    pthread_mutex_t     stats_mtx;
};

static void *audio_rx_thread(void *arg) {
    ctrl_audio_dtls_t *c = (ctrl_audio_dtls_t *)arg;
    uint8_t buf[2048];
    alog("rx_thread started");
    int idle_ticks = 0;
    while (!c->abort_flag) {
        int n = wolfSSL_read(c->ssl, buf, sizeof(buf));
        if (n <= 0) {
            int err = wolfSSL_get_error(c->ssl, n);
            int se = shadow_sock_errno();
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE
                || se == EAGAIN || se == EWOULDBLOCK) {
                idle_ticks++;
                if (idle_ticks % 50 == 0) {
                    alog("idle %ds (no audio packets)", idle_ticks / 10);
                }
                continue;
            }
            alog("wolfSSL_read returned %d (err=%d sockerr=%d) — closing", n, err, se);
            break;
        }
        idle_ticks = 0;
        pthread_mutex_lock(&c->stats_mtx);
        c->stats.bytes_recv += (uint32_t)n;
        c->stats.frames_recv++;
        pthread_mutex_unlock(&c->stats_mtx);
        static int g_rd_log = 0;
        if (g_rd_log < 5) {
            alog("RX frame %d B (Opus payload)", n);
            g_rd_log++;
        }
        if (c->cb) {
            /* Phase 2: hand this to audio_decoder; no rtp_ts (raw Opus). */
            c->cb(buf, (size_t)n, 0, c->user);
        }
    }
    c->rx_running = false;
    alog("rx_thread exited");
    return NULL;
}

/* A1 2026-08-21 - DTLS I/O trace callbacks. Delegate to wolfSSL's
 * Embed*From/To and log the record size plus its header. DTLS landmarks:
 *   byte0: 0x16=handshake 0x14=CCS 0x15=alert 0x17=appdata
 *   byte1-2: version (fe ff = DTLS1.0, fe fd = DTLS1.2)
 *   byte13: handshake type (01=ClientHello 02=ServerHello 03=HelloVerifyReq
 *           0b=Certificate 0c=ServerKeyExchange 0e=ServerHelloDone
 *           10=ClientKeyExchange 14=Finished) */
static void a1_log_record(const char *dir, const char *buf, int rc) {
    if (rc <= 0) { alog("A1 %s rc=%d (=<0 : rien passe)", dir, rc); return; }
    const unsigned char *b = (const unsigned char *)buf;
    int n = rc < 16 ? rc : 16;
    char hex[64] = {0};
    for (int i = 0; i < n; i++) snprintf(hex + i * 3, 4, "%02x ", b[i]);
    int hs = (rc > 13) ? b[13] : -1;
    alog("A1 %s %d B | %s| type=0x%02x ver=%02x%02x hs=0x%02x",
         dir, rc, hex, b[0], rc > 2 ? b[1] : 0, rc > 2 ? b[2] : 0, hs);
}

static int dtls_trace_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    int rc = EmbedReceiveFrom(ssl, buf, sz, ctx);
    a1_log_record("RECV", buf, rc);
    return rc;
}

static int dtls_trace_send(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    int rc = EmbedSendTo(ssl, buf, sz, ctx);
    a1_log_record("SEND", buf, rc);
    return rc;
}

/* S11 2026-08-21 - the DTLS plumbing is pulled out here so it can be shared.
 * The 2026-08-21 capture proves `:base+12` carries the INPUT channel
 * (DTLS/UDP), not audio: the SSL object fed by this socket carried 1833 moves
 * of 144 B, 19 clicks of 152 B and 1212 server replies of 104 B (KB.md §3.21).
 * Rather than write a second DTLS client in ctrl_input_tcp.c, we expose this
 * one, which already carries fixes A2 (set_dtls_fd_connected, without which
 * the handshake fails 100% of the time for want of WOLFSSL_IPV6), D13
 * (set_peer) and A3 (using_nonblock, so the thread sees abort_flag within
 * 100 ms).
 * Returns the socket, the CTX and the session; the caller owns them. */
int ctrl_dtls_connect_raw(const char *vm_host, uint16_t port,
                           int *out_sock, WOLFSSL_CTX **out_ctx, WOLFSSL **out_ssl) {
    if (!vm_host || !out_sock || !out_ctx || !out_ssl) return -1;
    *out_sock = -1; *out_ctx = NULL; *out_ssl = NULL;

    const char *connect_host = vm_host;
    if (strncmp(vm_host, "ipv6-", 5) == 0) connect_host = vm_host + 5;

    alog("opening UDP+DTLS %s:%u", connect_host, port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);
    int gai = session_getaddrinfo(connect_host, port_str, &hints, &res);   /* DNS1 */
    if (gai != 0 || !res) {
        alog("getaddrinfo FAIL gai=%d", gai);
        return -1;
    }
    int sock = -1;
    /* D13 2026-08-21 - keep the peer address for wolfSSL_dtls_set_peer.
     * `res` is freed right after the loop, so without this copy the address is
     * gone - which is exactly why the call was missing. */
    struct sockaddr_storage peer_ss;
    socklen_t peer_len = 0;
    memset(&peer_ss, 0, sizeof(peer_ss));
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, SOCK_DGRAM, 0);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            /* 2026-05-18 PERF - symmetry with the video UDP socket (N56):
             * raise the audio SO_RCVBUF to 512 KB. Audio bursts (one Opus
             * frame = one packet every 20 ms) can saturate the 208 KB default
             * if the receive thread stalls even briefly -> ALSA underrun ->
             * choppy sound. 512 KB is ~5 s of audio buffer. */
            int rcvbuf = 512 * 1024;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
            alog("UDP connected (family=%d) SO_RCVBUF=%dKB", rp->ai_family, rcvbuf / 1024);
            if (rp->ai_addrlen <= sizeof(peer_ss)) {          /* D13 */
                memcpy(&peer_ss, rp->ai_addr, rp->ai_addrlen);
                peer_len = (socklen_t)rp->ai_addrlen;
            }
            break;
        }
        shadow_closesocket(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) {
        alog("UDP connect FAIL errno=%d", shadow_sock_errno());
        return -1;
    }

    /* DTLS 1.2 client. wolfSSL 5.x renames the method depending on the
     * WOLFSSL_DTLS build flag: historically wolfDTLSv1_2_client_method; some
     * builds only expose wolfDTLSv1_2_client_method_ex or
     * wolfDTLS_client_method. Fall back safely. */
#if defined(HAVE_WOLFSSL_DTLS) || defined(WOLFSSL_DTLS)
    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfDTLSv1_2_client_method());
#else
    /* wolfSSL built without DTLS -> DTLS audio disabled. */
    alog("DTLS not built into wolfSSL — audio channel disabled");
    shadow_closesocket(sock);
    return -1;
    WOLFSSL_CTX *ctx = NULL;
#endif
    if (!ctx) {
        alog("CTX_new(DTLSv1_2_client) FAIL");
        shadow_closesocket(sock);
        return -1;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);

    /* === A1 2026-08-21 - trace the DTLS I/O ===
     * The audio handshake failed in 100% of sessions (err=-308) and nobody had
     * ever looked at OUR wire: the 2026-05-23 RE documents the desktop
     * sequence byte by byte (ClientHello 211B -> HelloVerifyRequest 60B ->
     * ClientHello+cookie 243B -> ServerHello 106B -> ...) and concluded "code
     * ready, err=-308 = plain timeout". Instrument it to find where it really
     * breaks: do we even emit the ClientHello? does the server answer?
     * SHADOW_AUDIO_DTLS_TRACE=1 turns it on. */
    {
        static int g_trace = -1;
        if (g_trace < 0) {
            const char *e = getenv("SHADOW_AUDIO_DTLS_TRACE");
            g_trace = e ? atoi(e) : 0;
        }
        if (g_trace) {
            wolfSSL_CTX_SetIORecv(ctx, dtls_trace_recv);
            wolfSSL_CTX_SetIOSend(ctx, dtls_trace_send);
            alog("A1 trace DTLS activee");
        }
    }

    /* RE2 2026-05-18 - cipher list widened to match the desktop OpenSSL one.
     * The ShadowPCDisplay strings dump shows ECDHE_ECDSA and DHE_RSA variants
     * in its cipher list. Our previous version was missing ECDSA AES256,
     * ECDSA CHACHA20 and the DHE-RSA fallbacks. The server can silently refuse
     * a ClientHello if no acceptable cipher is offered high enough up. */
    wolfSSL_CTX_set_cipher_list(ctx,
        /* ECDSA variants (the desktop's priority order) */
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        /* RSA variants */
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-CHACHA20-POLY1305:"
        /* DHE fallback */
        "DHE-RSA-AES256-GCM-SHA384:"
        "DHE-RSA-CHACHA20-POLY1305");

    WOLFSSL *ssl = wolfSSL_new(ctx);
    if (!ssl) {
        alog("SSL_new FAIL");
        wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        return -1;
    }
    /* === A2 2026-08-21 - ROOT CAUSE of "no sound" ===
     * The A1 trace shows our VERY FIRST send failing: `SEND rc=-174`, i.e.
     * NOT_COMPILED_IN - the ClientHello never leaves. So it was neither a
     * timeout nor the server (the 2026-05-23 analysis concluded "code ready,
     * err=-308 = plain timeout": wrong).
     *
     * Origin, in `src/wolfio.c::EmbedSendTo`:
     *     else if (!dtlsCtx->connected) {
     *         peer = dtlsCtx->peer.sa;
     *   #ifndef WOLFSSL_IPV6
     *         if (PeerIsIpv6(peer, peerSz)) return NOT_COMPILED_IN;
     *   #endif
     *     }
     * The vendored wolfSSL is built WITHOUT `WOLFSSL_IPV6`, and we hand it an
     * IPv6 peer (family=10, peer_len=28) - hence the refusal.
     *
     * Fix: our socket is already `connect()`ed (see the getaddrinfo loop
     * above), so we declare it as such to wolfSSL. `connected = 1` routes
     * through send()/recv() instead of sendto()/recvfrom(), which bypasses the
     * whole peer path - and therefore the IPv6 guard. No wolfSSL rebuild
     * needed, and we keep the IPv6 used everywhere else in the stack.
     * SHADOW_AUDIO_DTLS_CONNECTED=0 goes back to wolfSSL_set_fd(). */
    static int g_dtls_connected = -1;
    {
        if (g_dtls_connected < 0) {
            const char *e = getenv("SHADOW_AUDIO_DTLS_CONNECTED");
            g_dtls_connected = e ? atoi(e) : 1;
        }
        if (g_dtls_connected) {
            int fr = wolfSSL_set_dtls_fd_connected(ssl, sock);
            alog("A2 set_dtls_fd_connected rc=%d (socket already connect()ed)", fr);
        } else {
            wolfSSL_set_fd(ssl, sock);
        }
    }

    /* RE2 2026-05-18 - 5 s timeout (was 2 s, which can be too short for the
     * double DTLS handshake with the HelloVerifyRequest cookie). DTLS 1.2
     * expects: Client -> ClientHello -> Server -> HelloVerifyRequest(cookie)
     * -> Client -> ClientHello(cookie) -> Server -> ServerHello+...
     * If recv_timeout < the total round trip -> SOCKET_ERROR_E -308. */
#ifdef _WIN32
    DWORD tv_ms = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
#else
    struct timeval tv = {5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    /* RE2 2026-05-18 - wolfSSL_dtls_set_peer is required for UDP DTLS: it sets
     * the peer address, without which wolfSSL does not know where to send the
     * cookie back once it has received a HelloVerifyRequest. See RFC 6347,
     * §4.2.
     *
     * D13 2026-08-21 - the call was ABSENT: the comment above declared it
     * required, then a second comment rationalised doing without it thanks to
     * connect(). But wolfSSL keeps its own DTLS state, and the peer address
     * feeds its cookie state machine there, not just kernel routing. Measured:
     * 5 sessions out of 5 failed the handshake with errno=0 - so NOT an
     * ECONNREFUSED and not a socket timeout, the failure is internal to
     * wolfSSL, which fits an incomplete DTLS state.
     * SHADOW_AUDIO_DTLS_SETPEER=0 restores the previous behaviour. */
    {
        static int g_setpeer = -1;
        if (g_setpeer < 0) {
            const char *e = getenv("SHADOW_AUDIO_DTLS_SETPEER");
            g_setpeer = e ? atoi(e) : 1;
        }
        /* Review 2026-08-21 - also gate this on "connected" mode.
         * wolfSSL_dtls_set_peer sets userSet=1; in connected mode we no longer
         * take the peer path, but with SHADOW_AUDIO_DTLS_CONNECTED=0 (A2's
         * documented revert) the call reintroduced an IPv6 peer, and with it
         * the NOT_COMPILED_IN we had just worked around: the revert did not
         * actually go back to the earlier behaviour. */
        if (g_setpeer && peer_len > 0 && !g_dtls_connected) {
            int pr = wolfSSL_dtls_set_peer(ssl, &peer_ss, peer_len);
            alog("D13 wolfSSL_dtls_set_peer rc=%d (peer_len=%u)", pr,
                 (unsigned)peer_len);
        } else if (g_setpeer) {
            alog("D13 peer addr indisponible — set_peer saute");
        }
    }

    int rc = wolfSSL_connect(ssl);
    /* D12 2026-08-21 - capture errno IMMEDIATELY: wolfSSL_get_error() and
     * wolfSSL_ERR_error_string() make calls that overwrite it. The first
     * version of this diagnostic read it afterwards, and so wrongly reported
     * sockerr=0. */
    int sock_err_now = shadow_sock_errno();
    if (rc != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(ssl, rc);
        char errbuf[80] = {0};
        wolfSSL_ERR_error_string((unsigned long)err, errbuf);
        /* D12 2026-08-21 - the message did not say enough: err=-308
         * (SOCKET_ERROR_E) on a *connected* UDP socket usually means an
         * ECONNREFUSED reported by ICMP, i.e. "nobody is listening on
         * :base+12", which has nothing to do with DTLS. Log errno to tell the
         * two apart. Measured: 5 sessions out of 5 failed = no sound
         * (KB.md §3.17). */
        alog("DTLS handshake FAIL rc=%d err=%d (%s) sockerr=%d (111=ECONNREFUSED "
             "=> port closed on the server side, 110=ETIMEDOUT => no answer)",
             rc, err, errbuf, sock_err_now);
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        return -1;
    }
    alog("DTLS handshake OK cipher=%s version=%s",
         wolfSSL_get_cipher(ssl), wolfSSL_get_version(ssl));

    /* A3 2026-08-21 - RESPONSIVENESS TO SHUTDOWN. The comment just below
     * announces "non-blocking", but all we set was SO_RCVTIMEO: on the wolfSSL
     * side a socket timeout in DTLS triggers its internal retransmission logic
     * and `wolfSSL_read` does NOT hand control back. Measured result: the
     * rx_thread never exited, `rx_thread exited` was missing from the log, and
     * the process hung on close (151 s wall clock for --duration=30, killed by
     * the timeout).
     *
     * This bug had been masked until now: the handshake failed (see A2), so
     * the thread never existed. Fixing that revealed it.
     *
     * This is KB.md §7.3: every long-lived thread must see abort_flag within
     * 100 ms, otherwise HOS leaks handles and the console has to be rebooted.
     * `wolfSSL_dtls_set_using_nonblock` surfaces WANT_READ instead of looping
     * internally; combined with the 100 ms SO_RCVTIMEO already set below, the
     * loop revisits the abort_flag test every 100 ms without spinning. */
    wolfSSL_dtls_set_using_nonblock(ssl, 1);

    /* Switch the socket to non-blocking for rx_thread. */
#ifdef _WIN32
    DWORD tv_rx_ms = 100;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_rx_ms, sizeof(tv_rx_ms));
#else
    {
        struct timeval tv_rx = {0, 100 * 1000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rx, sizeof(tv_rx));
    }
#endif

    *out_sock = sock; *out_ctx = ctx; *out_ssl = ssl;
    return 0;
}

int ctrl_audio_dtls_open(ctrl_audio_dtls_t **out,
                          const char *vm_host, uint16_t base_port,
                          ctrl_audio_dtls_cb cb, void *user) {
    if (!out || !vm_host) return -1;
    *out = NULL;
    int sock = -1; WOLFSSL_CTX *ctx = NULL; WOLFSSL *ssl = NULL;
    if (ctrl_dtls_connect_raw(vm_host, (uint16_t)(base_port + 12),
                               &sock, &ctx, &ssl) != 0)
        return -1;

    ctrl_audio_dtls_t *c = (ctrl_audio_dtls_t *)calloc(1, sizeof(*c));
    if (!c) {
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        return -1;
    }
    c->sock = sock;
    c->ctx  = ctx;
    c->ssl  = ssl;
    c->cb   = cb;
    c->user = user;
    c->stats.handshake_ok = true;
    c->abort_flag = false;
    c->rx_running = true;
    pthread_mutex_init(&c->stats_mtx, NULL);

    if (pthread_create(&c->rx_thread, NULL, audio_rx_thread, c) != 0) {
        alog("pthread_create FAIL");
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        free(c);
        return -1;
    }

    *out = c;
    return 0;
}

void ctrl_audio_dtls_close(ctrl_audio_dtls_t *c) {
    if (!c) return;
    c->abort_flag = true;
    if (c->rx_thread) {
        pthread_join(c->rx_thread, NULL);
        c->rx_thread = 0;
    }
    if (c->ssl) {
        wolfSSL_shutdown(c->ssl);
        wolfSSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ctx) {
        wolfSSL_CTX_free(c->ctx);
        c->ctx = NULL;
    }
    if (c->sock >= 0) {
        shadow_closesocket(c->sock);
        c->sock = -1;
    }
    pthread_mutex_destroy(&c->stats_mtx);
    free(c);
}

void ctrl_audio_dtls_get_stats(const ctrl_audio_dtls_t *c, ctrl_audio_dtls_stats_t *out) {
    if (!c || !out) return;
    pthread_mutex_lock((pthread_mutex_t *)&c->stats_mtx);
    *out = c->stats;
    pthread_mutex_unlock((pthread_mutex_t *)&c->stats_mtx);
}
