/* VideoSslTcpChannel client. See ctrl_video_tcp.h for the RE context. */
#include "ctrl_video_tcp.h"
#include "cursor_wire.h"
#include "../services/sockets_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
/* AFTER <pthread.h>: tls_chan.h pulls in wolfssl/options.h, which does
 * `#undef _POSIX_THREADS` - see the note above. */
#include "tls_chan.h"

#include "../services/log.h"
/* S81 - the category is DECLARED here, not inferred from the message text.
 * `vlog` stays at INFO: the existing calls do not disappear. `vdbg` is there
 * for the verbose lines, which move over to it one at a time. */
#define vlog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
#define vdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
#define VST_RX_BUF       (32 * 1024)   /* TLS max record = 16K, accumulator 2× */
#define VST_HEADER_LEN   10

struct ctrl_video_tcp_s {
    int                sock;
    WOLFSSL_CTX       *ctx;
    WOLFSSL           *ssl;
    pthread_t          rx_thread;
    bool               rx_running;
    bool               abort_flag;
    on_video_tcp_frame_cb cb;
    on_cursor_frame_cb curseur_cb;   /* S54: see ctrl_video_tcp.h */
    ctrl_video_tcp_role_t role;      /* K16b: what this channel CARRIES */
    void              *udata;

    /* === K16c 2026-08-29 - WE ONLY WRITE FROM THE RECEIVE THREAD ===
     *
     * `ctrl_video_tcp_request_*` used to be called from the session thread and
     * wrote into the TLS object that the rx thread reads at that very moment.
     * wolfSSL is built here with `-DSINGLE_THREADED` (CMakeLists.txt:176 and
     * :243): it has NO internal locking, and two interleaved TLS records give a
     * stream the server can no longer read - which would show up as a dead
     * channel, i.e. as the very fault we are trying to fix.
     *
     * Requests therefore ARM a flag, and the receive loop is what emits. Same
     * pattern as `g_idr_needed` on the session side. `volatile` is enough: these
     * are single-producer, single-consumer flags, and losing one request to a
     * race would be harmless - the next one comes right along. */
    volatile int       demande_image_cle;
    volatile int       demande_rafraichissement;
    /* Keyframe request counter. IN THE STRUCT, so it is reset on every session:
     * a function-level `static` is the costliest family of defects in this repo
     * (black screen from the 3rd session on, sound audible only once,
     * resolution never re-announced). */
    uint16_t           compteur_ifr;
    ctrl_video_tcp_stats_t stats;
    pthread_mutex_t    stats_mtx;
};

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Parses as many COMPLETE frames as it can out of buf[0..len-1].
 * Returns the number of bytes consumed; the rest waits for the next read.
 *
 * === S54 2026-08-26 - FRAME BY THE ANNOUNCED LENGTH ===
 *
 * This parser used to do `nal_len = (len - pos) - 10` then `pos = len; break;`,
 * that is "one TCP read = one frame". That was wrong: a 4142-byte frame was
 * observed arriving in TWO pieces (1024 then 3118), read as two frames, one
 * truncated and the other starting in the middle of the pixels. The length is
 * in the header (`cursor_wire.h`, rule C1).
 *
 * This is the CURSOR channel (KB §3.37): type 0x12 frames go to `cursor_cb`,
 * no longer to the H.264 decoder. */
static size_t vst_parse_frames(ctrl_video_tcp_t *c, uint8_t *buf, size_t len) {
    size_t pos = 0;
    while (len - pos >= VST_HEADER_LEN) {
        uint8_t b0 = buf[pos];

        if (b0 > 0x3F) {
            /* "Unknown header type" - the desktop client drops it silently. We
             * drop the whole buffer to resync. */
            pthread_mutex_lock(&c->stats_mtx);
            c->stats.parse_errors++;
            pthread_mutex_unlock(&c->stats_mtx);
            vlog("vst: unknown byte0=0x%02x — resync (drop %zu B)", b0, len - pos);
            return len;  /* consume all = restart fresh */
        }

        if (b0 <= 0x0F) {
            /* === K15f 2026-08-29 - CONSUME THE FRAME, NOT ONE BYTE ===
             *
             * The old code advanced by ONE byte and went looking for the next
             * valid header. That works by accident on the cursor channel, where
             * these frames have an EMPTY payload: the following nine bytes are
             * zero, so we land on the next header nine loop turns later.
             *
             * The official client IGNORES the WHOLE frame, already consumed
             * (K15c, reading its dispatch). As soon as a frame of this class
             * carries a payload - and the first capture in the `reliability`
             * profile shows 39 of them on the video channel - the old behaviour
             * would walk INTO the payload and desynchronise, without an error.
             *
             * SHADOW_STFP_SKIP_ONE=1 restores the one-byte advance. */
            static int g_skip_one = -1;
            if (g_skip_one < 0) {
                const char *e = getenv("SHADOW_STFP_SKIP_ONE");
                g_skip_one = e ? atoi(e) : 0;
            }
            if (g_skip_one) { pos++; continue; }

            const uint32_t ln = read_le32(buf + pos + 2);
            if (ln > 8u * 1024u * 1024u) {   /* borne de bon sens : resync */
                pthread_mutex_lock(&c->stats_mtx);
                c->stats.parse_errors++;
                pthread_mutex_unlock(&c->stats_mtx);
                vlog("vst: classe 0x%02x, longueur absurde %u — resync", b0, ln);
                return len;
            }
            if (len - pos < (size_t)VST_HEADER_LEN + ln) break;   /* trame incomplete */
            pos += (size_t)VST_HEADER_LEN + ln;
            continue;
        }

        if ((b0 & 0xF0) == 0x30) {
            /* PING: 10-byte header, ts_ms at offset 6. No payload. */
            uint32_t ts_ms = read_le32(buf + pos + 6);
            pthread_mutex_lock(&c->stats_mtx);
            c->stats.pings_received++;
            pthread_mutex_unlock(&c->stats_mtx);
            static int g_ping_log = 0;
            if (g_ping_log < 3) {
                vlog("vst: PING ts_ms=%u", ts_ms);
                g_ping_log++;
            }
            pos += VST_HEADER_LEN;
            continue;
        }

        /* S54: the length comes from the header, not from the read. */
        cursor_wire_header_t e;
        /* K16a - the cap is this module's own buffer cap: beyond it the length
         * is nonsense and resynchronising is the right answer. It is the SAME
         * cap as the class-0 branch above. */
        if (!cursor_wire_parse_header(buf + pos, len - pos,
                                      CURSOR_WIRE_CAP_DEFAULT, &e)) {
            pthread_mutex_lock(&c->stats_mtx);
            c->stats.parse_errors++;
            pthread_mutex_unlock(&c->stats_mtx);
            vlog("vst: header refused (b0=0x%02x) - resync", b0);
            return len;
        }
        if (e.frame_len > len - pos) {
            /* Incomplete frame: leave the rest in the buffer, the next read
             * will complete it. That is exactly what the old code did not
             * do. */
            break;
        }
        uint8_t  flag     = e.seq;
        uint32_t frame_id = e.id;
        size_t   nal_len  = e.payload_len;
        if (nal_len == 0) {
            /* Payload-less frame: type 0x02 always is (55 seen, all at zero).
             * Nothing to hand up. */
            pos += e.frame_len;
            continue;
        }

        /* K16b - the class, not the parity of the counter. `flag` is byte 1,
         * i.e. the frame number, so `flag & 0x01` declared every other frame a
         * keyframe. Class 1 (`0x1_`) is the complete frame. */
        const int is_keyframe = ((b0 >> 4) & 0x0F) == 1;

        pthread_mutex_lock(&c->stats_mtx);
        c->stats.frames_received++;
        c->stats.bytes_received += (uint32_t)nal_len;
        if (is_keyframe) c->stats.idr_frames++;
        pthread_mutex_unlock(&c->stats_mtx);

        static int g_frame_log = 0;
        if (g_frame_log < 10) {
            vlog("vst: FRAME byte0=0x%02x flag=0x%02x frame_id=%u nal_len=%zu "
                 "head: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                 b0, flag, frame_id, nal_len,
                 buf[pos+10], buf[pos+11], buf[pos+12], buf[pos+13],
                 buf[pos+14], buf[pos+15], buf[pos+16], buf[pos+17],
                 buf[pos+18], buf[pos+19], buf[pos+20], buf[pos+21],
                 buf[pos+22], buf[pos+23], buf[pos+24], buf[pos+25]);
            g_frame_log++;
        }

        /* === K16b - ROUTING FOLLOWS THE CHANNEL'S ROLE ===
         *
         * `SHADOW_STFP_ROUTE=0` restores the previous routing, by CLASS. It is
         * there to reproduce the black screen in one command, which K16d's
         * cross-check alarm needs in order to prove it detects anything. */
        static int g_route_role = -1;
        if (g_route_role < 0) {
            const char *er = getenv("SHADOW_STFP_ROUTE");
            g_route_role = er ? atoi(er) : 1;
        }
        const bool vers_curseur =
            g_route_role ? (c->role == CTRL_VST_ROLE_CURSOR
                            && e.type == CURSOR_WIRE_TYPE_IMAGE)
                         : (e.type == CURSOR_WIRE_TYPE_IMAGE);

        if (vers_curseur) {
            /* S54: a cursor image. It never had any business in the H.264
             * decoder - the decoder was the one rejecting it, logging
             * "missing Annex-B start code" every session. */
            if (c->curseur_cb)
                c->curseur_cb(c->udata, buf + pos + VST_HEADER_LEN, nal_len,
                              e.type, e.id);
        } else if (c->cb) {
            /* On the VIDEO channel this branch ALSO receives class 0x12 - that
             * is the keyframe, and it is the whole point of K16b. */
            c->cb(c->udata, buf + pos + VST_HEADER_LEN, nal_len,
                  frame_id, flag, is_keyframe);
        }

        pos += e.frame_len;   /* and on to the next: several frames per read */
    }
    return pos;
}

static void *vst_recv_thread(void *arg) {
    ctrl_video_tcp_t *c = (ctrl_video_tcp_t *)arg;
    size_t   rx_cap = VST_RX_BUF;
    uint8_t *rx = (uint8_t *)malloc(rx_cap);
    if (!rx) {
        vlog("vst: rx_thread malloc FAIL");
        return NULL;
    }
    size_t accum = 0;

    vlog("vst: rx_thread started");
    int idle_ticks = 0;

    /* V16 RE 2026-05-18: 0x70 heartbeat every ~7 s (= the desktop pattern).
     * Without it the server FINs the connection after ~25-30 s.
     * Period configurable via SHADOW_VST_HB_MS (default 7000 = match desktop). */
    /* === K16c - THE HEARTBEAT RATE DEPENDS ON THE ROLE ===
     * Measured on the `reliability` profile capture: on the VIDEO channel the
     * official client emits `70` every **1.09 s** (97 occurrences), not every
     * 7 s. The original 7000 comes from the CURSOR channel, where it is right.
     * Beating too slowly on video risks the 25-30 s cutoff that this very
     * heartbeat exists to avoid - and we would read it as KB §3.34, that is,
     * as the very fault we are trying to work around.
     * `SHADOW_VST_HB_MS` still wins over both: setting it to 7000 restores the
     * previous behaviour exactly. */
    int hb_period_ms = (c->role == CTRL_VST_ROLE_VIDEO) ? 1000 : 7000;
    {
        const char *e = getenv("SHADOW_VST_HB_MS");
        if (e) {
            int v = atoi(e);
            if (v >= 1000 && v <= 60000) hb_period_ms = v;
        }
    }
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    long long t_last_hb_ms = (long long)t0.tv_sec * 1000 + t0.tv_nsec / 1000000;

    while (!c->abort_flag) {
        /* Heartbeat check: send 0x70 if due. */
        struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
        long long now_ms = (long long)tn.tv_sec * 1000 + tn.tv_nsec / 1000000;
        if (now_ms - t_last_hb_ms >= hb_period_ms) {
            const uint8_t p = 0x70;
            int wh = wolfSSL_write(c->ssl, &p, 1);
            static int g_hb_log = 0;
            if (g_hb_log < 5) {
                vlog("vst: heartbeat 0x70 sent (rc=%d, period=%dms)", wh, hb_period_ms);
                g_hb_log++;
            }
            t_last_hb_ms = now_ms;
        }

        /* === K16c - REQUESTS ARE SENT HERE, AND NOWHERE ELSE ===
         * See the comment on `demande_image_cle`: wolfSSL is built
         * `-DSINGLE_THREADED`, so writing from the session thread while this
         * thread is reading would interleave two TLS records. */
        if (c->demande_image_cle) {
            c->demande_image_cle = 0;
            /* K16c - THE SAME message as on UDP: `69 50 00 02 [counter u16 LE]`,
             * six bytes WRITTEN RAW, with no STFP header. This protocol's
             * upstream direction is not framed.
             *
             * K15q had put a bare 10-byte header, class 0, in its place, read
             * off what the official client writes - but on the WRONG socket:
             * correlating by `ssl=` pointer places it on `:base+14`, the
             * clipboard. So we were injecting a clipboard opcode into a stream
             * of video opcodes. No revert toggle for this one: the "old path"
             * is a message that does not exist on this channel, and restoring
             * it would make any later A/B unreadable. */
            const uint16_t n = c->compteur_ifr++;
            const uint8_t ifr[6] = {
                0x69, 0x50, 0x00, 0x02,
                (uint8_t)(n & 0xFF), (uint8_t)((n >> 8) & 0xFF)
            };
            const int w = wolfSSL_write(c->ssl, ifr, (int)sizeof ifr);
            static int g_log_ifr = 0;
            if (g_log_ifr < 5) {
                vlog("[K16c] key-frame request iP #%u (rc=%d)", (unsigned)n, w);
                g_log_ifr++;
            }
        }
        if (c->demande_rafraichissement) {
            c->demande_rafraichissement = 0;
            const uint8_t d = 0x64;
            const int w = wolfSSL_write(c->ssl, &d, 1);
            static int g_log_raf = 0;
            if (g_log_raf < 3) {
                vlog("vst: [R1] rafraichissement 0x64 emis (rc=%d)", w);
                g_log_raf++;
            }
        }

        /* === K15g 2026-08-29 - THE BUFFER MUST GROW, NOT BE FLUSHED ===
         *
         * 32 KB were enough for the cursor, whose largest frame is 4142 bytes.
         * A 1080p keyframe is 200 to 400 KB: the old code would have logged
         * "overflow" and THROWN the buffer away on every keyframe, that is,
         * desynchronised the stream permanently.
         *
         * So we grow until the announced frame fits. The 8 MB cap is the
         * parser's: beyond it the length is nonsense and resynchronising is the
         * right answer. */
        if (accum + 16384u > rx_cap) {
            size_t fresh = rx_cap * 2;
            if (fresh > 8u * 1024u * 1024u) {
                vlog("vst: tampon au plafond (%zu o) — resync", rx_cap);
                accum = 0;
            } else {
                uint8_t *n2 = (uint8_t *)realloc(rx, fresh);
                if (!n2) { vlog("vst: agrandissement du tampon KO — resync"); accum = 0; }
                else { rx = n2; rx_cap = fresh;
                       vlog("vst: tampon agrandi a %zu Ko", rx_cap / 1024); }
            }
        }
        int n = wolfSSL_read(c->ssl, rx + accum, (int)(rx_cap - accum));
        if (n <= 0) {
            int err = wolfSSL_get_error(c->ssl, n);
            int se = shadow_sock_errno();
            /* SO_RCVTIMEO timeout on the socket -> wolfSSL returns WANT_READ + errno=EAGAIN/EWOULDBLOCK */
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE
                || se == EAGAIN || se == EWOULDBLOCK) {
                idle_ticks++;
                if (idle_ticks % 25 == 0) {  /* every ~5s */
                    vlog("vst: idle %ds (no data from server, conn alive)", idle_ticks / 5);
                }
                continue;
            }
            vlog("vst: wolfSSL_read returned %d (err=%d sockerr=%d) — closing", n, err, se);
            break;
        }
        idle_ticks = 0;
        accum += (size_t)n;
        static int g_rd_log = 0;
        if (g_rd_log < 5) {
            vlog("vst: SSL_read got %d B (accum=%zu)", n, accum);
            g_rd_log++;
        }
        size_t consumed = vst_parse_frames(c, rx, accum);
        if (consumed > 0 && consumed < accum) {
            memmove(rx, rx + consumed, accum - consumed);
            accum -= consumed;
        } else if (consumed >= accum) {
            accum = 0;
        }
    }
    free(rx);
    c->rx_running = false;
    vlog("vst: rx_thread exited");
    return NULL;
}

void ctrl_video_tcp_set_cursor_cb(ctrl_video_tcp_t *c, on_cursor_frame_cb cb)
{
    if (c) c->curseur_cb = cb;
}

/* K15h: the old signature becomes a call to the new one, with 20. */
int ctrl_video_tcp_open(ctrl_video_tcp_t **out,
                        const char *vm_host, uint16_t base_port,
                        const uint8_t auth_hash[20],
                        const char *bearer_jwt,
                        const char *streaming_token,
                        on_video_tcp_frame_cb cb, void *udata)
{
    return ctrl_video_tcp_open_port(out, vm_host, base_port, 20,
                                    CTRL_VST_ROLE_CURSOR,
                                    auth_hash, bearer_jwt, streaming_token,
                                    cb, udata);
}

int ctrl_video_tcp_open_port(ctrl_video_tcp_t **out,
                         const char *vm_host, uint16_t base_port,
                         uint16_t port_offset,
                         ctrl_video_tcp_role_t role,
                         const uint8_t auth_hash[20],
                         const char *bearer_jwt,
                         const char *streaming_token,
                         on_video_tcp_frame_cb cb, void *udata) {
    if (!out || !vm_host) return -1;
    *out = NULL;

    /* Strip the "ipv6-" prefix for the connect, keep the original for SNI
     * (= but we do not send SNI on :base+20 anyway). */
    const char *connect_host = vm_host;
    if (strncmp(vm_host, "ipv6-", 5) == 0) connect_host = vm_host + 5;

    uint16_t port = (uint16_t)(base_port + port_offset);
    vlog("vst: opening %s:%u (base=%u)", connect_host, port, base_port);

    /* TCP + TLS connect: tls_chan.c, shared with the control channel.
     * This module used to copy the same sequence WITHOUT a connect timeout - an
     * unreachable server therefore blocked this thread forever. The 8 s
     * timeout is the one the bootstrap already uses. */
    tls_chan ch;
    if (!tls_chan_open(&ch, connect_host, port, 8000, "vst")) return -1;
    int sock = ch.sock;
    WOLFSSL_CTX *ctx = ch.ctx;
    WOLFSSL *ssl = ch.ssl;

    vlog("vst: TLS handshake OK cipher=%s", wolfSSL_get_cipher(ssl));

    /* Connect_msg variants - the desktop client sends 25B + 3x 0x64 + periodic
     * 0x70. The V16 RE plaintext capture of 2026-05-18 confirms variant 5 was
     * the right format (= 25B `41 01 00 14 00 [hash20]`). Variants 7/8 (=
     * streamingtoken body, jwt body) were dead ends - the body is the 20B auth
     * hash.
     *
     * SHADOW_VST_VARIANT (= for A/B debugging only, variant=5 is the target):
     *   0 = none (= just listen)
     *   1 = 5B `00 00 00 00 00` (= server FINs immediately)
     *   2 = 25B `00 00 00 14 00 [hash20]` (= idle 25s then FIN)
     *   3 = raw 20B auth_hash alone (= idle then FIN)
     *   4 = 57B placeholder + zero cipher key (= widened test)
     *   5 = **25B `41 01 00 14 00 [hash20]` = CONFIRMED desktop format** (V16)
     *   6 = 31B register + IFR request (= idle then FIN)
     *   7 = HEADER `41 01 00 [jwt_len LE] [main_jwt]` (= tried, FIN)
     *   8 = HEADER `41 01 00 [tok_len LE] [streaming_token]` (= tried, FIN)
     * Default 5 (= confirmed correct by the MASTER plaintext capture of
     * 2026-05-18). */
    int variant = 5;
    {
        const char *e = getenv("SHADOW_VST_VARIANT");
        if (e) variant = atoi(e);
    }
    /* Buffer sized for a large JWT. Max ~2KB. */
    uint8_t connect_msg[2048];
    int connect_len = 0;
    switch (variant) {
        case 0: /* No connect msg */
            break;
        case 1:
            memset(connect_msg, 0, 5);
            connect_len = 5;
            break;
        case 2: {
            /* 5B header + 20B body. hdr_int unknown - try hdr_int = 0,
             * body_len = 20 -> [00 00 00 14 00 hash20...] */
            connect_msg[0] = 0x00;
            connect_msg[1] = 0x00; connect_msg[2] = 0x00;     /* subtype LE = 0 */
            connect_msg[3] = 0x14; connect_msg[4] = 0x00;     /* body_len LE = 20 */
            if (auth_hash) memcpy(connect_msg + 5, auth_hash, 20);
            else memset(connect_msg + 5, 0, 20);
            connect_len = 25;
            break;
        }
        case 3:
            if (auth_hash) memcpy(connect_msg, auth_hash, 20);
            else memset(connect_msg, 0, 20);
            connect_len = 20;
            break;
        case 4:
            /* Full secrets - placeholder, the cipher key is not passed yet */
            connect_msg[0] = 0x00;
            connect_msg[1] = 0x00; connect_msg[2] = 0x00;
            connect_msg[3] = 0x34; connect_msg[4] = 0x00;     /* body_len LE = 52 */
            if (auth_hash) memcpy(connect_msg + 5, auth_hash, 20);
            else memset(connect_msg + 5, 0, 20);
            memset(connect_msg + 25, 0, 32);                  /* cipher key TBD */
            connect_len = 57;
            break;
        case 5: {
            /* Byte-exact UDP register packet format (ctrl_msgs.c::ctrl_build_udp_register). */
            connect_msg[0] = 0x41;
            connect_msg[1] = 0x01;
            connect_msg[2] = 0x00;
            connect_msg[3] = 0x14;   /* length 20 */
            connect_msg[4] = 0x00;
            if (auth_hash) memcpy(connect_msg + 5, auth_hash, 20);
            else memset(connect_msg + 5, 0, 20);
            connect_len = 25;
            break;
        }
        case 6: {
            /* Variant 5 register with an IFR request appended: 25B register + 6B IFR */
            connect_msg[0] = 0x41;
            connect_msg[1] = 0x01;
            connect_msg[2] = 0x00;
            connect_msg[3] = 0x14;
            connect_msg[4] = 0x00;
            if (auth_hash) memcpy(connect_msg + 5, auth_hash, 20);
            else memset(connect_msg + 5, 0, 20);
            connect_msg[25] = 0x69;
            connect_msg[26] = 0x50;
            connect_msg[27] = 0x00;
            connect_msg[28] = 0x02;
            connect_msg[29] = 0x01;
            connect_msg[30] = 0x00;
            connect_len = 31;
            break;
        }
        case 8: {
            /* TIER1 RE 2026-05-16: body = streaming_token (= 34 chars typically).
             * Header = `0x41 0x01 0x00 [body_len LE u16]`. */
            if (!streaming_token) {
                vlog("vst: variant 8 needs streaming_token — fallback variant 5");
                connect_msg[0] = 0x41; connect_msg[1] = 0x01; connect_msg[2] = 0x00;
                connect_msg[3] = 0x14; connect_msg[4] = 0x00;
                if (auth_hash) memcpy(connect_msg + 5, auth_hash, 20);
                else memset(connect_msg + 5, 0, 20);
                connect_len = 25;
                break;
            }
            size_t tok_len = strlen(streaming_token);
            if (tok_len > 0xFFFF || tok_len + 5 > sizeof(connect_msg)) {
                vlog("vst: streaming_token too large (%zu B)", tok_len);
                tok_len = sizeof(connect_msg) - 5;
            }
            connect_msg[0] = 0x41;
            connect_msg[1] = 0x01;
            connect_msg[2] = 0x00;
            connect_msg[3] = (uint8_t)(tok_len & 0xFF);
            connect_msg[4] = (uint8_t)((tok_len >> 8) & 0xFF);
            memcpy(connect_msg + 5, streaming_token, tok_len);
            connect_len = (int)(5 + tok_len);
            vlog("vst: variant 8 streaming_token=%zuB", tok_len);
            break;
        }
        case 7: {
            /* RE agent #2, byte-exact: hdr_int = 0x41430001 hardcoded in rodata.
             * Header = `[0x41 ('A')][0x01 0x00 (subtype LE)][body_len LE u16]`.
             * Body = main_jwt (= the JWT from /proximus-credentials, ~100B typically). */
            if (!bearer_jwt) {
                vlog("vst: variant 7 needs bearer_jwt — falling back to variant 5");
                connect_msg[0] = 0x41;
                connect_msg[1] = 0x01;
                connect_msg[2] = 0x00;
                connect_msg[3] = 0x14;
                connect_msg[4] = 0x00;
                if (auth_hash) memcpy(connect_msg + 5, auth_hash, 20);
                else memset(connect_msg + 5, 0, 20);
                connect_len = 25;
                break;
            }
            size_t jwt_len = strlen(bearer_jwt);
            if (jwt_len > 0xFFFF || jwt_len + 5 > sizeof(connect_msg)) {
                vlog("vst: bearer_jwt too large (%zu B) — clamp", jwt_len);
                jwt_len = sizeof(connect_msg) - 5;
            }
            connect_msg[0] = 0x41;
            connect_msg[1] = 0x01;
            connect_msg[2] = 0x00;
            connect_msg[3] = (uint8_t)(jwt_len & 0xFF);
            connect_msg[4] = (uint8_t)((jwt_len >> 8) & 0xFF);
            memcpy(connect_msg + 5, bearer_jwt, jwt_len);
            connect_len = (int)(5 + jwt_len);
            break;
        }
        default:
            vlog("vst: unknown VARIANT=%d, falling back to variant 0 (none)", variant);
            break;
    }
    vlog("vst: variant=%d connect_msg=%dB", variant, connect_len);
    if (connect_len > 0) {
        int wr = wolfSSL_write(ssl, connect_msg, connect_len);
        if (wr != connect_len) {
            vlog("vst: connect_msg write FAIL rc=%d (want=%d)", wr, connect_len);
            wolfSSL_free(ssl);
            wolfSSL_CTX_free(ctx);
            shadow_closesocket(sock);
            return -1;
        }
        vlog("vst: connect_msg %dB sent — server should start pushing frames", connect_len);
    } else {
        vlog("vst: no connect_msg sent (variant 0) — passive listen mode");
    }

    /* V16 RE 2026-05-18, plaintext capture: after the 25B auth, the desktop
     * client sends:
     *   3x 1B `0x64` ('d') spread over ~0.87s, +1.16s, +4.3s = stream start trigger
     *   then 1B `0x70` ('p') heartbeat every ~6-8s (= sent by rx_thread, see below)
     * We send the three 'd' back to back (no sleeps - the server gates on the
     * count, not on the timing). The periodic 0x70 lives in vst_recv_thread. */
    /* === K16c - THE THREE `0x64` ARE A CURSOR MATTER ===
     * Exhaustive dump of the official VIDEO channel's upstream direction in the
     * `reliability` profile: 1608 writes, four shapes, and **zero `0x64`**. The
     * server pushes its keyframe unprompted 74 ms after the registration.
     * Sending them on video means adding an opcode the official client never
     * sends, into a stream of raw opcodes.
     * They stay on the cursor, where R1 measured them (92 during resizes, 0
     * outside them).
     *
     * Here we still write from the calling thread, and that is SAFE: the
     * receive thread is only created after this block. */
    if (variant == 5 && role == CTRL_VST_ROLE_CURSOR) {
        const uint8_t d = 0x64;
        int wd1 = wolfSSL_write(ssl, &d, 1);
        int wd2 = wolfSSL_write(ssl, &d, 1);
        int wd3 = wolfSSL_write(ssl, &d, 1);
        vlog("vst: 3x 0x64 trigger sent (rc=%d/%d/%d)", wd1, wd2, wd3);
    }

    /* Set the socket recv timeout so close() does not deadlock on a blocking read. */
    {
#ifdef _WIN32
        DWORD tv = 200;  /* ms, Windows-style */
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
        struct timeval tv = {0, 200 * 1000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    ctrl_video_tcp_t *c = (ctrl_video_tcp_t *)calloc(1, sizeof(*c));
    if (!c) {
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        return -1;
    }
    c->sock = sock;
    c->ctx = ctx;
    c->ssl = ssl;
    c->cb = cb;
    c->role = role;
    c->udata = udata;
    c->rx_running = true;
    c->abort_flag = false;
    pthread_mutex_init(&c->stats_mtx, NULL);

    if (pthread_create(&c->rx_thread, NULL, vst_recv_thread, c) != 0) {
        vlog("vst: pthread_create FAIL");
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        free(c);
        return -1;
    }
    *out = c;
    return 0;
}

void ctrl_video_tcp_close(ctrl_video_tcp_t *c) {
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

void ctrl_video_tcp_get_stats(const ctrl_video_tcp_t *c, ctrl_video_tcp_stats_t *out) {
    if (!c || !out) return;
    /* mtx is mutable in stats - cast away const */
    pthread_mutex_lock((pthread_mutex_t *)&c->stats_mtx);
    *out = c->stats;
    pthread_mutex_unlock((pthread_mutex_t *)&c->stats_mtx);
}

/* === R1 2026-08-21 - THE `0x64` REFRESH REQUEST ===
 * Guided capture `captures_resize_20260821_175354` (35 window resizes, geometry
 * sampled every 200 ms). Net result:
 *   - the official client sends NO control message on a resize (no
 *     `kNotifyResolution` f14, no `kDisplayConfig` f10 beyond the bootstrap):
 *     **the stream resolution is not renegotiated**, the client simply rescales
 *     locally;
 *   - what it does send on THIS channel (`:base+20`) is one-byte `0x64`
 *     messages: **92 during the resizes, 0 outside them**. So it is a picture
 *     refresh request, emitted when the local scaling changes.
 * (`0x70` is the already-implemented heartbeat: 13 occurrences, spread out.)
 * Call this when the render area changes size. */
/* === K16c 2026-08-29 - THE KEYFRAME REQUEST ===
 *
 * It ARMS, it does not write: the send happens in the receive loop (see
 * `keyframe_requested` in the struct). The message itself is the SAME as on
 * UDP, `69 50 00 02 [counter u16 LE]`, written raw.
 *
 * K15q had guessed a different message - a bare 10-byte STFP header, class 0 -
 * read off what the official client writes. The shape was right, the SOCKET was
 * wrong: correlating by `ssl=` pointer places it on `:base+14`, the clipboard.
 * That is also why it never produced anything - and, worse, the server had
 * already sent its keyframe: we were the ones throwing it away (K16b).
 *
 * `SHADOW_STFP_KEYFRAME=0` disarms the request. */
int ctrl_video_tcp_request_keyframe(ctrl_video_tcp_t *c)
{
    if (!c || !c->ssl) return -1;
    static int g_on = -1;
    if (g_on < 0) {
        const char *e = getenv("SHADOW_STFP_KEYFRAME");
        g_on = e ? atoi(e) : 1;
    }
    if (!g_on) return 0;
    c->demande_image_cle = 1;
    return 0;
}

/* === R1 2026-08-21 - THE `0x64` REFRESH REQUEST ===
 * Guided capture `captures_resize_20260821_175354` (35 window resizes, geometry
 * sampled every 200 ms). Net result:
 *   - the official client sends NO control message on a resize (no
 *     `kNotifyResolution`, no `kDisplayConfig` beyond the bootstrap): the stream
 *     resolution is NOT renegotiated, the client simply rescales locally;
 *   - what it does send on the CURSOR channel is one-byte `0x64` messages: 92
 *     during the resizes, 0 outside them.
 * (`0x70` is the heartbeat, already implemented.)
 *
 * K16c: this arms, it no longer writes - same reason as above. */
int ctrl_video_tcp_request_refresh(ctrl_video_tcp_t *c)
{
    if (!c) return -1;
    static int g_on = -1;
    if (g_on < 0) {
        const char *e = getenv("SHADOW_VIDEO_REFRESH");
        g_on = e ? atoi(e) : 1;
    }
    if (!g_on || !c->ssl) return 0;
    c->demande_rafraichissement = 1;
    return 0;
}
