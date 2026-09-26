/* See ann_reply.h for the wire and for why this module exists. */
#include "ann_reply.h"
#include "proto.h"

#include <string.h>

/* Reads every field of a message, calling `fn` for each. A field we do not know
 * is skipped rather than treated as an error: the server is free to add fields,
 * and a reply we half understand is worth more than one we refuse. */
typedef void (*field_fn)(uint32_t field, uint32_t wire,
                         const uint8_t *buf, size_t len, int off, void *user);

static void walk(const uint8_t *buf, size_t len, field_fn fn, void *user)
{
    int off = 0;
    while (off >= 0 && (size_t)off < len) {
        uint32_t field = 0, wire = 0;
        int next = pb_read_tag(buf, len, off, &field, &wire);
        if (next < 0) return;
        fn(field, wire, buf, len, next, user);
        {
            const int skipped = pb_skip_field(buf, len, next, wire);
            /* `pb_skip_field` returning a NON-INCREASING offset is how a
             * malformed message becomes an infinite loop - the defect that hung
             * the control thread on a 12-byte message (KB §9, 2026-08-25). The
             * test is here and not only in `proto.c` because this walker is the
             * one that would spin. */
            if (skipped <= off) return;
            off = skipped;
        }
    }
}

static uint64_t varint_at(const uint8_t *buf, size_t len, int off)
{
    uint64_t v = 0;
    return (pb_read_varint(buf, len, off, &v) < 0) ? 0 : v;
}

static float fixed32_at(const uint8_t *buf, size_t len, int off)
{
    uint32_t bits;
    float f;
    if (off < 0 || (size_t)off + 4 > len) return 0.0f;
    bits = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8)
         | ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* ── StreamingProtocol (SessionInfo.f4) ───────────────────────────────────── */

static void on_protocol(uint32_t field, uint32_t wire,
                        const uint8_t *buf, size_t len, int off, void *user)
{
    ann_reply_t *r = (ann_reply_t *)user;
    if (wire != 0) return;                       /* all of these are varints */
    switch (field) {
        case 1: r->protocol  = (int)varint_at(buf, len, off); break;
        case 3: r->tcp       = varint_at(buf, len, off) != 0;  break;
        case 4: r->encrypted = varint_at(buf, len, off) != 0;  break;
        case 5: r->port      = (uint32_t)varint_at(buf, len, off); break;
        default: break;
    }
}

/* ── SessionInfo (the f1 of every channel body) ───────────────────────────── */

static void on_session(uint32_t field, uint32_t wire,
                       const uint8_t *buf, size_t len, int off, void *user)
{
    ann_reply_t *r = (ann_reply_t *)user;
    if (field == 2 && wire == 0) { r->handle = varint_at(buf, len, off); return; }
    if (field == 4 && wire == 2) {
        const uint8_t *sub; size_t sl;
        if (pb_read_lendelim(buf, len, off, &sub, &sl) >= 0)
            walk(sub, sl, on_protocol, r);
    }
}

/* ── Video mode (video body f2) ───────────────────────────────────────────── */

static void on_mode(uint32_t field, uint32_t wire,
                    const uint8_t *buf, size_t len, int off, void *user)
{
    ann_reply_t *r = (ann_reply_t *)user;
    if (field == 1 && wire == 0) r->width  = (uint32_t)varint_at(buf, len, off);
    else if (field == 2 && wire == 0) r->height = (uint32_t)varint_at(buf, len, off);
    else if (field == 3 && wire == 5) r->fps = fixed32_at(buf, len, off);
}

/* ── One channel's body ───────────────────────────────────────────────────── */

static void on_channel_body(uint32_t field, uint32_t wire,
                            const uint8_t *buf, size_t len, int off, void *user)
{
    ann_reply_t *r = (ann_reply_t *)user;

    if (field == 1 && wire == 2) {
        const uint8_t *sub; size_t sl;
        if (pb_read_lendelim(buf, len, off, &sub, &sl) >= 0) {
            r->have_session = 1;
            walk(sub, sl, on_session, r);
        }
        return;
    }

    switch (r->channel) {
        case ANN_CHAN_VIDEO:
            if (field == 2 && wire == 2) {
                const uint8_t *sub; size_t sl;
                if (pb_read_lendelim(buf, len, off, &sub, &sl) >= 0) {
                    r->have_mode = 1;
                    walk(sub, sl, on_mode, r);
                }
            }
            else if (field == 3 && wire == 0) r->codec = (uint32_t)varint_at(buf, len, off);
            else if (field == 5 && wire == 0) r->color444 = varint_at(buf, len, off) != 0;
            else if (field == 7 && wire == 0) r->bitrate_bps = (uint32_t)varint_at(buf, len, off);
            break;

        case ANN_CHAN_AUDIO:
        case ANN_CHAN_MICRO:
            if (field == 2 && wire == 0) { r->have_audio = 1; r->sample_rate = (uint32_t)varint_at(buf, len, off); }
            else if (field == 3 && wire == 0) r->bits = (uint32_t)varint_at(buf, len, off);
            else if (field == 4 && wire == 0) r->audio_codec = (uint32_t)varint_at(buf, len, off);
            break;

        /* === FILETRANSFER: ITS FIELD 2 IS A PRIVATE KEY ===
         *
         * Deliberately not read. There is nothing here we need from it, and the
         * only way to be sure it is never logged is never to copy it out. */
        case ANN_CHAN_FILEXFER:
        default:
            break;
    }
}

/* ── ChannelInfo: the field NUMBER is the channel ─────────────────────────── */

static void on_channel_info(uint32_t field, uint32_t wire,
                            const uint8_t *buf, size_t len, int off, void *user)
{
    ann_reply_t *r = (ann_reply_t *)user;
    const uint8_t *sub; size_t sl;
    if (wire != 2 || field < 1 || field > 8) return;
    if (r->channel != ANN_CHAN_NONE) return;     /* exactly one is set */
    if (pb_read_lendelim(buf, len, off, &sub, &sl) < 0) return;
    r->channel = (ann_channel_t)field;
    walk(sub, sl, on_channel_body, r);
}

static void on_response(uint32_t field, uint32_t wire,
                        const uint8_t *buf, size_t len, int off, void *user)
{
    const uint8_t *sub; size_t sl;
    if (field != 8 || wire != 2) return;         /* f3.8 = ChannelInfo */
    if (pb_read_lendelim(buf, len, off, &sub, &sl) < 0) return;
    walk(sub, sl, on_channel_info, user);
}

static void note_response_field(uint32_t field, uint32_t wire,
                                const uint8_t *buf, size_t len, int off, void *user)
{
    ann_reply_t *r = (ann_reply_t *)user;
    (void)buf; (void)len; (void)off; (void)wire;
    if (r->response_field == 0) r->response_field = field;
}

static void on_top(uint32_t field, uint32_t wire,
                   const uint8_t *buf, size_t len, int off, void *user)
{
    const uint8_t *sub; size_t sl;
    if (field == 1 && wire == 0) {               /* f1 = le seq renvoye */
        uint64_t v = 0;
        if (pb_read_varint(buf, len, off, &v) >= 0)
            ((ann_reply_t *)user)->seq = (uint32_t)v;
        return;
    }
    if (field != 3 || wire != 2) return;         /* f3 = the response payload */
    if (pb_read_lendelim(buf, len, off, &sub, &sl) < 0) return;
    /* The type first, so a reply we decline to decode can still name itself. */
    walk(sub, sl, note_response_field, user);
    walk(sub, sl, on_response, user);
}

int ann_reply_parse(const uint8_t *body, size_t len, ann_reply_t *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!body || len == 0) return 0;
    walk(body, len, on_top, out);
    return out->channel != ANN_CHAN_NONE;
}

int ann_reply_port_offset(const ann_reply_t *r)
{
    if (!r || r->port == 0) return -1;
    return (int)(r->port % 1000u);
}

const char *ann_reply_channel_name(ann_channel_t c)
{
    switch (c) {
        case ANN_CHAN_VIDEO:     return "video";
        case ANN_CHAN_AUDIO:     return "audio";
        case ANN_CHAN_INPUT:     return "input";
        case ANN_CHAN_CURSOR:    return "curseur";
        case ANN_CHAN_MICRO:     return "micro";
        case ANN_CHAN_GAMEPAD:   return "manette";
        case ANN_CHAN_CLIPBOARD: return "presse-papier";
        case ANN_CHAN_FILEXFER:  return "transfert";
        default:                 return "?";
    }
}
