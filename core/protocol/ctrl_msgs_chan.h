/* ctrl_msgs_chan - the two channel orders, and the map between them.
 *
 * Header-only and PURE, so the map is covered by tests/test_vid_uplink.c. It
 * lives apart from ctrl_msgs.h because that header's companion .c needs wolfSSL
 * and cannot be linked into an offline test - and this map is exactly the kind
 * of thing that must not go unchecked.
 *
 * === SRV5 2026-10-02 — TWO ORDERS, NEVER TO BE CONFUSED ======================
 *
 * `SHADOW_CHAN_IDX_*` indexes the eight announcement BODIES, in the order the
 * official client sends them (ctrl_msgs.c, CI_BODY_5..CI_BODY_12).
 * `ann_channel_t` (ann_reply.h) is the server's own channel NUMBER, which it
 * puts in its announcement replies and in its "Channel down" notifications.
 * They are different permutations. A single confusion between them
 * re-announces the WRONG channel - and on a channel that is working, that hands
 * the server a duplicate stream registration.
 *
 * The body order was derived from the first byte of each captured body (the
 * ChannelInfo oneof tag) read against the channel-name order the official
 * binary's `ctrlchanv2: Channel down` table gives (KB §3.38):
 *
 *   idx 0  CI_BODY_5   tag 0x0a = f1  VIDEO
 *   idx 1  CI_BODY_6   tag 0x22 = f4  CURSOR
 *   idx 2  CI_BODY_7   tag 0x1a = f3  INPUT
 *   idx 3  CI_BODY_8   tag 0x12 = f2  AUDIO
 *   idx 4  CI_BODY_9   tag 0x32 = f6  CONTROLLER
 *   idx 5  CI_BODY_10  tag 0x3a = f7  CLIPBOARD
 *   idx 6  CI_BODY_11  tag 0x2a = f5  MIC
 *   idx 7  CI_BODY_12  tag 0x42 = f8  FILETRANSFER
 *
 * Cross-checked on the CONTENTS, not just the tags: CI_BODY_8 (audio) carries
 * `10 80 f7 02` = 48000 and `18 10` = 16, i.e. 48 kHz / 16-bit, and CI_BODY_11
 * (mic) carries the same pair - which is also what the server's microphone
 * decoder demands (`Micro::CodecSession::Opus::Decoder::SpecificCodecInit`
 * refuses anything that is not signed 16-bit). An index whose body did not
 * match its name would fail that check.
 */
#pragma once

#include "ann_reply.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHADOW_CHAN_IDX_VIDEO         0
#define SHADOW_CHAN_IDX_CURSOR        1
#define SHADOW_CHAN_IDX_INPUT         2
#define SHADOW_CHAN_IDX_AUDIO         3
#define SHADOW_CHAN_IDX_CONTROLLER    4
#define SHADOW_CHAN_IDX_CLIPBOARD     5
#define SHADOW_CHAN_IDX_MIC           6
#define SHADOW_CHAN_IDX_FILETRANSFER  7

/* The body index for a server channel number, or -1 if unknown.
 * -1 and not 0: a default of 0 would silently re-announce the VIDEO channel. */
static inline int shadow_chan_idx_from_ann(int ann_channel)
{
    switch (ann_channel) {
        case ANN_CHAN_VIDEO:     return SHADOW_CHAN_IDX_VIDEO;        /* 1 -> 0 */
        case ANN_CHAN_AUDIO:     return SHADOW_CHAN_IDX_AUDIO;        /* 2 -> 3 */
        case ANN_CHAN_INPUT:     return SHADOW_CHAN_IDX_INPUT;        /* 3 -> 2 */
        case ANN_CHAN_CURSOR:    return SHADOW_CHAN_IDX_CURSOR;       /* 4 -> 1 */
        case ANN_CHAN_MICRO:     return SHADOW_CHAN_IDX_MIC;          /* 5 -> 6 */
        case ANN_CHAN_GAMEPAD:   return SHADOW_CHAN_IDX_CONTROLLER;   /* 6 -> 4 */
        case ANN_CHAN_CLIPBOARD: return SHADOW_CHAN_IDX_CLIPBOARD;    /* 7 -> 5 */
        case ANN_CHAN_FILEXFER:  return SHADOW_CHAN_IDX_FILETRANSFER; /* 8 -> 7 */
        default:                 return -1;
    }
}

#ifdef __cplusplus
}
#endif
