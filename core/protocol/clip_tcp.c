/* clip_tcp - see clip_tcp.h for the protocol facts this encodes and for the two
 * server behaviours a caller must know about. */
#include "clip_tcp.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include <errno.h>
#include "../services/sockets_compat.h"
#include "tls_chan.h"
#include "clip_wire.h"
#include "clip_chan.h"
#include "../common/log.h"

#define cliplog(...) JOURNAL_INFO_(JOURNAL_CAT_NETWORK, __VA_ARGS__)

/* Reassembly buffer. 4 MiB is `clip_chan`'s default cap, derived from the VM's
 * own clamp of 1 Mi UTF-16 units at worst-case UTF-8 cost. Allocated once per
 * channel rather than per message: a clipboard paste is not a hot path, but a
 * 4 MiB allocation inside the receive loop would be. */
#define CLIP_BUF_CAP (4u * 1024u * 1024u)

struct clip_tcp {
    tls_chan          tls;
    pthread_t         rx;
    int               rx_started;
    volatile int      stop;
    const volatile int *abort_flag;

    /* wolfSSL here is built -DSINGLE_THREADED: two threads writing one TLS
     * session interleave records. Every write goes through this. K16c paid for
     * that lesson on the video channel. */
    pthread_mutex_t   tx_lock;

    clip_chan_t       chan;
    uint8_t          *buf;

    clip_tcp_text_cb  cb;
    void             *user;

    /* CLIP4: the pull, and the VM's own pull. `auto_request` is written by any
     * thread and read by the receive thread; one int, and a stale read costs at
     * most one extra REQUEST, so it needs no lock. */
    volatile int          auto_request;
    clip_tcp_request_cb   req_cb;
    void                 *req_user;

    clip_tcp_stats_t  st;
};

static uint32_t now_ms_u32(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

/* One place that writes to the TLS session. `tls_chan` brings the channel up
 * and hands back the WOLFSSL*; reads and writes go straight through it, the way
 * ctrl_video_tcp and ctrl_input_tcp do. */
static bool tx(clip_tcp_t *c, const uint8_t *p, size_t n)
{
    if (!c || !p || n == 0 || !c->tls.ssl) return false;
    pthread_mutex_lock(&c->tx_lock);
    const int w = wolfSSL_write(c->tls.ssl, p, (int)n);
    pthread_mutex_unlock(&c->tx_lock);
    return w == (int)n;
}

static bool send_simple(clip_tcp_t *c, uint16_t opcode)
{
    uint8_t h[CLIP_WIRE_HEADER_LEN];
    const size_t n = clip_wire_build_header(h, sizeof h, opcode, CLIP_FMT_TEXT);
    return n != 0 && tx(c, h, n);
}

int clip_tcp_request(clip_tcp_t *c)
{
    if (!c) return -1;
    if (!send_simple(c, CLIP_OP_REQUEST)) return -1;
    c->st.tx_requests++;
    clip_chan_note_request_sent(&c->chan, now_ms_u32());
    return 0;
}

int clip_tcp_send_text(clip_tcp_t *c, const uint8_t *text, size_t n)
{
    if (!c) return -1;
    if (n > 0 && !text) return -1;

    /* Build into a heap buffer: a paste can be megabytes, and this runs on a
     * caller thread whose stack we do not own. */
    const size_t total = CLIP_WIRE_HEADER_LEN + n;
    uint8_t *msg = (uint8_t *)malloc(total ? total : 1);
    if (!msg) return -1;

    const size_t built = clip_wire_build_reply_text(msg, total, c->chan.cfg.cap,
                                                    (const char *)text, n);
    if (built == 0) { free(msg); return -2; }   /* over the cap, or malformed */

    const bool ok = tx(c, msg, built);
    free(msg);
    if (!ok) return -1;
    c->st.tx_replies++;
    /* Remember what we pasted: the VM will announce an UPDATE for our own
     * paste, and without this the next REQUEST would deliver our own text back
     * to the client as if the user had copied it in the VM. */
    clip_chan_note_text(&c->chan, text, n);
    return 0;
}

static void handle_events(clip_tcp_t *c, const clip_ev_t *evs, size_t n_evs)
{
    for (size_t i = 0; i < n_evs; i++) {
        switch (evs[i].kind) {
            case CLIP_EV_UPDATE:
                c->st.rx_updates++;
                /* The pull: an UPDATE carries nothing, the text comes only
                 * after our REQUEST. `should_request` is what keeps us from
                 * sending one per UPDATE when several arrive in a burst.
                 *
                 * CLIP4: and `auto_request` is what a one-way direction turns
                 * off. Not answering is the whole point - see clip_tcp.h. */
                if (!c->auto_request) { c->st.rx_updates_ignored++; break; }
                if (clip_chan_should_request(&c->chan, now_ms_u32()))
                    (void)clip_tcp_request(c);
                break;

            case CLIP_EV_REQUEST:
                /* The VM is asking for OUR clipboard. Core owns none, so this
                 * is handed to whoever does. We do NOT answer an empty REPLY in
                 * its place: a zero-length REPLY CLEARS the VM's clipboard, so
                 * silence is the only harmless answer when nobody is listening.
                 *
                 * CLIP4: `req_cb` is that listener. It must not answer inline -
                 * see clip_tcp.h. With no handler, the old once-only line
                 * stands, because it is then still exactly true. */
                c->st.rx_asked++;
                if (c->req_cb) {
                    c->req_cb(c->req_user);
                } else {
                    static int said = 0;
                    if (!said) {
                        said = 1;
                        cliplog("clip: the VM asked for our clipboard; core has "
                                "none to give. A client that owns one should "
                                "answer with clip_tcp_send_text()");
                    }
                }
                break;

            case CLIP_EV_TEXT:
                /* The stale-text trap: the VM announces an UPDATE even when its
                 * clipboard holds no text, then replies with its previous
                 * cache. Delivering that would make a client re-paste old
                 * content silently. */
                if (clip_chan_text_is_new(&c->chan, evs[i].text, evs[i].text_len)) {
                    clip_chan_note_text(&c->chan, evs[i].text, evs[i].text_len);
                    c->st.rx_texts++;
                    if (c->cb) c->cb(evs[i].text, evs[i].text_len, c->user);
                } else {
                    c->st.rx_stale++;
                }
                break;

            case CLIP_EV_BAD_UTF8:
            case CLIP_EV_OVERSIZE:
            case CLIP_EV_UNKNOWN:
                c->st.rx_bad++;
                break;

            case CLIP_EV_FLUSH:
                /* The server is not believed to send this (C85). Counted, not
                 * acted on: if it ever appears, the count is the evidence. */
                c->st.rx_bad++;
                break;

            case CLIP_EV_PROTO_ERROR:
                /* A header we refuse gives no length, so there is no next
                 * boundary to find: the only correct action is to stop. */
                cliplog("clip: protocol error, the stream cannot be resynced - "
                        "closing the channel");
                c->stop = 1;
                return;

            default:
                break;
        }
    }
}

static void *rx_thread(void *arg)
{
    clip_tcp_t *c = (clip_tcp_t *)arg;
    uint8_t net[16384];

    while (!c->stop && !(c->abort_flag && *c->abort_flag)) {
        /* The socket carries SO_RCVTIMEO (set at open), so a quiet channel
         * returns WANT_READ about every 100 ms and the abort flag is honoured
         * within that bound. A thread that polls less often leaks into HOS on
         * process exit and only a reboot clears it (CLAUDE.md). */
        const int n = wolfSSL_read(c->tls.ssl, net, (int)sizeof net);
        if (n <= 0) {
            const int err = wolfSSL_get_error(c->tls.ssl, n);
            const int se  = shadow_sock_errno();
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE
                || se == EAGAIN || se == EWOULDBLOCK)
                continue;                /* just a quiet channel */
            break;                       /* peer closed, or a real error */
        }

        c->st.rx_bytes += (uint64_t)n;

        /* `clip_chan_feed` returns what it consumed, which is less than `n`
         * when the event array fills. Not re-feeding the remainder loses bytes
         * and desynchronises the stream, so the loop is on the return value. */
        size_t off = 0;
        while (off < (size_t)n && !c->stop) {
            clip_ev_t evs[8];
            size_t n_evs = 0;
            const size_t used = clip_chan_feed(&c->chan, net + off, (size_t)n - off,
                                               evs, 8, &n_evs);
            handle_events(c, evs, n_evs);
            if (used == 0) break;        /* cannot progress: PROTO_ERROR */
            off += used;
        }
    }
    return NULL;
}

int clip_tcp_open(clip_tcp_t **out, const char *host, int port,
                  clip_tcp_text_cb cb, void *user,
                  const volatile int *abort_flag)
{
    if (!out || !host || port <= 0 || port > 65535) return -1;
    *out = NULL;

    clip_tcp_t *c = (clip_tcp_t *)calloc(1, sizeof *c);
    if (!c) return -1;
    c->cb = cb; c->user = user; c->abort_flag = abort_flag;
    c->auto_request = 1;   /* CLIP4: pulled by default; a direction turns it off */

    c->buf = (uint8_t *)malloc(CLIP_BUF_CAP);
    if (!c->buf) { free(c); return -1; }

    clip_chan_cfg_t cfg;
    clip_chan_cfg_from_env(&cfg);
    if (!clip_chan_init(&c->chan, c->buf, CLIP_BUF_CAP, &cfg)) {
        free(c->buf); free(c); return -1;
    }
    if (pthread_mutex_init(&c->tx_lock, NULL) != 0) {
        free(c->buf); free(c); return -1;
    }

    if (!tls_chan_open(&c->tls, host, port, 5000, "clip")) {
        cliplog("clip: TLS open FAILED on :%d", port);
        pthread_mutex_destroy(&c->tx_lock);
        free(c->buf); free(c);
        return -1;
    }

    /* A recv timeout on the socket, so the receive thread wakes often enough to
     * see the abort flag and `clip_tcp_close` never deadlocks on a blocking
     * read. Same value and same reason as the video TCP channel. */
    {
#ifdef _WIN32
        DWORD tv = 100;
        setsockopt(c->tls.sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
#else
        struct timeval tv = {0, 100 * 1000};
        setsockopt(c->tls.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    }

    /* CONNECT FIRST, and this is not a formality: the server's
     * `IsThisMessageMine` returns true only for opcode 0, and that return is
     * what binds this socket to the clipboard engine. Any other first message
     * leaves the channel open, the TLS valid, and every byte routed nowhere -
     * with no error on either side. */
    if (!send_simple(c, CLIP_OP_CONNECT)) {
        cliplog("clip: CONNECT send FAILED on :%d - the channel would be deaf", port);
        tls_chan_close(&c->tls);
        pthread_mutex_destroy(&c->tx_lock);
        free(c->buf); free(c);
        return -1;
    }

    if (pthread_create(&c->rx, NULL, rx_thread, c) != 0) {
        cliplog("clip: pthread_create FAILED");
        tls_chan_close(&c->tls);
        pthread_mutex_destroy(&c->tx_lock);
        free(c->buf); free(c);
        return -1;
    }
    c->rx_started = 1;

    cliplog("clip: channel open on :%d, CONNECT sent, cap %u bytes",
            port, (unsigned)cfg.cap);
    *out = c;
    return 0;
}

void clip_tcp_close(clip_tcp_t *c)
{
    if (!c) return;
    c->stop = 1;
    if (c->rx_started) pthread_join(c->rx, NULL);
    tls_chan_close(&c->tls);
    pthread_mutex_destroy(&c->tx_lock);
    cliplog("clip: closed - updates=%u (ignored=%u) texts=%u stale=%u asked=%u "
            "requests=%u replies=%u bad=%u bytes=%llu",
            c->st.rx_updates, c->st.rx_updates_ignored, c->st.rx_texts,
            c->st.rx_stale, c->st.rx_asked,
            c->st.tx_requests, c->st.tx_replies, c->st.rx_bad,
            (unsigned long long)c->st.rx_bytes);
    free(c->buf);
    free(c);
}

void clip_tcp_set_auto_request(clip_tcp_t *c, bool on)
{
    if (c) c->auto_request = on ? 1 : 0;
}

void clip_tcp_set_request_cb(clip_tcp_t *c, clip_tcp_request_cb cb, void *user)
{
    if (!c) return;
    c->req_cb   = cb;
    c->req_user = user;
}

void clip_tcp_get_stats(const clip_tcp_t *c, clip_tcp_stats_t *out)
{
    if (!out) return;
    if (!c) { memset(out, 0, sizeof *out); return; }
    *out = c->st;
}
