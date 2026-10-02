#include "ctrl_tcp.h"
#include "../services/sockets_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>   /* gettimeofday - available on MinGW-w64 AND POSIX */

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include "ctrl_inv.h"      /* SRV7: Request oneof field extraction (pure, tested) */
/* AFTER <pthread.h>: tls_chan.h pulls in wolfssl/options.h, which does
 * `#undef _POSIX_THREADS` - see the note above. */
#include "tls_chan.h"

#include "../common/log.h"
/* S81 - the log category is DECLARED here, not inferred from the text of the
 * messages. `tlog` stays at INFO: the existing calls do not disappear. `tdbg`
 * is there for the high-volume lines, which move over to it one at a time. */
#define tlog(...) JOURNAL_INFO_(JOURNAL_CAT_SESSION, __VA_ARGS__)
#define tdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SESSION, __VA_ARGS__)
struct ctrl_tcp_session {
    int sock;
    WOLFSSL_CTX *ctx;
    WOLFSSL *ssl;
    shadow_cipher *cipher;  /* not owned */
    char host[256];
    char bearer[2048];  /* JWT for Authorization header (set via ctrl_tcp_set_bearer) */
    int instance;       /* API version path /N/ (default 3, set via ctrl_tcp_set_instance) */
    char path[64];      /* endpoint path suffix, default "forward" */
    /* D1 2026-08-20 - peer-close detection. Without it a server FIN
     * (rc=0 / err=-397) left the ctrl_session loop writing forever into a dead
     * socket: 37,880 `err=-397` lines + 20,078 `recv header FAIL` in 9 min,
     * picture frozen and not one intelligible message about it. We latch the
     * state, log it ONCE, and short-circuit the I/O that follows. Pattern taken
     * from ctrl_input_tcp.c, which already did this properly. */
    bool     peer_closed;
    int      close_err;
    unsigned suppressed;
};

#ifndef SOCKET_PEER_CLOSED_E
#  define SOCKET_PEER_CLOSED_E (-397)   /* wolfssl/error-ssl.h */
#endif

/* D1: an error means "peer closed" (fatal, unrecoverable) if TLS received a
 * clean FIN (rc=0), if wolfSSL reports SOCKET_PEER_CLOSED_E, or if the socket
 * underneath is in ECONNRESET/EPIPE. Everything else stays transient. */
/* Review 2026-08-21 - `se` is passed in by the caller, captured IMMEDIATELY
 * after the call that failed. The first version read errno HERE, i.e. AFTER
 * wolfSSL_get_error(): an ECONNRESET left behind by the opening of a side
 * channel (vst/input-tcp/comchan regularly fail with -308 on the same thread,
 * just before the loop) could latch peer_closed wrongly and make us believe in
 * a server close that never happened. That is the trap D12 documented in
 * ctrl_audio_dtls.c without fixing it here.
 *
 * The constants: under Winsock, shadow_sock_errno() returns WSAGetLastError(),
 * which does NOT use the POSIX values - sockets_compat.h only remaps
 * EAGAIN/EWOULDBLOCK/EINPROGRESS. Without the WSAE* codes this branch was dead
 * on the Windows build and the D1 guard never fired there. */
#ifndef WSAECONNRESET
#  define WSAECONNRESET   10054
#endif
#ifndef WSAECONNABORTED
#  define WSAECONNABORTED 10053
#endif

/* Why the last `ctrl_tcp_open_port` returned NULL, for a caller that only sees
 * the NULL. Single-threaded by construction: the control channel is opened
 * from one thread, once per session. */
static const char *g_last_fail = "not attempted";

const char *ctrl_tcp_last_failure(void) { return g_last_fail; }

static bool is_peer_closed(int rc, int err, int se) {
    if (rc == 0) return true;
    if (err == SOCKET_PEER_CLOSED_E) return true;
    return se == ECONNRESET || se == EPIPE
        || se == WSAECONNRESET || se == WSAECONNABORTED;
}

static void mark_peer_closed(ctrl_tcp_session *s, int rc, int err, const char *op) {
    if (!s) return;
    if (!s->peer_closed) {
        s->peer_closed = true;
        s->close_err   = err;
        tlog("ctrl_tcp: *** PEER CLOSED pendant %s (rc=%d err=%d sockerr=%d) *** "
             "control channel dead - we stop writing",
             op, rc, err, shadow_sock_errno());
    } else {
        s->suppressed++;
    }
}

bool ctrl_tcp_peer_closed(const ctrl_tcp_session *s) {
    return s ? s->peer_closed : true;
}

int ctrl_tcp_close_err(const ctrl_tcp_session *s) {
    return s ? s->close_err : 0;
}

void ctrl_tcp_set_path(ctrl_tcp_session *s, const char *path) {
    if (!s) return;
    if (path) snprintf(s->path, sizeof(s->path), "%s", path);
    else      snprintf(s->path, sizeof(s->path), "forward");
}

void ctrl_tcp_set_bearer(ctrl_tcp_session *s, const char *bearer) {
    if (!s) return;
    if (bearer) snprintf(s->bearer, sizeof(s->bearer), "%s", bearer);
    else        s->bearer[0] = 0;
}

void ctrl_tcp_set_instance(ctrl_tcp_session *s, int instance) {
    if (!s) return;
    s->instance = instance;
}

/* The TCP connect is now shared: tls_chan.c holds exactly this code, moved
 * there on 2026-08-25. The video and input channels used to copy it WITHOUT
 * the connect timeout - so they blocked forever on an unreachable server. The
 * TLS setup stays here: this channel is the only one that sends an SNI
 * extension, and only on port 443 (REST API). */
static int tcp_connect_any(const char *host, int port, int timeout_ms,
                           const volatile int *abort) {
    return tls_chan_tcp_connect_abortable(host, port, timeout_ms, "ctrl_tcp", abort);
}

/* Compatibility alias - formerly v4-only, now any-family. */
static int tcp_connect_v4(const char *host, int port, int timeout_ms,
                          const volatile int *abort) {
    return tcp_connect_any(host, port, timeout_ms, abort);
}

ctrl_tcp_session *ctrl_tcp_open(const char *host, int timeout_ms) {
    return ctrl_tcp_open_port(host, 443, timeout_ms);
}

/* S57b - the abort flag travels as a PARAMETER, all the way down to the
 * `select` that waits for the connect. Not a module variable set around the
 * call: that would be session state in disguise, and this repository spent
 * today paying for exactly that family (G58 - the gamepad toggle cached for
 * the life of the process).
 *
 * `ctrl_tcp_open_port` keeps its signature and passes NULL, so the callers
 * with no flag to offer are untouched. */
ctrl_tcp_session *ctrl_tcp_open_port(const char *host, int port, int timeout_ms) {
    return ctrl_tcp_open_port_abortable(host, port, timeout_ms, NULL);
}

ctrl_tcp_session *ctrl_tcp_open_port_abortable(const char *host, int port,
                                               int timeout_ms,
                                               const volatile int *abort) {
    if (!host) return NULL;
    g_last_fail = "TCP connect";   /* until the socket is up, that is the stage */

    /* For DNS resolution / TCP connect: we use the hostname exactly as given
     * by /vm/ip (e.g. `ipv4-gpu-X.frsbg01.compute.shadow.tech`). Stripping
     * "ipv6-" only matters when we get the IPv6 prefix and want to force
     * IPv4. */
    const char *connect_host = host;
    if (strncmp(host, "ipv6-", 5) == 0) connect_host = host + 5;

    int sock = tcp_connect_v4(connect_host, port, timeout_ms, abort);
    if (sock < 0) return NULL;

    /* For SNI we first considered stripping "ipv4-"/"ipv6-" too, because the
     * Linux client pcap used the hostname WITH the `ipv6-` prefix while
     * nginx's SslCtrlChanV2 binary vhost might be configured on the bare
     * hostname. Settled: the SNI captured in the official pcap DOES carry the
     * ipv6-/ipv4- prefix, so we send the hostname verbatim. The strip above
     * still applies to the TCP connect (connect_host); only the SNI is kept
     * as-is, to match what the server expects. */
    const char *sni_host = host;

    /* The socket is UP by the time we get here, and that distinction is the
     * whole reason this exists. `ctrl_tcp_open_port` returns NULL for "could
     * not reach the port" and for "reached it and TLS refused", so its caller
     * announced `M8.fail - zombie VM or a different port base` for both. On
     * 2026-09-13 the console printed exactly that after fifteen attempts whose
     * own log lines read `connected ... NODELAY=on` two lines above: the port
     * was open, the VM was alive, and the message sent the reader to the one
     * place the fault was not. A diagnostic that names a cause it cannot know
     * is worse than one that says nothing. */
    g_last_fail = "TLS setup";

    /* wolfSSLv23_client_method = "any version, prefer highest" -> the
     * ClientHello carries the TLS 1.2 extensions (extended_master_secret,
     * renegotiation_info, session_ticket) plus a mixed cipher list. That is
     * what the Shadow Linux client does on its binary streams (pcap
     * session-20260502-214916 stream 122/123), whereas our TLS 1.3-only hello
     * got a 400 back from nginx. */
    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!ctx) {
        tlog("ctrl_tcp: wolfSSL_CTX_new FAIL");
        g_last_fail = "wolfSSL_CTX_new";
        shadow_closesocket(sock);
        return NULL;
    }
    /* SAME laxness as wss.c - we trust the server hostname via SNI but skip
     * cert chain verification because we don't ship a CA bundle on Switch. */
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);

    /* Forced cipher list, to match the Shadow Linux client exactly:
     * AES_128_GCM_SHA256 first (= what nginx appears to accept for the
     * CtrlChanV2 binary streams). Cipher list from the pcap:
     * 1301,1302,1303,c02f,... In wolfSSL string form:
     * "TLS13-AES128-GCM-SHA256:TLS13-AES256-GCM-SHA384:TLS13-CHACHA20-POLY1305-SHA256". */
    if (wolfSSL_CTX_set_cipher_list(ctx,
        "TLS13-AES128-GCM-SHA256:"
        "TLS13-AES256-GCM-SHA384:"
        "TLS13-CHACHA20-POLY1305-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES256-GCM-SHA384") != WOLFSSL_SUCCESS) {
        tlog("ctrl_tcp: WARN set_cipher_list FAIL — utilise default");
    }

    WOLFSSL *ssl = wolfSSL_new(ctx);
    if (!ssl) {
        tlog("ctrl_tcp: wolfSSL_new FAIL");
        g_last_fail = "wolfSSL_new (entropy or memory)";
        wolfSSL_CTX_free(ctx);
        shadow_closesocket(sock);
        return NULL;
    }
    /* SNI extension: sent ONLY on :443 (REST API).
     * On :13011 / :13014 / :13020 (SslCtrlChanV2) the ClientHello captured by
     * tcpdump 2026-05-06 carries NEITHER an SNI nor an ALPN extension - it is
     * bare TLS. Reproducing that means not calling UseSNI or UseALPN. */
    if (port == 443) {
        if (wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, sni_host, strlen(sni_host))
            != WOLFSSL_SUCCESS) {
            tlog("ctrl_tcp: UseSNI(%s) FAIL — non-fatal", sni_host);
        } else {
            tlog("ctrl_tcp: SNI=%s (connect_host=%s, port=443)", sni_host, connect_host);
        }
    } else {
        tlog("ctrl_tcp: SNI/ALPN absents (port=%d, SslCtrlChanV2 binaire)", port);
    }

    if (wolfSSL_set_fd(ssl, sock) != WOLFSSL_SUCCESS) {
        tlog("ctrl_tcp: set_fd FAIL");
        g_last_fail = "wolfSSL_set_fd";
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        shadow_closesocket(sock);
        return NULL;
    }

    int rc = wolfSSL_connect(ssl);
    if (rc != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(ssl, rc);
        char errbuf[80] = {0};
        wolfSSL_ERR_error_string((unsigned long)err, errbuf);
        tlog("ctrl_tcp: wolfSSL_connect FAIL rc=%d err=%d (%s)", rc, err, errbuf);
        g_last_fail = "TLS handshake";
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        shadow_closesocket(sock);
        return NULL;
    }
    tlog("ctrl_tcp: TLS handshake OK cipher=%s", wolfSSL_get_cipher(ssl));

    ctrl_tcp_session *s = (ctrl_tcp_session *)calloc(1, sizeof(*s));
    if (!s) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        shadow_closesocket(sock);
        return NULL;
    }
    s->sock = sock;
    s->ctx = ctx;
    s->ssl = ssl;
    s->cipher = NULL;
    s->instance = 3;
    snprintf(s->path, sizeof(s->path), "forward");
    snprintf(s->host, sizeof(s->host), "%s", connect_host);
    g_last_fail = "none";
    return s;
}

static bool ssl_write_all(ctrl_tcp_session *s, const uint8_t *buf, size_t len) {
    if (!s || s->peer_closed) return false;   /* D1 : plus un seul syscall */
    WOLFSSL *ssl = s->ssl;
    size_t off = 0;
    while (off < len) {
        int w = wolfSSL_write(ssl, buf + off, (int)(len - off));
        if (w <= 0) {
            int se = shadow_sock_errno();      /* BEFORE any other call */
            int err = wolfSSL_get_error(ssl, w);
            if (is_peer_closed(w, err, se)) {
                mark_peer_closed(s, w, err, "write");
                return false;
            }
            tlog("ctrl_tcp: wolfSSL_write rc=%d err=%d off=%zu", w, err, off);
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

/* S48: last server clock (ms) seen in a status report. */
/* S56: raised as soon as the server announces CHANNEL_DOWN on the AUDIO
 * channel. This is the oracle for the intermittent-audio defect: readable
 * ~1 s into the session. */
volatile int g_audio_channel_down = 0;

/* AUD18 2026-09-11: how many CHANNEL_DOWN(AUDIO) the server announced this
 * session. The flag above is set once and cannot show a second death; the
 * session loop compares this count with its own copy to time EACH announcement
 * against our last emission on :base+30 - the S57 signature (KB §3.38). Both
 * sides run on the session thread: it is the only one that reads this channel.
 * Reset per session, with the flag. */
volatile unsigned g_audio_channel_down_n = 0;

/* S60: mask of the channels the server declared dead during the session
 * (bit N = channel N, see the VIDEO..FILETRANSFER enum). Reset per session. */
volatile unsigned g_channels_down = 0;

volatile uint32_t g_srv_clock_ms = 0;
/* D4 2026-08-28 - counter of control frames RECEIVED. Serves as the liveness
 * oracle for the host: when the video goes quiet, the only question that
 * matters is "is the server still talking to us?". A counter that keeps rising
 * while UDP is dead says the media path is at fault, not the VM. */
volatile uint32_t g_ctrl_rx_frames = 0;

static bool ssl_read_all(ctrl_tcp_session *s, uint8_t *buf, size_t want, int timeout_ms) {
    if (!s || s->peer_closed) return false;   /* D1 */
    WOLFSSL *ssl = s->ssl;
    /* wolfSSL_read can return < want under TLS 1.3 depending on the record
     * framing, so we loop. The timeout is handled by select() on the
     * underlying fd. */
    int fd = wolfSSL_get_fd(ssl);
    size_t off = 0;
    struct timeval start;
    gettimeofday(&start, NULL);

    while (off < want) {
        if (timeout_ms <= 0) {
            /* With no timeout the block below is skipped: a WANT_READ would
             * then `continue` without ever waiting, i.e. a BUSY wait burning
             * 100% of a core - on Switch, enough to starve the other threads.
             * No current caller passes <= 0, but the API accepts it: so we
             * wait for the socket to become readable, with no deadline. */
            if (wolfSSL_pending(ssl) == 0) {
                fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
                if (select(fd + 1, &rfds, NULL, NULL, NULL) < 0) {
                    tlog("ctrl_tcp: select (no timeout) errno=%d", shadow_sock_errno());
                    return false;
                }
            }
        } else {
            struct timeval now;
            gettimeofday(&now, NULL);
            long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000
                              + (now.tv_usec - start.tv_usec) / 1000;
            if (elapsed_ms >= timeout_ms) {
                tlog("ctrl_tcp: recv TIMEOUT off=%zu/%zu after %ldms",
                     off, want, elapsed_ms);
                return false;
            }
            if (wolfSSL_pending(ssl) == 0) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(fd, &rfds);
                long rem_ms = timeout_ms - elapsed_ms;
                struct timeval tv = { rem_ms / 1000, (rem_ms % 1000) * 1000 };
                int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
                if (sr == 0) {
                    tlog("ctrl_tcp: select TIMEOUT off=%zu/%zu", off, want);
                    return false;
                }
                if (sr < 0) {
                    tlog("ctrl_tcp: select errno=%d", shadow_sock_errno());
                    return false;
                }
            }
        }
        int r = wolfSSL_read(ssl, buf + off, (int)(want - off));
        if (r <= 0) {
            int se = shadow_sock_errno();      /* BEFORE any other call */
            int err = wolfSSL_get_error(ssl, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            if (is_peer_closed(r, err, se)) {
                mark_peer_closed(s, r, err, "read");
                return false;
            }
            tlog("ctrl_tcp: wolfSSL_read rc=%d err=%d off=%zu", r, err, off);
            return false;
        }
        off += (size_t)r;
    }
    return true;
}

/* The real SslCtrlChanV2 wire format (reversed 2026-05-06 with an LD_PRELOAD
 * hook on SSL_write in the official desktop app, see memory
 * project_sslctrlchanv2_wire_format_FOUND):
 *
 *   [ uint16_be msg_type ][ uint16_be payload_len ][ protobuf body ]
 *
 * Observed msg_type = 1 for Capabilities + Authentication. Other types may
 * exist for Encryption / RegisterSession / Heartbeat. The server frames its
 * replies the same way.
 *
 * NO HTTP wrapping: this is bare TLS, just raw frames after the handshake. Our
 * earlier attempt with POST /N/forward got 400 Bad Request every time.
 */
bool ctrl_tcp_send_cleartext(ctrl_tcp_session *s,
                             const uint8_t *body, size_t body_len) {
    /* K15 2026-08-21 - inventory of the oneof fields WE emit, so it can be
     * compared against the official client's (capture: f1x1 f4x1 f12x1 f10x1
     * f8x8 f3x66 f7x2 f6x2 f9x8). A message the desktop never sends could be
     * enough on its own to get us downgraded server-side.
     * SHADOW_CTRL_INVENTORY=1. */
    {
        static int g_inv = -1;
        static unsigned counts[32];
        if (g_inv < 0) {
            const char *e = getenv("SHADOW_CTRL_INVENTORY");
            g_inv = e ? atoi(e) : 0;
        }
        if (g_inv && body && body_len > 6) {
            /* SRV7 2026-10-02: the extraction moved to ctrl_inv.h, which is
             * pure and tested (tests/test_ctrl_inv.c, with the pre-fix parser
             * as a counter-case). It used to skip field 1 unconditionally, so
             * the Capabilities message - the only one with no sequence number -
             * was counted as nothing, every session. */
            const int fld = ctrl_inv_request_field(body, body_len);
            if (fld >= 0 && fld < 32) {
                counts[fld]++;
                static unsigned n = 0;
                if ((++n % 25) == 0 || n < 3) {
                    char line[256]; int o = 0;
                    for (unsigned f = 0; f < 32 && o < 200; f++)
                        if (counts[f]) o += snprintf(line + o, sizeof(line) - o, "f%u=%u ", f, counts[f]);
                    tlog("[K15] inventaire ctrl : %s", line);
                }
            }
        }
    }

    if (!s || !body) return false;
    if (s->peer_closed) return false;   /* D1: cuts the "send raw frame" spam */
    if (body_len > 0xFFFF) {
        tlog("ctrl_tcp: send body_len=%zu > 65535 — fragmentation requise (TODO)",
             body_len);
        return false;
    }
    uint8_t hdr[4];
    hdr[0] = 0x00; hdr[1] = 0x01;          /* msg_type = 1 (default) */
    hdr[2] = (uint8_t)((body_len >> 8) & 0xFF);
    hdr[3] = (uint8_t)(body_len & 0xFF);
    tlog("ctrl_tcp: send raw frame type=1 len=%zu", body_len);
    if (!ssl_write_all(s, hdr, 4)) return false;
    if (!ssl_write_all(s, body, body_len)) return false;
    return true;
}

bool ctrl_tcp_recv_cleartext(ctrl_tcp_session *s,
                             uint8_t *buf, size_t cap, size_t *out_len,
                             int timeout_ms) {
    if (!s || !buf || !out_len) return false;
    if (s->peer_closed) return false;   /* D1 */
    *out_len = 0;
    /* Read 4-byte header [u16_be type][u16_be len]. */
    uint8_t hdr[4];
    if (!ssl_read_all(s, hdr, 4, timeout_ms)) {
        /* D1: if the peer closed, mark_peer_closed has already logged once -
         * no point repeating "recv header FAIL" on every loop turn. */
        if (!s->peer_closed) tlog("ctrl_tcp: recv header FAIL");
        return false;
    }
    /* Diagnostic: when nginx answers with HTTP (= wrong routing), the first 4
     * bytes are "HTTP". Read the status line + headers to see the status code. */
    if (hdr[0] == 'H' && hdr[1] == 'T' && hdr[2] == 'T' && hdr[3] == 'P') {
        char err[2048] = {0};
        memcpy(err, hdr, 4);
        int got = wolfSSL_read(s->ssl, err + 4, (int)(sizeof(err) - 5));
        if (got > 0) err[4 + got] = 0;
        /* Truncate at the end of the status line, or at the first CRLFCRLF */
        char *eol = strstr(err, "\r\n\r\n");
        if (eol) *eol = 0;
        tlog("ctrl_tcp: SERVER RETURNED HTTP RESPONSE (= wrong routing!) :");
        tlog("  %s", err);
        return false;
    }
    uint16_t msg_type = ((uint16_t)hdr[0] << 8) | hdr[1];
    uint16_t payload_len = ((uint16_t)hdr[2] << 8) | hdr[3];
    g_ctrl_rx_frames++;   /* D4: the oracle for whether the host is alive */
    tlog("ctrl_tcp: recv raw frame type=%u len=%u", msg_type, payload_len);
    if ((size_t)payload_len > cap) {
        tlog("ctrl_tcp: recv payload_len=%u > cap=%zu", payload_len, cap);
        return false;
    }
    if (payload_len > 0) {
        if (!ssl_read_all(s, buf, payload_len, timeout_ms)) {
            tlog("ctrl_tcp: recv body FAIL");
            return false;
        }
        /* === S46 2026-08-26 - WHAT THE SMALL SERVER NOTIFICATIONS SAY ===
         *
         * The audio channel `:base+30` starts in roughly one session out of
         * three, and the difference between a session that has it and one that
         * does not lies in these messages: right after the channel is
         * registered, the server sends `41` then `522` when sound flows, and
         * `522` then `41` then a SECOND `41` when it stays silent. Everything
         * else in the exchange is identical to the byte.
         *
         * We only logged their SIZE, so we could see the difference without
         * being able to read it. These short messages are rare and small:
         * dumping them in the clear costs nothing and is exactly what was
         * missing to decide. Capped so it cannot drown the log.
         * `SHADOW_DUMP_CTRL_SMALL=0` turns it off. */
        /* S48: remember the server clock, which it hands us in its status
         * reports (`0a 06 08 <stream> 10 <timestamp>`). The official client
         * echoes it back in its channel-unregister messages; without it we can
         * only reconstruct an approximate duration. */
        if (payload_len >= 20 && payload_len <= 64) {
            for (unsigned i = 0; i + 6 < payload_len; i++) {
                if (buf[i] == 0x1a && buf[i+1] == 0x06 && buf[i+2] == 0x08
                    && buf[i+4] == 0x10) {
                    uint32_t v = 0; unsigned d = 0;
                    for (unsigned k = i + 5; k < payload_len && d < 28; k++) {
                        v |= (uint32_t)(buf[k] & 0x7f) << d;
                        d += 7;
                        if (!(buf[k] & 0x80)) break;
                    }
                    if (v) g_srv_clock_ms = v;
                    break;
                }
            }
        }
        /* === S56 2026-08-27 - THE SERVER TELLS US WHICH CHANNEL WENT DOWN ===
         *
         * We used to log these small messages as hex without reading them. They
         * are decodable, and the official binary gives their meaning exactly:
         *
         *   `ProcessNotifyMessage` switches on the type; case 5 loads the
         *   string `ctrlchanv2: Channel down: {} (sessionId={})`.
         *   The channel-name table, read out of the binary, gives the order
         *   VIDEO, AUDIO, INPUT, CURSOR, MICRO, CONTROLLER, CLIPBOARD,
         *   FILETRANSFER - that is, indices 0 to 7.
         *
         * In other words, the "41-byte notification" that correlated with the
         * absence of sound in 32 sessions out of 32 was simply saying:
         * **AUDIO CHANNEL DOWN**. It had been in our logs for hours, unreadable
         * for want of a twenty-line decoder.
         *
         * The `sessionId` field is not a clock (an earlier reading believed it
         * was, seeing the value climb): it is the identifier the server
         * assigned to the channel in its announcement reply.
         *
         * `SHADOW_DUMP_CTRL_SMALL=1` adds the raw hex next to the decoding. */
        if (payload_len >= 8 && payload_len <= 64) {
            static const char *NOTIF[] = {
                "STOP", "POWEROFF", "RECO_STREAMING", "VM_MIGRATION",
                "VR_ABANDONED", "CHANNEL_DOWN"
            };
            static const char *CANAL[] = {
                "VIDEO", "AUDIO", "INPUT", "CURSOR",
                "MICRO", "CONTROLLER", "CLIPBOARD", "FILETRANSFER"
            };
            /* `12 <len> 0a <len> 08 <type> [1a <len> 08 <channel> 10 <sessionId varint>]* `
             * Field 3 is REPEATED: one frame can name two channels. */
            unsigned type = 0xff;
            for (unsigned i = 0; i + 5 < payload_len; i++) {
                if (buf[i] == 0x0a && buf[i + 2] == 0x08) { type = buf[i + 3]; break; }
            }
            for (unsigned i = 0; i + 4 < payload_len; i++) {
                if (buf[i] != 0x1a || buf[i + 2] != 0x08 || buf[i + 4] != 0x10) continue;
                const unsigned canal = buf[i + 3];
                uint32_t sid = 0; unsigned d = 0;
                for (unsigned k = i + 5; k < payload_len && d < 28; k++) {
                    sid |= (uint32_t)(buf[k] & 0x7f) << d;
                    d += 7;
                    if (!(buf[k] & 0x80)) break;
                }
                /* `canal=` is a LOG FIELD KEY parsed by tools/test/campagne*.sh
                 * (`[S56] CHANNEL_DOWN canal=CONTROLLER`) - it stays as it is. */
                tlog("[S56] %s canal=%s sessionId=%u",
                     type < 6 ? NOTIF[type] : "NOTIFY?",
                     canal < 8 ? CANAL[canal] : "?", sid);
                /* The oracle: has the audio channel gone down? Readable ~1 s
                 * into the session, where before we had to sit through 15 s of
                 * silence to guess it. Deliberately with NO log cap: capping is
                 * exactly what made failures invisible from the 2nd session on
                 * (KB §3.28). */
                /* S60: we record EVERY channel the server declares dead, not
                 * just audio. `g_audio_channel_down` had no reader anywhere in
                 * the repo - an oracle nobody consults is worth nothing. The
                 * mask is published in the summary of every run. */
                if (type == 5 && canal < 8) {
                    g_channels_down |= (1u << canal);
                    if (canal == 1) {
                        g_audio_channel_down = 1;
                        g_audio_channel_down_n++;   /* AUD18: every one, not just the first */
                    }
                }
                i += 4;
            }
            static int g_dump_small = -1;
            if (g_dump_small < 0) {
                const char *e = getenv("SHADOW_DUMP_CTRL_SMALL");
                g_dump_small = e ? atoi(e) : 0;
            }
            if (g_dump_small) {
                char hex[3 * 64 + 1]; int o = 0;
                for (unsigned i = 0; i < payload_len && i < 64; i++)
                    o += snprintf(hex + o, sizeof(hex) - o, "%02x ", buf[i]);
                tlog("[S46] notification %u B : %s", payload_len, hex);
            }
        }
        if (0) {
            return false;
        }
    }
    *out_len = (size_t)payload_len;
    return true;
}

bool ctrl_tcp_has_pending(ctrl_tcp_session *s) {
    if (!s || !s->ssl) return false;
    if (wolfSSL_pending(s->ssl) > 0) return true;
    int fd = wolfSSL_get_fd(s->ssl);
    if (fd < 0) return false;
    fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
    struct timeval tv = {0, 0};
    int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
    return sr > 0 && FD_ISSET(fd, &rfds);
}

/* ING-2 2026-09-11 - see ctrl_tcp.h. Neither call touches the socket:
 * wolfSSL_get_fd() returns the descriptor wolfSSL_set_fd() stored, and
 * wolfSSL_pending() reads the buffer of the record already decrypted. */
int ctrl_tcp_poll_fd(const ctrl_tcp_session *s) {
    if (!s || !s->ssl || s->peer_closed) return -1;
    return wolfSSL_get_fd(s->ssl);
}

bool ctrl_tcp_buffered(const ctrl_tcp_session *s) {
    if (!s || !s->ssl || s->peer_closed) return false;
    return wolfSSL_pending(s->ssl) > 0;
}

void ctrl_tcp_attach_cipher(ctrl_tcp_session *s, shadow_cipher *cipher) {
    if (!s) return;
    s->cipher = cipher;
    tlog("ctrl_tcp: cipher attached %p", (void *)cipher);
}

bool ctrl_tcp_send(ctrl_tcp_session *s, const uint8_t *body, size_t body_len) {
    if (!s || !body) return false;
    if (!s->cipher) return ctrl_tcp_send_cleartext(s, body, body_len);

    /* Wrapped wire format: [ ct | nonce12 | tag16 ]. shadow_cipher_encrypt
     * works in place, so we copy the body in first and then encrypt. */
    uint8_t wire[8 * 1024];
    if (body_len + SHADOW_AEAD_OVERHEAD > sizeof(wire)) {
        tlog("ctrl_tcp: send body_len=%zu too large for the wire buffer", body_len);
        return false;
    }
    memcpy(wire, body, body_len);
    int total = shadow_cipher_encrypt(s->cipher, wire, (int)body_len);
    if (total < 0) {
        tlog("ctrl_tcp: shadow_cipher_encrypt FAIL body_len=%zu", body_len);
        return false;
    }
    return ctrl_tcp_send_cleartext(s, wire, (size_t)total);
}

bool ctrl_tcp_recv(ctrl_tcp_session *s, uint8_t *buf, size_t cap,
                   size_t *out_len, int timeout_ms) {
    if (!s || !buf || !out_len) return false;
    if (!s->cipher) return ctrl_tcp_recv_cleartext(s, buf, cap, out_len, timeout_ms);

    /* Read the wire payload straight into buf, then decrypt in place. */
    size_t wire_len = 0;
    if (!ctrl_tcp_recv_cleartext(s, buf, cap, &wire_len, timeout_ms))
        return false;
    if (wire_len < SHADOW_AEAD_OVERHEAD) {
        tlog("ctrl_tcp: recv encrypted wire_len=%zu < %d",
             wire_len, SHADOW_AEAD_OVERHEAD);
        return false;
    }
    if (!shadow_cipher_decrypt_wire(s->cipher, buf, (int)wire_len)) {
        tlog("ctrl_tcp: shadow_cipher_decrypt_wire FAIL wire_len=%zu", wire_len);
        return false;
    }
    *out_len = wire_len - SHADOW_AEAD_OVERHEAD;
    return true;
}

bool ctrl_tcp_send_raw(ctrl_tcp_session *s, const uint8_t *buf, size_t len) {
    if (!s || !buf) return false;
    return ssl_write_all(s, buf, len);
}

bool ctrl_tcp_recv_raw(ctrl_tcp_session *s, uint8_t *buf, size_t want,
                        int timeout_ms) {
    if (!s || !buf) return false;
    return ssl_read_all(s, buf, want, timeout_ms);
}

void ctrl_tcp_close(ctrl_tcp_session *s) {
    if (!s) return;
    if (s->ssl) {
        wolfSSL_shutdown(s->ssl);
        wolfSSL_free(s->ssl);
    }
    if (s->ctx) wolfSSL_CTX_free(s->ctx);
    /* AF7 2026-09-10 - the ONLY close of the protocol that did not go through
     * `shadow_closesocket`. Under Windows the CRT `close()` rejects a socket
     * handle with EBADF and does nothing: the :base+11 connection stayed open,
     * with no TCP FIN, one per session and one per reconnection attempt, until
     * the final WSACleanup. The comment in sockets_compat.h promised a remap of
     * close() that never existed - which is why no review ever caught it. */
    if (s->sock >= 0) shadow_closesocket(s->sock);
    free(s);
}
