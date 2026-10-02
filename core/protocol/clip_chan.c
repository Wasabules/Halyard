/* clip_chan.c - CLIP 2026-10-02. See clip_chan.h for the why.
 *
 * No socket, no thread, no logging. The only impure function is
 * clip_chan_cfg_from_env(), which is why it is at the top and alone. */

#include "clip_chan.h"

#include <stdlib.h>   /* getenv, strtoul - the SHADOW_CLIP_* toggles */
#include <string.h>   /* memcpy */

void clip_chan_cfg_default(clip_chan_cfg_t *cfg)
{
    if (!cfg) return;
    cfg->cap                = CLIP_WIRE_CAP_DEFAULT;
    cfg->request_timeout_ms = CLIP_CHAN_REQUEST_TIMEOUT_MS;
    cfg->strict             = true;
}

void clip_chan_cfg_from_env(clip_chan_cfg_t *cfg)
{
    if (!cfg) return;
    clip_chan_cfg_default(cfg);

    const char *e = getenv("SHADOW_CLIP_STRICT");
    if (e) cfg->strict = (atoi(e) != 0);

    e = getenv("SHADOW_CLIP_MAX");
    if (e) {
        /* `strtoul` and not `atoi`: 4194304 fits an int, but a user typing a
         * larger number into `env.txt` would get a negative int and, after the
         * cast, a cap of nearly 4 GiB - a toggle meant to tighten the bound
         * would have removed it. */
        const unsigned long v = strtoul(e, NULL, 10);
        /* 0 restores the default rather than refusing every message: a toggle
         * must not be able to switch the feature off by arithmetic accident. */
        if (v > 0 && v <= 0xFFFFFFFFul) cfg->cap = (uint32_t)v;
    }
}

bool clip_chan_init(clip_chan_t *st, uint8_t *buf, size_t buf_cap,
                    const clip_chan_cfg_t *cfg)
{
    if (!st || !buf || buf_cap == 0) return false;

    memset(st, 0, sizeof(*st));
    if (cfg) st->cfg = *cfg;
    else     clip_chan_cfg_default(&st->cfg);
    if (st->cfg.cap == 0) st->cfg.cap = CLIP_WIRE_CAP_DEFAULT;

    st->buf     = buf;
    st->buf_cap = buf_cap;
    st->state   = CLIP_CACHE_IDLE;
    return true;
}

void clip_chan_reset(clip_chan_t *st)
{
    if (!st) return;
    st->hdr_have           = 0;
    st->state              = CLIP_CACHE_IDLE;
    st->announced          = 0;
    st->received           = 0;
    st->complete_pending   = false;
    st->update_outstanding = false;
    st->request_in_flight  = false;
    st->request_sent_ms    = 0;
    /* `last_text_*` deliberately SURVIVES a reset. It answers "has the user's
     * clipboard content changed", a question that does not become open again
     * because a socket died. Clearing it would make every reconnect re-paste
     * the same text. */
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;      /* FNV-1a 64 offset basis */
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Pushes one event. Returns false when the array is full, which is what makes
 * clip_chan_feed stop and report a partial consumption. */
static bool emit(clip_ev_t *evs, size_t max_evs, size_t *n_evs,
                 const clip_ev_t *ev)
{
    if (!evs || *n_evs >= max_evs) return false;
    evs[(*n_evs)++] = *ev;
    return true;
}

/* A complete header is in st->hdr. Decides what happens next and emits.
 * Returns false if the event array is full (nothing was consumed, the header
 * stays staged) or on a protocol error. */
static bool on_header(clip_chan_t *st, clip_ev_t *evs, size_t max_evs,
                      size_t *n_evs, bool *fatal)
{
    clip_wire_header_t h;
    clip_ev_t ev;
    memset(&ev, 0, sizeof(ev));

    /* The cap handed to the parser is the CONFIGURED one, not the buffer size.
     * The two are different limits: `cfg.cap` says what the protocol may
     * legitimately carry, `buf_cap` says what we happen to have room for. A
     * payload over `cfg.cap` is a refusal of the header (we cannot trust the
     * length at all); one that merely exceeds `buf_cap` is a message we
     * understand and choose to skip, and skipping requires the length. Merging
     * them would turn "my buffer is small today" into "drop the connection". */
    if (!clip_wire_parse_header(st->hdr, CLIP_WIRE_HEADER_LEN,
                                st->cfg.cap, st->cfg.strict, &h)) {
        ev.kind = CLIP_EV_PROTO_ERROR;
        (void)emit(evs, max_evs, n_evs, &ev);
        *fatal = true;
        return false;
    }

    ev.opcode    = h.opcode;
    ev.announced = h.payload_len;

    if (!clip_wire_opcode_known(h.opcode)) {
        /* The server consumes an unknown type and keeps going - "process
         * unknown message of type 0x%x." with no SetInvalid. We do the same:
         * the header is well-formed, so the declared length tells us exactly
         * where the next message starts. Refusing here would desynchronise on
         * a protocol addition we would otherwise survive. */
        ev.kind = CLIP_EV_UNKNOWN;
        if (!emit(evs, max_evs, n_evs, &ev)) return false;
        st->hdr_have  = 0;
        st->announced = h.payload_len;
        st->received  = 0;
        st->state     = h.payload_len ? CLIP_CACHE_SKIPPING : CLIP_CACHE_IDLE;
        return true;
    }

    if (h.opcode == CLIP_OP_REPLY_TEXT) {
        if ((size_t)h.payload_len > st->buf_cap) {
            ev.kind = CLIP_EV_OVERSIZE;
            if (!emit(evs, max_evs, n_evs, &ev)) return false;
            st->hdr_have  = 0;
            st->announced = h.payload_len;
            st->received  = 0;
            st->state     = h.payload_len ? CLIP_CACHE_SKIPPING : CLIP_CACHE_IDLE;
            /* The pull is finished either way: the VM has answered. Leaving the
             * update outstanding would make us request the same oversized
             * clipboard again, for ever. */
            st->update_outstanding = false;
            st->request_in_flight  = false;
            return true;
        }
        st->hdr_have  = 0;
        st->announced = h.payload_len;
        st->received  = 0;
        /* A zero-length REPLY_TEXT is a real message - the VM's clipboard is
         * empty - so it completes immediately rather than waiting for bytes
         * that will never come. It goes through the same `complete_pending`
         * path as a reassembled one, so there is one completion path and not
         * two. */
        if (h.payload_len == 0) {
            st->state            = CLIP_CACHE_IDLE;
            st->complete_pending = true;
        } else {
            st->state = CLIP_CACHE_WAITING;
        }
        return true;
    }

    /* FLUSH / UPDATE / REQUEST / CONNECT: ten bytes, no payload. A non-zero
     * length on one of those is not something either side produces; we trust
     * the length anyway and skip it, because the length is the only thing that
     * can put us back on a message boundary. */
    switch (h.opcode) {
        case CLIP_OP_FLUSH:   ev.kind = CLIP_EV_FLUSH;  break;
        case CLIP_OP_UPDATE:  ev.kind = CLIP_EV_UPDATE; break;
        case CLIP_OP_REQUEST: ev.kind = CLIP_EV_REQUEST; break;
        default:              ev.kind = CLIP_EV_UNKNOWN; break;  /* CONNECT */
    }
    if (h.opcode == CLIP_OP_CONNECT) {
        /* The VM never sends CONNECT: nothing in `StfpClient::handleApplication_`
         * @0x140c2f110 produces opcode 0 - its subtypes map to FLUSH, UPDATE
         * and REPLY_TEXT only. CONNECT travels client -> server, as the demux
         * key. One arriving at us is not ours to interpret, so it is reported
         * as UNKNOWN rather than silently swallowed. */
        ev.kind = CLIP_EV_UNKNOWN;
    }

    if (!emit(evs, max_evs, n_evs, &ev)) return false;

    if (ev.kind == CLIP_EV_UPDATE) st->update_outstanding = true;

    st->hdr_have  = 0;
    st->announced = h.payload_len;
    st->received  = 0;
    st->state     = h.payload_len ? CLIP_CACHE_SKIPPING : CLIP_CACHE_IDLE;
    return true;
}

size_t clip_chan_feed(clip_chan_t *st, const uint8_t *data, size_t n,
                      clip_ev_t *evs, size_t max_evs, size_t *n_evs)
{
    size_t dummy = 0;
    if (!n_evs) n_evs = &dummy;
    *n_evs = 0;

    if (!st || !st->buf || (!data && n != 0)) return 0;

    size_t used = 0;
    bool   fatal = false;

    for (;;) {
        /* 0. A completed REPLY_TEXT whose event is still undelivered. It is
         * drained before anything else and before the `used < n` test, so that
         * the last message of a read is reported even when it ends exactly on
         * the read's last byte. */
        if (st->complete_pending) {
            clip_ev_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.opcode    = (uint16_t)CLIP_OP_REPLY_TEXT;
            ev.announced = st->announced;
            ev.text      = st->buf;
            ev.text_len  = (size_t)st->announced;
            /* Valid UTF-8 is checked HERE and not left to the caller, because
             * the only parser that will ever object is ours: the VM's
             * `MultiByteToWideChar(CP_UTF8, 0, ...)` carries no
             * MB_ERR_INVALID_CHARS and substitutes U+FFFD without a word. */
            ev.kind = clip_wire_utf8_valid(st->buf, (size_t)st->announced)
                    ? CLIP_EV_TEXT : CLIP_EV_BAD_UTF8;

            if (!emit(evs, max_evs, n_evs, &ev)) break;   /* retry next call */
            st->complete_pending = false;
            /* The pull is answered either way - a malformed reply is still a
             * reply, and asking again would fetch the same bytes. */
            st->update_outstanding = false;
            st->request_in_flight  = false;
        }

        if (used >= n || fatal) break;

        /* 1. A payload is outstanding: it comes first, with no header in front
         * of it. The VM streams REPLY_TEXT content raw after its header - this
         * is the same order as `StfpClient::DealWithInput`, whose cache loop
         * runs BEFORE it ever looks at a header. */
        if (st->state == CLIP_CACHE_WAITING || st->state == CLIP_CACHE_SKIPPING) {
            const uint32_t want = st->announced - st->received;
            size_t take = n - used;
            if (take > (size_t)want) take = (size_t)want;

            if (st->state == CLIP_CACHE_WAITING)
                memcpy(st->buf + st->received, data + used, take);

            st->received += (uint32_t)take;
            used         += take;

            if (st->received < st->announced) break;   /* still "waiting" */

            const bool was_skip = (st->state == CLIP_CACHE_SKIPPING);
            st->state = CLIP_CACHE_IDLE;
            if (!was_skip) st->complete_pending = true;
            continue;
        }

        /* 2. No payload outstanding: fill the ten-byte header. */
        const uint8_t prev_have = st->hdr_have;
        const size_t  need = (size_t)CLIP_WIRE_HEADER_LEN - st->hdr_have;
        size_t take = n - used;
        if (take > need) take = need;
        memcpy(st->hdr + st->hdr_have, data + used, take);
        st->hdr_have += (uint8_t)take;
        used         += take;

        if (st->hdr_have < CLIP_WIRE_HEADER_LEN) break;   /* split header */

        if (!on_header(st, evs, max_evs, n_evs, &fatal)) {
            /* The event array is full. REWIND: the bytes that completed this
             * header are given back, so the module has exactly ONE contract -
             * "a return below `n` means drain the events and feed the rest" -
             * and the caller never has to know that a header can be staged.
             *
             * This is not cosmetic. Keeping them staged and counted as consumed
             * is also correct, but then a caller that re-feeds the tail (the
             * obvious reading of a partial return) would stage the same header
             * twice and read the stream one message out of step from there on.
             * That failure produces no error at all, which is this channel's
             * whole history. A payload already memcpy'd into `buf` cannot be
             * rewound the same way - hence `complete_pending` for that case.
             *
             * `fatal` is different: a refused header has no length, so there is
             * no next boundary and nothing to rewind to. Its bytes stay
             * consumed and the caller drops the connection. */
            if (!fatal) {
                st->hdr_have = prev_have;
                used        -= take;
            }
            break;
        }
    }

    return used;
}

bool clip_chan_should_request(const clip_chan_t *st, uint32_t now_ms)
{
    if (!st || !st->update_outstanding) return false;
    if (!st->request_in_flight) return true;
    if (st->cfg.request_timeout_ms == 0) return false;
    /* Unsigned subtraction, so a millisecond counter that wraps at 2^32 (49.7
     * days of uptime, which a console reaches) still yields the right elapsed
     * time instead of an enormous one. */
    return (uint32_t)(now_ms - st->request_sent_ms) >= st->cfg.request_timeout_ms;
}

void clip_chan_note_request_sent(clip_chan_t *st, uint32_t now_ms)
{
    if (!st) return;
    st->request_in_flight = true;
    st->request_sent_ms   = now_ms;
}

bool clip_chan_text_is_new(const clip_chan_t *st, const uint8_t *text, size_t n)
{
    if (!st) return false;
    if (!st->have_last_text) return true;
    /* The length comparison is the one line of this module that NO test can
     * pin: removing it changes nothing observable, because two texts of
     * different lengths already hash differently. It stays because it makes the
     * answer exact rather than probabilistic - on a hash collision between two
     * different lengths it is the length that decides - and it is recorded here
     * so that nobody deletes it as dead weight on the strength of a passing
     * suite. Measured: the CLIP mutation run detected 10 of 11 mutations, and
     * this was the eleventh. */
    if (st->last_text_len != n) return true;
    return st->last_text_hash != fnv1a(text, n);
}

void clip_chan_note_text(clip_chan_t *st, const uint8_t *text, size_t n)
{
    if (!st) return;
    st->last_text_hash = fnv1a(text, n);
    st->last_text_len  = n;
    st->have_last_text = true;
}
