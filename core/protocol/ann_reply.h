/* ann_reply - the server's answer to a channel announcement, decoded.
 *
 * === WHAT WE WERE THROWING AWAY ===
 *
 * We send eight channel announcements and the server answers every one of them.
 * Each answer states, for that channel, the GRANTED transport, protocol, port
 * offset and session handle - and for video the resolution, frame rate, codec
 * and bitrate cap, for audio the codec actually granted.
 *
 * All of it was dumped to the log as hexadecimal and thrown away. So the client
 * derived the channel map from a REST port and guessed the audio codec from
 * what it had asked for, while the server had already said both, in every
 * session, in a message we were reading and discarding.
 *
 * === AND WHY THE DUMP HAD TO GO ===
 *
 * The eighth reply - FileTransfer - carries an OpenSSH ed25519 PRIVATE KEY in
 * cleartext, the session material for the :base+15 SFTP channel. It begins at
 * offset 33 of the reply body. The old code logged the first 160 bytes of every
 * reply in hex AND in ASCII, so a completed bootstrap wrote a live private key
 * into `halyard.log` - which `tools/switch-logsink.sh` then mirrored over
 * plain TCP to the development machine.
 *
 * That is why this module exists rather than a wider dump: it reads the fields
 * we want, NEVER copies the key field, and lets the caller log FACTS instead of
 * bytes. Nothing here returns, stores or formats field 2 of the FileTransfer
 * body, and nothing should be added that does.
 *
 * Wire, established from 170 replies across 30 capture sets and cross-checked
 * against 49 lines of the official client's own telemetry:
 *
 *   reply body = protobuf
 *     f3 = response payload
 *       f8 = ChannelInfo, in which EXACTLY ONE field is set and ITS NUMBER IS
 *            THE CHANNEL: 1 Video, 2 Audio, 3 Input, 4 Cursor, 5 Micro,
 *            6 Gamepad, 7 Clipboard, 8 FileTransfer
 *         f1 = SessionInfo
 *           f2 = granted session handle (a server millisecond clock)
 *           f4 = StreamingProtocol
 *             f1 = protocol   absent/0 SUFP, 4 FlatBuffers (input), 5 SFTP
 *             f3 = network    absent/0 UDP, 1 TCP
 *             f4 = encrypted  1 in 170/170
 *             f5 = port, ALWAYS 7000 + offset - see `port_offset` below
 *         f2.. = per-channel extras (video mode, audio codec, ...)
 */
#ifndef SHADOW_ANN_REPLY_H
#define SHADOW_ANN_REPLY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channel numbers are the field numbers inside f3.8 - the same mapping the
 * requests use, which is why a reply identifies its channel by itself and does
 * not depend on the order replies arrive in. */
typedef enum {
    ANN_CHAN_NONE     = 0,
    ANN_CHAN_VIDEO    = 1,
    ANN_CHAN_AUDIO    = 2,
    ANN_CHAN_INPUT    = 3,
    ANN_CHAN_CURSOR   = 4,
    ANN_CHAN_MICRO    = 5,
    ANN_CHAN_GAMEPAD  = 6,
    ANN_CHAN_CLIPBOARD = 7,
    ANN_CHAN_FILEXFER = 8,
} ann_channel_t;

typedef struct {
    /* The sequence number the server SENDS BACK (field 1 of the message).
     *
     * It echoes our request's, which makes the control channel matchable - and
     * therefore measurable. It is the client's only source of a real round
     * trip: of the twelve stages in the latency report, one contains network
     * and it measures only jitter. See streaming/rtt.h. 0 = absent, which is
     * the case for unsolicited messages. */
    uint32_t seq;

    /* The response-type field number seen inside f3, whatever it was: 8 is a
     * channel announcement, 3 capabilities or periodic stats, 4 authentication,
     * 10 display config, 12 encryption, 13 set-bitrate...
     *
     * Reported even when the reply is NOT an announcement, so the caller can
     * say WHICH message it declined to decode instead of "unrecognised". A
     * length alone leaves the reader guessing; a field number is a fact, and it
     * costs nothing to carry - it is a number, never content. 0 = none seen. */
    uint32_t response_field;

    ann_channel_t channel;
    int      have_session;      /* a SessionInfo was present */
    uint64_t handle;            /* granted session handle */

    int      protocol;          /* 0 SUFP, 4 FlatBuffers, 5 SFTP */
    int      tcp;               /* 1 = TCP, 0 = UDP */
    int      encrypted;
    uint32_t port;              /* as announced: 7000 + offset, VM-internal */

    /* Video (channel 1). `have_mode` says whether the server stated one. */
    int      have_mode;
    uint32_t width, height;
    float    fps;
    uint32_t codec;             /* absent = 0 = H.264, 1 = H.265 */
    int      color444;
    uint32_t bitrate_bps;

    /* Audio out (2) and micro (5). */
    int      have_audio;
    uint32_t sample_rate, bits;
    uint32_t audio_codec;       /* 1 = Opus, 2 = FLAC */
} ann_reply_t;

/* Parses one reply BODY (what `ctrl_tcp_recv_cleartext` returns, i.e. without
 * the 4-byte [type][len] header). Returns 1 when a channel announcement reply
 * was recognised, 0 otherwise - a reply of another kind is not an error, it is
 * simply not ours.
 *
 * Never fails destructively: an unknown or malformed field is skipped, and the
 * fields that did parse are kept. A truncated reply gives what it contained. */
int ann_reply_parse(const uint8_t *body, size_t len, ann_reply_t *out);

/* The channel's OFFSET from the port base, derived from the announced port.
 *
 * === THE TRAP THIS FUNCTION EXISTS TO NAME ===
 *
 * The announced port was 7010/7012/7013/7014/7015/7020/7030/7032 in 170 replies
 * out of 170, across SIX different port bases (9000, 10000, 11000, 13000, 14000,
 * 15000). It is the port INSIDE the VM and it never varies. Dialling it would
 * reach nothing.
 *
 * What it does give - from the server, every session - is `port % 1000`, the
 * channel offset. That is the channel map (10, 12, 13, 14, 15, 20, 30, 32) said
 * by the machine that owns it, instead of a table we maintain by hand.
 *
 * Returns -1 when no port was announced. */
int ann_reply_port_offset(const ann_reply_t *r);

/* A short, printable name. Never NULL. */
const char *ann_reply_channel_name(ann_channel_t c);

#ifdef __cplusplus
}
#endif
#endif
