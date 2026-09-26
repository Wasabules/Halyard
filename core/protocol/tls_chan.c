/* tls_chan.c - see tls_chan.h. Code moved out of ctrl_tcp.c, which held the
 * only version that bounded the connect with a timeout. */
#include "tls_chan.h"

#include "../services/sockets_compat.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "../common/log.h"
#include "session_host.h"   /* DNS1: one lookup of the VM name per session */

/* S81 - this module's log category. See shadow/journal.h: it is DECLARED
 * here, never inferred from the text of the messages. */
#define tclog(...) JOURNAL_INFO_(JOURNAL_CAT_NETWORK, __VA_ARGS__)
#define tcdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_NETWORK, __VA_ARGS__)

#define clog(tag, fmt, ...) tclog("%s: " fmt, (tag), ##__VA_ARGS__)

/* Cipher list shared by the TCP channels. Identical in the three original
 * modules up to ordering - so it can be factored out without changing the TLS
 * fingerprint the server sees. No SNI, no ALPN, no certificate validation:
 * that is what the official client does. */
static const char TLS_CIPHERS[] =
    "TLS13-AES128-GCM-SHA256:"
    "TLS13-AES256-GCM-SHA384:"
    "TLS13-CHACHA20-POLY1305-SHA256:"
    "ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-RSA-AES256-GCM-SHA384";

/* === S57b 2026-09-14 - AN ABORT MUST BE SEEN IN 100 ms, NOT IN 5 s ====
 *
 * `select` used to be given the WHOLE timeout, so a thread inside a 5 s
 * connect could not notice that the session had been told to stop. Measured
 * on console: the stop flag went up at t=556.741 and this call returned at
 * t=559.992 - 3.25 s late, with the user already back on the menu and the
 * teardown running around it. That breaks this repository's first rule for a
 * long-lived thread (poll the abort flag at 100 ms or finer), and a teardown
 * racing a live connect is how handles leak on these consoles.
 *
 * `abort` may be NULL, which is exactly the behaviour of before. */
int tls_chan_tcp_connect_abortable(const char *host, int port, int timeout_ms,
                                   const char *tag, const volatile int *abort)
{
    if (!host) return -1;
    if (!tag) tag = "tls_chan";

    /* AF_UNSPEC: the official client goes over IPv6 on desktop (tcpdump
     * 2026-05-06 towards 2a0a:e805:...), while the Switch only has IPv4. We
     * try every address returned. */
    struct addrinfo hints = {0}, *res = NULL, *rp = NULL;
    /* SHADOW_FORCE_IPV4=1: reproduce the Switch's network path, which has no
     * usable IPv6, on a desktop that does have one. */
    {
        static int v4 = -1;
        if (v4 < 0) {
            const char *e = getenv("SHADOW_FORCE_IPV4");
#ifdef __SWITCH__
            /* === S108 2026-08-29 - ON CONSOLE, IPv6 ONLY COSTS TIME ===
             *
             * `CLAUDE.md` has said it for months - force IPv4 everywhere, the
             * HOS resolver is flaky on AAAA - and this path did not apply it:
             * it went out AF_UNSPEC on both platforms. The comment right above
             * even acknowledged that "the Switch only has IPv4" without drawing
             * the consequence.
             *
             * What that costs, measured on a session where the VM was slow to
             * answer: five seconds of guard timeout per IPv6 attempt, and one
             * attempt per retry round. Of the thirty-four seconds of waiting
             * observed before the picture arrived, HALF was spent dialling
             * addresses the console cannot reach.
             *
             * The default therefore becomes IPv4 on console, and stays
             * AF_UNSPEC on desktop - where IPv6 works and where the official
             * client uses it (tcpdump 2026-05-06). `SHADOW_FORCE_IPV4=0`
             * restores the old behaviour if you want to measure. */
            v4 = e ? atoi(e) : 1;
#else
            v4 = e ? atoi(e) : 0;
#endif
        }
        hints.ai_family = v4 ? AF_INET : AF_UNSPEC;
    }
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int gai = session_getaddrinfo(host, port_str, &hints, &res);   /* DNS1 */
    if (gai != 0 || !res) {
        clog(tag, "getaddrinfo(%s:%d) FAIL gai=%d", host, port, gai);
        return -1;
    }

    int sock = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        char ip[INET6_ADDRSTRLEN] = {0};
        if (rp->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)rp->ai_addr)->sin_addr,
                      ip, sizeof(ip));
        } else if (rp->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)rp->ai_addr)->sin6_addr,
                      ip, sizeof(ip));
        }
        clog(tag, "try %s [%s]:%d", rp->ai_family == AF_INET ? "v4" : "v6", ip, port);

        sock = socket(rp->ai_family, SOCK_STREAM, 0);
        if (sock < 0) {
            clog(tag, "socket FAIL errno=%d", shadow_sock_errno());
            continue;
        }

        /* PERF1 2026-05-18 - Nagle off. These channels send small messages at
         * a high rate (gE feedback on the control channel, input events at
         * 30-60 Hz, keyframe retransmission requests on video); Nagle buffers
         * them for up to 40 ms, which you feel directly in your hand on
         * input. */
        {
            int one = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        }

        int cr, cerr;
        if (timeout_ms > 0) {
            /* Non-blocking connect + select: without it an unreachable server
             * blocks the thread forever. That was exactly the case for vst and
             * input-tcp before this code was shared. */
            shadow_set_nonblocking(sock, 1);
            cr = connect(sock, rp->ai_addr, rp->ai_addrlen);
            cerr = shadow_sock_errno();
            if (cr < 0 && cerr != EINPROGRESS && cerr != EWOULDBLOCK) {
                clog(tag, "connect FAIL errno=%d", cerr);
                shadow_closesocket(sock); sock = -1; continue;
            }
            if (cr < 0) {
                int sr = 0, waited = 0;
                while (waited < timeout_ms) {
                    if (abort && *abort) {
                        clog(tag, "connect ABANDONNE apres %d ms (arret demande)",
                             waited);
                        shadow_closesocket(sock);
                        freeaddrinfo(res);
                        return -1;
                    }
                    const int slice = (timeout_ms - waited > 100)
                                    ? 100 : (timeout_ms - waited);
                    fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
                    struct timeval tv = { slice / 1000, (slice % 1000) * 1000 };
                    sr = select(sock + 1, NULL, &wfds, NULL, &tv);
                    if (sr != 0) break;          /* ready, or an error */
                    waited += slice;
                }
                if (sr <= 0) {
                    clog(tag, "connect TIMEOUT %dms on %s", timeout_ms, ip);
                    shadow_closesocket(sock); sock = -1; continue;
                }
                int soerr = 0; socklen_t sl = sizeof(soerr);
                /* Winsock wants a char* for optval; POSIX accepts void*. */
                getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
                if (soerr != 0) {
                    clog(tag, "connect SO_ERROR=%d on %s", soerr, ip);
                    shadow_closesocket(sock); sock = -1; continue;
                }
            }
            shadow_set_nonblocking(sock, 0);   /* TLS veut du bloquant */
        } else {
            cr = connect(sock, rp->ai_addr, (int)rp->ai_addrlen);
            if (cr != 0) {
                clog(tag, "connect FAIL errno=%d", shadow_sock_errno());
                shadow_closesocket(sock); sock = -1; continue;
            }
        }
        clog(tag, "connected %s [%s]:%d NODELAY=on",
             rp->ai_family == AF_INET ? "v4" : "v6", ip, port);
        /* DNS1 2026-09-11 - the first channel of a session to connect (the
         * control channel) gives the address every later lookup of this name
         * is answered from, without DNS. See session_host.h. */
        if (session_host_learn(host, rp->ai_addr, NULL, 0))
            clog(tag, "[DNS1] VM address kept for the session: %s - "
                      "the following channels no longer resolve it", ip);
        break;
    }
    freeaddrinfo(res);
    return sock;
}

bool tls_chan_handshake(tls_chan *out, int sock, const char *tag)
{
    if (!out || sock < 0) return false;
    if (!tag) tag = "tls_chan";
    out->sock = sock; out->ctx = NULL; out->ssl = NULL;

    out->ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!out->ctx) { clog(tag, "CTX_new FAIL"); return false; }
    wolfSSL_CTX_set_verify(out->ctx, WOLFSSL_VERIFY_NONE, NULL);
    if (wolfSSL_CTX_set_cipher_list(out->ctx, TLS_CIPHERS) != WOLFSSL_SUCCESS)
        clog(tag, "WARN set_cipher_list FAIL - the default list is used");

    out->ssl = wolfSSL_new(out->ctx);
    if (!out->ssl) {
        clog(tag, "SSL_new FAIL");
        wolfSSL_CTX_free(out->ctx); out->ctx = NULL;
        return false;
    }
    wolfSSL_set_fd(out->ssl, sock);

    int rc = wolfSSL_connect(out->ssl);
    if (rc != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(out->ssl, rc);
        char errbuf[80] = {0};
        wolfSSL_ERR_error_string((unsigned long)err, errbuf);
        clog(tag, "wolfSSL_connect FAIL rc=%d err=%d (%s)", rc, err, errbuf);
        wolfSSL_free(out->ssl);         out->ssl = NULL;
        wolfSSL_CTX_free(out->ctx);     out->ctx = NULL;
        return false;                    /* the socket stays with the caller */
    }
    clog(tag, "TLS up (%s)", wolfSSL_get_version(out->ssl));
    return true;
}

int tls_chan_tcp_connect(const char *host, int port, int timeout_ms, const char *tag)
{
    return tls_chan_tcp_connect_abortable(host, port, timeout_ms, tag, NULL);
}

bool tls_chan_open(tls_chan *out, const char *host, int port,
                   int timeout_ms, const char *tag)
{
    if (!out) return false;
    out->sock = -1; out->ctx = NULL; out->ssl = NULL;

    int sock = tls_chan_tcp_connect(host, port, timeout_ms, tag);
    if (sock < 0) return false;
    if (!tls_chan_handshake(out, sock, tag)) {
        shadow_closesocket(sock);
        out->sock = -1;
        return false;
    }
    return true;
}

void tls_chan_close(tls_chan *c)
{
    if (!c) return;
    if (c->ssl) { wolfSSL_shutdown(c->ssl); wolfSSL_free(c->ssl); c->ssl = NULL; }
    if (c->ctx) { wolfSSL_CTX_free(c->ctx); c->ctx = NULL; }
    if (c->sock >= 0) { shadow_closesocket(c->sock); c->sock = -1; }
}
