#include "sufp.h"

#include <stdlib.h>
#include <string.h>

#include "../common/log.h"

/* S81 — the category is DECLARED here, not inferred from the message text.
 * `slog` stays at INFO: the existing calls do not disappear. `sdbg` is there for
 * the bulky lines, which move over to it one at a time. */
#define slog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
#define sdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
/* State of a frame being reassembled */
typedef struct {
    bool      used;
    uint32_t  frame_id;
    uint8_t   subchan;
    /* NUMBER of expected chunks (= the wire's `max` field + 1, see S32). Named
     * `chunk_count` and not `max_chunks`: the wire field is the LAST INDEX, and
     * confusing the two is exactly what G4 cost. */
    uint16_t  chunk_count;
    uint16_t  recv_count;
    bool      slot_filled[SUFP_MAX_CHUNKS_PER_FRAME];
    uint8_t  *chunks[SUFP_MAX_CHUNKS_PER_FRAME];
    size_t    chunk_lens[SUFP_MAX_CHUNKS_PER_FRAME];
    size_t    total_size;
} sufp_frame;

struct sufp_reasm {
    sufp_frame    window[SUFP_WINDOW_SIZE];
    uint8_t      *out_buf;
    size_t        out_cap;
    sufp_frame_cb cb;
    void         *user;
};

static inline uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
}
static inline void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void frame_clear(sufp_frame *f) {
    if (!f) return;
    for (int i = 0; i < SUFP_MAX_CHUNKS_PER_FRAME; i++) {
        free(f->chunks[i]);
        f->chunks[i] = NULL;
        f->chunk_lens[i] = 0;
        f->slot_filled[i] = false;
    }
    f->used = false;
    f->frame_id = 0;
    f->subchan = 0;
    f->chunk_count = 0;
    f->recv_count = 0;
    f->total_size = 0;
}

sufp_reasm *sufp_create(sufp_frame_cb cb, void *user) {
    sufp_reasm *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->cb = cb;
    r->user = user;
    /* Reused output buffer (= zero allocation per completed frame) */
    r->out_cap = SUFP_MAX_FRAME_BYTES;
    r->out_buf = malloc(r->out_cap);
    if (!r->out_buf) { free(r); return NULL; }
    return r;
}

void sufp_destroy(sufp_reasm *r) {
    if (!r) return;
    for (int i = 0; i < SUFP_WINDOW_SIZE; i++) frame_clear(&r->window[i]);
    free(r->out_buf);
    free(r);
}

/* Finds a slot for this frame_id, or allocates a free one / reuses the oldest */
static sufp_frame *find_or_alloc_slot(sufp_reasm *r, uint32_t frame_id,
                                        uint8_t subchan, uint16_t chunk_count) {
    /* Match existing */
    for (int i = 0; i < SUFP_WINDOW_SIZE; i++) {
        if (r->window[i].used && r->window[i].frame_id == frame_id) {
            if (r->window[i].chunk_count != chunk_count
                || r->window[i].subchan != subchan) {
                slog("sufp: frame_id=%u incoherent (count=%u/%u sub=%u/%u)",
                     frame_id, r->window[i].chunk_count, chunk_count,
                     r->window[i].subchan, subchan);
                frame_clear(&r->window[i]);
                continue;
            }
            return &r->window[i];
        }
    }
    /* Alloc free */
    for (int i = 0; i < SUFP_WINDOW_SIZE; i++) {
        if (!r->window[i].used) {
            sufp_frame *f = &r->window[i];
            f->used = true;
            f->frame_id = frame_id;
            f->subchan = subchan;
            f->chunk_count = chunk_count;
            return f;
        }
    }
    /* Window full: reuse the slot with the smallest frame_id (= the oldest) */
    int oldest = 0;
    uint32_t oldest_id = r->window[0].frame_id;
    for (int i = 1; i < SUFP_WINDOW_SIZE; i++) {
        if (r->window[i].frame_id < oldest_id) {
            oldest_id = r->window[i].frame_id;
            oldest = i;
        }
    }
    /* Log only 1 eviction in 100, to avoid spamming. */
    static uint32_t evict_count = 0;
    if (((++evict_count) % 100) == 0)
        slog("sufp: window full evict_count=%u (1 log/100), oldest_id=%u",
             evict_count, oldest_id);
    frame_clear(&r->window[oldest]);
    sufp_frame *f = &r->window[oldest];
    f->used = true;
    f->frame_id = frame_id;
    f->subchan = subchan;
    f->chunk_count = chunk_count;
    return f;
}

static int complete_frame(sufp_reasm *r, sufp_frame *f) {
    /* Concatenate chunks 1..max into out_buf */
    size_t off = 0;
    for (int i = 0; i < f->chunk_count; i++) {
        if (!f->slot_filled[i]) {
            slog("sufp: complete_frame missing chunk %d/%d", i + 1, f->chunk_count);
            frame_clear(f);
            return -1;
        }
        if (off + f->chunk_lens[i] > r->out_cap) {
            slog("sufp: frame too large (>%zu)", r->out_cap);
            frame_clear(f);
            return -1;
        }
        memcpy(r->out_buf + off, f->chunks[i], f->chunk_lens[i]);
        off += f->chunk_lens[i];
    }
    uint32_t fid = f->frame_id;
    uint8_t sub = f->subchan;
    if (r->cb) r->cb(sub, fid, r->out_buf, off, r->user);
    frame_clear(f);
    return 1;
}

int sufp_feed(sufp_reasm *r, const uint8_t *buf, size_t len) {
    if (!r || !buf || len < 1) return -1;
    uint8_t b0 = buf[0];
    uint8_t pkt_type = b0 & 0x0f;
    /* uint8_t version = (b0 >> 4) & 0x0f; */

    if (pkt_type == SUFP_PKT_TYPE_PING) {
        /* RTT measurement — not handled here, we just return 2 */
        return 2;
    }
    if (pkt_type < SUFP_PKT_TYPE_DATA1 || pkt_type > SUFP_PKT_TYPE_DATA3) {
        return -1;
    }

    size_t hdr = (pkt_type == SUFP_PKT_TYPE_DATA3) ? SUFP_HDR_TYPE3 : SUFP_HDR_LEGACY;
    if (len <= hdr) return -1;

    uint8_t  subchan    = buf[1];
    uint16_t chunk_idx  = rd_le16(buf + 2);
    uint16_t max_chunks = rd_le16(buf + 4);
    uint32_t frame_id   = rd_le32(buf + 6);
    /* uint8_t  flags  = (pkt_type == SUFP_PKT_TYPE_DATA3) ? buf[10] : 0; */

    /* N58 2026-05-14: the chunks are 0-based, not 1-based. The MASTER capture
     * showed 736 chunks (12 %) at chunk_idx=0 that we were dropping — that is
     * the START of every picture's NAL. (The upper bound was corrected by S32
     * below: it is max+1, not max.) */
    /* === S32 2026-08-25 — `max` IS THE LAST INDEX, HERE TOO ===
     * This field was treated as a COUNT (valid indices 0..max-1). On the VIDEO
     * wire, G4 then G21 proved it is the LAST INDEX: the server sends 0..max,
     * i.e. max+1 chunks. Getting it wrong cost every picture's tail and 626
     * ffmpeg errors.
     *
     * PROOF that the CURSOR channel follows the same rule, with no new capture:
     * a single-packet cursor frame carries `max_chunks == 0`
     * (`ctrl_session.c::on_cursor_packet`, the direct path's condition,
     * described as a "self-contained sentinel"). Under COUNT semantics that
     * would read "zero chunks" — incoherent. Under LAST INDEX semantics it reads
     * "last index 0", i.e. exactly one chunk. It is the same SUFP v3 header,
     * from the same server, on two channels: the encoding does not change from
     * one channel to another.
     *
     * CONSEQUENCE of the old behaviour: a two-chunk cursor frame announces
     * max=1; we rejected index 1, then `recv_count == max_chunks` (1 == 1)
     * declared the picture complete — it was delivered missing its second half.
     * Invisible until now because the bitmap accessors have no consumer, but
     * wrong all the same.
     *
     * SHADOW_SUFP_MAX_PLUS1=0 restores the COUNT semantics. */
    static int g_sufp_plus1 = -1;
    if (g_sufp_plus1 < 0) {
        const char *e = getenv("SHADOW_SUFP_MAX_PLUS1");
        g_sufp_plus1 = e ? atoi(e) : 1;
    }
    uint16_t chunk_count = g_sufp_plus1 ? (uint16_t)(max_chunks + 1) : max_chunks;
    if (chunk_count == 0) return -1;          /* max=0xFFFF under +1, or max=0 without */
    if (chunk_idx >= chunk_count) return -1;
    if (chunk_count > SUFP_MAX_CHUNKS_PER_FRAME) {
        slog("sufp: chunk_count=%u > SUFP_MAX_CHUNKS_PER_FRAME", chunk_count);
        return -1;
    }

    sufp_frame *f = find_or_alloc_slot(r, frame_id, subchan, chunk_count);
    if (!f) return -1;

    int slot_i = chunk_idx;  /* 0-indexed */
    if (f->slot_filled[slot_i]) {
        /* Duplicate chunk — drop silently */
        return 0;
    }
    size_t payload_len = len - hdr;
    f->chunks[slot_i] = malloc(payload_len);
    if (!f->chunks[slot_i]) return -1;
    memcpy(f->chunks[slot_i], buf + hdr, payload_len);
    f->chunk_lens[slot_i] = payload_len;
    f->slot_filled[slot_i] = true;
    f->recv_count++;
    f->total_size += payload_len;

    if (f->recv_count == f->chunk_count) {
        return complete_frame(r, f);
    }
    return 0;
}

int sufp_build_chunk_v3(uint8_t *out_buf, size_t out_cap,
                         uint8_t version, uint8_t subchan,
                         uint16_t chunk_idx, uint16_t max_chunks,
                         uint32_t frame_id, uint8_t flags,
                         const uint8_t *payload, size_t payload_len) {
    if (!out_buf || out_cap < SUFP_HDR_TYPE3 + payload_len) return -1;
    out_buf[0] = (uint8_t)((version << 4) | SUFP_PKT_TYPE_DATA3);
    out_buf[1] = subchan;
    wr_le16(out_buf + 2, chunk_idx);
    wr_le16(out_buf + 4, max_chunks);
    wr_le32(out_buf + 6, frame_id);
    out_buf[10] = flags;
    if (payload_len > 0) memcpy(out_buf + SUFP_HDR_TYPE3, payload, payload_len);
    return (int)(SUFP_HDR_TYPE3 + payload_len);
}

int sufp_build_ping(uint8_t *out_buf, size_t out_cap,
                     uint8_t subchan, uint32_t seq, uint32_t timestamp_us) {
    if (!out_buf || out_cap < SUFP_HDR_LEGACY) return -1;
    out_buf[0] = SUFP_PKT_TYPE_PING;  /* type=0xf, version=0 */
    out_buf[1] = subchan;
    wr_le32(out_buf + 2, seq);
    wr_le32(out_buf + 6, timestamp_us);
    return SUFP_HDR_LEGACY;
}
