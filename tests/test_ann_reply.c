/* test_ann_reply.c - the server's answer to a channel announcement
 * (core/protocol/ann_reply.c).
 *
 * This is the only place our code reads a message that STATES THE SESSION: the
 * granted transport, port offset, video mode, and audio codec. Getting it wrong
 * does not crash anything - it silently reports the wrong thing about the
 * session, which is the kind of defect this repo has been misled by twice.
 *
 * Two families of checks, and the second is why the file exists:
 *
 *   - the nominal replies decode field by field, including the one trap: the
 *     channel is the FIELD NUMBER inside f3.8, not the arrival order.
 *
 *   - THE KEY IS NEVER COPIED OUT. The eighth reply carries an OpenSSH ed25519
 *     private key in cleartext. The old code logged 160 bytes of every reply in
 *     hex and in ASCII, so a completed bootstrap wrote a live private key into
 *     the log file - and the network log mirror sent it over plain TCP. The
 *     check named [FUITE] builds a reply whose FileTransfer body holds a
 *     recognisable marker and then walks the ENTIRE output structure looking
 *     for it. If someone ever adds a field that reads that blob, this fails.
 */
#include "../core/protocol/ann_reply.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                   \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* ── A minimal protobuf encoder, to build replies ─────────────────────────── */

static size_t put_varint(uint8_t *b, size_t o, uint64_t v)
{
    while (v >= 0x80) { b[o++] = (uint8_t)(v | 0x80); v >>= 7; }
    b[o++] = (uint8_t)v;
    return o;
}
static size_t put_tag(uint8_t *b, size_t o, uint32_t f, uint32_t w)
{ return put_varint(b, o, ((uint64_t)f << 3) | w); }
static size_t put_uint(uint8_t *b, size_t o, uint32_t f, uint64_t v)
{ o = put_tag(b, o, f, 0); return put_varint(b, o, v); }
static size_t put_f32(uint8_t *b, size_t o, uint32_t f, float v)
{ uint32_t bits; memcpy(&bits, &v, 4); o = put_tag(b, o, f, 5);
  b[o++] = (uint8_t)bits; b[o++] = (uint8_t)(bits >> 8);
  b[o++] = (uint8_t)(bits >> 16); b[o++] = (uint8_t)(bits >> 24); return o; }
static size_t put_bytes(uint8_t *b, size_t o, uint32_t f, const uint8_t *d, size_t n)
{ o = put_tag(b, o, f, 2); o = put_varint(b, o, n); memcpy(b + o, d, n); return o + n; }

/* Wraps `inner` as field `f` of the message being built. */
static size_t put_sub(uint8_t *b, size_t o, uint32_t f, const uint8_t *inner, size_t n)
{ return put_bytes(b, o, f, inner, n); }

/* SessionInfo { f2 = handle, f4 = StreamingProtocol{f1,f3,f4,f5} } */
static size_t make_session(uint8_t *b, uint64_t handle, int proto, int tcp, uint32_t port)
{
    uint8_t sp[64]; size_t sn = 0;
    if (proto) sn = put_uint(sp, sn, 1, (uint64_t)proto);
    if (tcp)   sn = put_uint(sp, sn, 3, 1);
    sn = put_uint(sp, sn, 4, 1);                 /* encrypted, 1 in 170/170 */
    sn = put_uint(sp, sn, 5, port);
    { size_t o = 0;
      o = put_uint(b, o, 2, handle);
      return put_sub(b, o, 4, sp, sn); }
}

/* body -> ChannelInfo{f<chan> = body} -> f3{f8 = ChannelInfo} -> top */
static size_t wrap_reply(uint8_t *out, int chan, const uint8_t *body, size_t bn)
{
    uint8_t ci[512]; size_t cn = put_sub(ci, 0, (uint32_t)chan, body, bn);
    uint8_t f3[600]; size_t f3n = put_sub(f3, 0, 8, ci, cn);
    size_t o = put_uint(out, 0, 1, 7);            /* f1 = seq, present in most replies */
    return put_sub(out, o, 3, f3, f3n);
}

/* ── Les reponses nominales ───────────────────────────────────────────────── */

static void the_video_reply(void)
{
    uint8_t mode[32]; size_t mn = 0;
    uint8_t body[256]; size_t bn = 0;
    uint8_t sess[96];  size_t sn;
    uint8_t rep[600];  size_t rn;
    ann_reply_t r;

    mn = put_uint(mode, mn, 1, 1280);
    mn = put_uint(mode, mn, 2, 720);
    mn = put_f32(mode, mn, 3, 120.0f);

    sn = make_session(sess, 1234567u, 0 /*SUFP*/, 0 /*UDP*/, 7010);
    bn = put_sub(body, bn, 1, sess, sn);
    bn = put_sub(body, bn, 2, mode, mn);
    bn = put_uint(body, bn, 7, 70000000u);        /* granted bitrate cap */

    rn = wrap_reply(rep, 1, body, bn);
    CHECK(ann_reply_parse(rep, rn, &r) == 1, "[video] the reply is recognised");
    CHECK(r.channel == ANN_CHAN_VIDEO, "[video] the channel comes from the FIELD NUMBER");
    CHECK(r.have_session && r.handle == 1234567u, "[video] the granted handle");
    CHECK(r.port == 7010, "[video] the announced port");
    CHECK(ann_reply_port_offset(&r) == 10, "[video] and its offset is the channel map's 10");
    CHECK(r.tcp == 0, "[video] UDP by default");
    CHECK(r.encrypted == 1, "[video] the channel is encrypted");
    CHECK(r.have_mode && r.width == 1280 && r.height == 720,
          "[video] THE GRANTED RESOLUTION - the question open since L18");
    CHECK(r.fps > 119.9f && r.fps < 120.1f, "[video] and the granted frame rate, a float32");
    CHECK(r.codec == 0, "[video] an absent codec field means H.264");
    CHECK(r.bitrate_bps == 70000000u, "[video] the granted bitrate cap");
}

static void the_audio_reply(void)
{
    uint8_t body[256]; size_t bn = 0;
    uint8_t sess[96];  size_t sn;
    uint8_t rep[600];  size_t rn;
    ann_reply_t r;

    sn = make_session(sess, 42, 0, 0, 7030);
    bn = put_sub(body, bn, 1, sess, sn);
    bn = put_uint(body, bn, 2, 48000);
    bn = put_uint(body, bn, 3, 16);
    bn = put_uint(body, bn, 4, 2);                /* 2 = FLAC */

    rn = wrap_reply(rep, 2, body, bn);
    CHECK(ann_reply_parse(rep, rn, &r) == 1, "[audio] recognised");
    CHECK(r.channel == ANN_CHAN_AUDIO, "[audio] channel 2");
    CHECK(ann_reply_port_offset(&r) == 30, "[audio] offset 30, as the channel map says");
    CHECK(r.have_audio && r.sample_rate == 48000 && r.bits == 16, "[audio] format");
    /* The whole point: the codec the server GRANTED, not the one we asked for.
     * Our client used to assume its own request had been honoured. */
    CHECK(r.audio_codec == 2, "[audio] THE GRANTED CODEC - FLAC, not what we asked");
}

static void the_transport_flag(void)
{
    uint8_t body[256]; size_t bn = 0;
    uint8_t sess[96];  size_t sn;
    uint8_t rep[600];  size_t rn;
    ann_reply_t r;

    /* The reliability profile: the same channels, granted over TCP. Only this
     * flag moves - the announced port does not. */
    sn = make_session(sess, 9, 0, 1 /*TCP*/, 7010);
    bn = put_sub(body, bn, 1, sess, sn);
    rn = wrap_reply(rep, 1, body, bn);
    CHECK(ann_reply_parse(rep, rn, &r) == 1, "[transport] recognised");
    CHECK(r.tcp == 1, "[transport] TCP is reported");
    CHECK(r.port == 7010, "[transport] and the port is unchanged by it");

    /* SFTP on file transfer, FlatBuffers on input: the protocol field. */
    sn = make_session(sess, 9, 5 /*SFTP*/, 1, 7015);
    bn = 0; bn = put_sub(body, bn, 1, sess, sn);
    rn = wrap_reply(rep, 8, body, bn);
    ann_reply_parse(rep, rn, &r);
    CHECK(r.protocol == 5 && ann_reply_port_offset(&r) == 15, "[transport] SFTP on offset 15");
}

/* ── LA FUITE ─────────────────────────────────────────────────────────────── */

static void the_key_is_never_copied(void)
{
    /* A stand-in for the real thing: the FileTransfer body's field 2 holds an
     * OpenSSH private key in cleartext on the wire. We put a marker there and
     * then search every byte of the output structure for it.
     *
     * The old code logged 160 bytes of this reply in hex AND in ASCII, so a
     * completed bootstrap wrote a live private key into halyard.log and
     * the network mirror pushed it over plain TCP to the dev machine. */
    static const char MARKER[] = "-----BEGIN OPENSSH PRIVATE KEY-----MARKER";
    uint8_t body[512]; size_t bn = 0;
    uint8_t sess[96];  size_t sn;
    uint8_t rep[900];  size_t rn;
    ann_reply_t r;
    const uint8_t *raw;
    size_t i;
    int found = 0;

    sn = make_session(sess, 77, 5, 1, 7015);
    bn = put_sub(body, bn, 1, sess, sn);
    bn = put_bytes(body, bn, 2, (const uint8_t *)MARKER, sizeof MARKER - 1);

    rn = wrap_reply(rep, 8, body, bn);
    CHECK(ann_reply_parse(rep, rn, &r) == 1, "[FUITE] the file-transfer reply still parses");
    CHECK(r.channel == ANN_CHAN_FILEXFER, "[FUITE] and is identified");
    CHECK(r.handle == 77, "[FUITE] its session handle is read, which is all we want from it");

    /* The search: every byte of the result, for the first eight of the marker. */
    raw = (const uint8_t *)&r;
    for (i = 0; i + 8 <= sizeof r; i++)
        if (memcmp(raw + i, MARKER, 8) == 0) found = 1;
    CHECK(!found, "[FUITE] NOTHING of the key field appears anywhere in the parsed result");
}

/* ── Les bornes ───────────────────────────────────────────────────────────── */

static void the_bounds(void)
{
    ann_reply_t r;
    uint8_t buf[64];
    size_t i;

    CHECK(ann_reply_parse(NULL, 10, &r) == 0, "[bornes] NULL body");
    CHECK(ann_reply_parse(buf, 0, &r) == 0, "[bornes] empty body");
    CHECK(ann_reply_parse(buf, 4, NULL) == 0, "[bornes] NULL output");

    /* A reply of another kind is NOT an error - it is simply not ours. */
    { size_t n = put_uint(buf, 0, 1, 3);
      n = put_uint(buf, n, 5, 1);
      CHECK(ann_reply_parse(buf, n, &r) == 0, "[bornes] another reply type returns 0");
      CHECK(r.channel == ANN_CHAN_NONE, "[bornes] and leaves the result blank"); }

    /* ...but it still NAMES ITSELF. The server interleaves stats and config
     * replies with the announcements, and a caller that can only say
     * "unrecognised, 36 bytes" leaves the reader guessing at exactly the moment
     * a fact was available. The field number is a number, never content. */
    { uint8_t inner[16]; size_t in = put_uint(inner, 0, 2, 1);   /* f3.3 = stats */
      uint8_t body[64];  size_t bn = put_sub(body, 0, 3, inner, in);
      uint8_t rep[128];  size_t rn = put_sub(rep, 0, 3, body, bn);
      CHECK(ann_reply_parse(rep, rn, &r) == 0, "[type] a stats reply is not an announcement");
      CHECK(r.response_field == 3, "[type] and it says so: response f3.3");
      CHECK(r.channel == ANN_CHAN_NONE, "[type] with no channel invented"); }
    { uint8_t inner[16]; size_t in = put_uint(inner, 0, 1, 1);   /* f3.12 = encryption */
      uint8_t body[64];  size_t bn = put_sub(body, 0, 12, inner, in);
      uint8_t rep[128];  size_t rn = put_sub(rep, 0, 3, body, bn);
      ann_reply_parse(rep, rn, &r);
      CHECK(r.response_field == 12, "[type] an encryption reply names itself too"); }

    /* Truncation at every length: the parser must never read past the end and
     * must never hang. `pb_skip_field` returning a non-increasing offset is how
     * a malformed message became an infinite loop once (KB §9, 2026-08-25). */
    {
        uint8_t body[128]; size_t bn = 0;
        uint8_t sess[96];  size_t sn = make_session(sess, 5, 0, 0, 7013);
        uint8_t rep[400];  size_t rn;
        bn = put_sub(body, bn, 1, sess, sn);
        rn = wrap_reply(rep, 6, body, bn);
        CHECK(ann_reply_parse(rep, rn, &r) == 1 && r.channel == ANN_CHAN_GAMEPAD,
              "[bornes] the full gamepad reply parses");
        for (i = 0; i < rn; i++) {
            ann_reply_t t;
            (void)ann_reply_parse(rep, i, &t);   /* must simply return */
        }
        checks++;   /* reaching here IS the check: no hang, no crash */
    }

    /* Every single-byte corruption: still no hang and no crash. */
    {
        uint8_t body[128]; size_t bn = 0;
        uint8_t sess[96];  size_t sn = make_session(sess, 5, 0, 0, 7020);
        uint8_t rep[400];  size_t rn;
        bn = put_sub(body, bn, 1, sess, sn);
        rn = wrap_reply(rep, 4, body, bn);
        for (i = 0; i < rn; i++) {
            ann_reply_t t;
            const uint8_t save = rep[i];
            rep[i] = (uint8_t)(save ^ 0xFF);
            (void)ann_reply_parse(rep, rn, &t);
            rep[i] = save;
        }
        checks++;
    }

    CHECK(ann_reply_port_offset(NULL) == -1, "[bornes] no record, no offset");
    memset(&r, 0, sizeof r);
    CHECK(ann_reply_port_offset(&r) == -1, "[bornes] no announced port, no offset");
    CHECK(strcmp(ann_reply_channel_name(ANN_CHAN_NONE), "?") == 0, "[bornes] unknown channel name");
    CHECK(strcmp(ann_reply_channel_name(ANN_CHAN_VIDEO), "video") == 0, "[bornes] a known one");
}

int main(void)
{
    printf("test_ann_reply - the server's announcement agreements\n");
    the_video_reply();
    the_audio_reply();
    the_transport_flag();
    the_key_is_never_copied();
    the_bounds();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
