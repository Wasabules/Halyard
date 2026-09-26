/* VideoSslTcpChannel - port_base+20 TCP+TLS video correction channel.
 *
 * Found by RE on 2026-05-15 (tools/ida/out/VIDEO_SSL_TCP_CHANNEL_RE.md):
 *   - plain TLS (wolfSSL, no SNI, no ALPN, no certificate validation)
 *   - after the handshake: a single 5 B connect_msg "00 00 00 00 00" is sent
 *   - the server then pushes VideoTcpFrames on its own:
 *       [u8 type/version][u8 flag][4B ignored][u32 LE seq_id][NAL bytes...]
 *     type byte0 in [0x10..0x2F] = video frame
 *     type byte0 in [0x30..0x3F] = PING (latency keepalive)
 *
 * === 2026-08-27 - THIS CHANNEL CARRIES THE CURSOR, NOT VIDEO ===
 *
 * The module name and everything above it are MISLEADING; they are kept so the
 * history stays readable. `:base+20` is the CURSOR channel (KB §3.37). Dumping
 * the whole channel in the clear - from the very capture that had founded the
 * opposite conclusion - yields 1467 32x32 BGRA pointer images, 30 "cursor
 * hidden" frames, and ZERO byte of video.
 *
 * "Payload = raw H.264 NAL bytestream" is therefore FALSE: it is a BGRA image.
 * Do NOT build the "phase 2" described above - it would push cursor bitmaps
 * into the decoder.
 *
 * The STFP format itself is real: it is the framing shared by ALL of this
 * protocol's TCP channels. Seeing it tells you nothing about the medium being
 * carried - the CHANNEL identifies that, not the type byte.
 *
 * `VideoSslTcpChannel` does exist in the official binary, but it is only
 * instantiated in the `reliability` streaming profile, which has never been
 * captured. The day it is, TCP video will arrive on the VIDEO channel's port
 * (base+10), not here: all three constructor variants read the same port.
 *
 * This module stays useful for what it really DOES: open the cursor channel
 * over TLS and frame it by the announced length.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctrl_video_tcp_s ctrl_video_tcp_t;

/* === K16b 2026-08-29 - THE CHANNEL'S ROLE, NOT THE FRAME'S CLASS ===
 *
 * This module reads the STFP framing, which is SHARED by several channels. It
 * used to route frames by their CLASS (`0x12` -> cursor, everything else ->
 * video), and that was the same mistake KB §3.37 corrected on the port map: the
 * class does not say what a frame carries, the CHANNEL does.
 *
 * `0x12` means "complete frame / first frame". On `:base+20` that is a pointer
 * sprite; on `:base+10` it is THE KEYFRAME - SPS, PPS and IDR, once per
 * session, 74 ms after the registration. Routed by class, it went to
 * `cursor_cb`, which is NULL on the video instance: the one frame that could
 * start the decoder was destroyed in silence, after bumping the
 * received-frames counter.
 *
 * Hence the exact picture K15r showed - normal bitrate, `decoded` tracking
 * `video`, `sps=0 pps=0 idr_t=0`, black screen, zero errors - and three
 * keyframe requests sent for nothing: the server had already answered.
 *
 * So the role is now EXPLICIT at open time. Deriving it from the port offset
 * would work today and fall back into the same trap tomorrow. */
typedef enum {
    CTRL_VST_ROLE_CURSOR = 0,   /* :base+20 — sprites de pointeur */
    CTRL_VST_ROLE_VIDEO   = 1,   /* :base+10 — flux H.264 en profil reliability */
} ctrl_video_tcp_role_t;

/* Callback fired once per fully-parsed VideoTcpFrame.
 *   nal_bytes  : pointer into the internal buffer (valid for the call only)
 *   len        : payload bytes (= total - 10 header)
 *   frame_id   : u32 LE from header bytes 6..9
 *   flag       : u8 from header byte 1 (= subtype / frame counter)
 *   is_keyframe: frame class 1 (`0x1_`) = complete frame. K16b: this used to
 *                be bit 0 of `flag`, i.e. the PARITY of the frame counter.
 */
typedef void (*on_video_tcp_frame_cb)(void *udata,
                                       const uint8_t *nal_bytes, size_t len,
                                       uint32_t frame_id, uint8_t flag,
                                       int is_keyframe);

/* === S54 2026-08-26 - THIS IS THE CURSOR CHANNEL ===
 *
 * The module is called "video_tcp" because we took `:base+20` for a
 * `VideoSslTcpChannel` for a long time. The official client's telemetry NAMES
 * its sockets after their port: this one is called `Cursor` (KB §3.37, verified
 * on two different port bases). Its frames carry the IMAGE of the remote
 * pointer.
 *
 * The file name is left alone - renaming it would churn a huge diff for
 * nothing. What changes is where the frames go: those of type
 * `CURSOR_WIRE_TYPE_IMAGE` now reach this callback, instead of being pushed
 * into the H.264 decoder, which rejected them every session ("VST frame
 * missing Annex-B start code").
 *
 * `payload` points PAST the 10-byte frame header; that is what
 * `cursor_wire_parse_image()` expects. */
typedef void (*on_cursor_frame_cb)(void *udata,
                                    const uint8_t *charge, size_t len,
                                    uint8_t type, uint32_t id);

/* Optional: without this callback, cursor frames are counted and dropped (the
 * pre-S54 behaviour, minus the detour through the video decoder). */
void ctrl_video_tcp_set_cursor_cb(ctrl_video_tcp_t *c, on_cursor_frame_cb cb);

/* Opens the TLS connection to vm_host:base_port+20 and sends connect_msg.
 * Starts a receiver thread that calls `cb` for every parsed frame.
 * Returns 0 on success, -1 on error (e.g. server reject / TLS failure). */
/* Opens with explicit auth_hash (= 20 bytes from the Auth reply on :base+11) +
 * cipher key (= 32 bytes from the Encryption reply) + bearer_jwt (= main_jwt
 * from /proximus-credentials). The JWT is the variant 7 body (= the ~100 byte
 * connect_msg the server expects). */
int  ctrl_video_tcp_open (ctrl_video_tcp_t **out,
                          const char *vm_host, uint16_t base_port,
                          const uint8_t auth_hash[20],
                          const char *bearer_jwt,
                          const char *streaming_token,
                          on_video_tcp_frame_cb cb, void *udata);

/* === K15h 2026-08-29 - THE SAME CLIENT, ON ANOTHER PORT ===
 *
 * `ctrl_video_tcp_open` hardcodes `base_port + 20`, because the cursor was the
 * only STFP channel we read. The first capture in the `reliability` profile
 * shows that VIDEO uses the same framing on `base_port + 10` (K15d), and that
 * the channel is wrapped in TLS just like the cursor one (K15e).
 *
 * This variant therefore takes the offset as a parameter.
 * `ctrl_video_tcp_open` becomes a call to it with 20, identically - no
 * existing caller changes behaviour. */
int  ctrl_video_tcp_open_port(ctrl_video_tcp_t **out,
                              const char *vm_host, uint16_t base_port,
                              uint16_t port_offset,
                              ctrl_video_tcp_role_t role,
                              const uint8_t auth_hash[20],
                              const char *bearer_jwt,
                              const char *streaming_token,
                              on_video_tcp_frame_cb cb, void *udata);

/* Closes the connection and joins the thread. Safe to call multiple times. */
void ctrl_video_tcp_close(ctrl_video_tcp_t *c);

/* Stats accessor (= count of frames received, pings, parse errors). */
typedef struct {
    uint32_t frames_received;
    uint32_t pings_received;
    uint32_t parse_errors;
    uint32_t bytes_received;
    /* K16b - this used to count `flag & 0x01`, that is the PARITY of the frame
     * counter: half the frames were declared keyframes. The real marker is the
     * class, `(byte 0) >> 4 == 1`. Same family as V12, where five displayed
     * counters were never written - a counter that lies is worse than a
     * missing one, because you lean on it. */
    uint32_t idr_frames;
} ctrl_video_tcp_stats_t;

void ctrl_video_tcp_get_stats(const ctrl_video_tcp_t *c, ctrl_video_tcp_stats_t *out);

#ifdef __cplusplus
}
#endif

/* R1 - picture refresh request (one byte `0x64`) on :base+20.
 * The official client emits it when the window is resized: 92 occurrences
 * during resizes, 0 outside them. It does NOT renegotiate the resolution (no
 * kNotifyResolution, no kDisplayConfig) - it rescales locally and simply asks
 * for fresh pictures. SHADOW_VIDEO_REFRESH=0 disables it. */
int ctrl_video_tcp_request_refresh(ctrl_video_tcp_t *c);

/* KEYFRAME request on an STFP channel. K16c: the message is the SAME one as on
 * UDP, `69 50 00 02 [counter u16 LE]`, written raw - this protocol's upstream
 * direction is not framed. K15q had guessed a bare 10-byte STFP header with
 * class 0 instead, read off what the official client writes; the shape was
 * right but the SOCKET was wrong (it sits on `:base+14`, the clipboard).
 * Arms a flag; the receive loop does the writing. See ctrl_video_tcp.c. */
int ctrl_video_tcp_request_keyframe(ctrl_video_tcp_t *c);
