// CtrlChanV2 protobuf message builders/parsers, Shadow-specific.
//
// Schema reconstructed by RE (see recon/ENCRYPTION_AND_FRAMING.md section 3):
//
//   message Message {
//       oneof body { Request request = 1; Reply reply = 2; }
//   }
//   message Request {
//       oneof what {
//           Authentication      authentication      = 1;
//           RegisterSession     register_session    = 2;
//           Encryption          encryption          = 3;
//           UnregisterSession   unregister_session  = 4;
//           DisplayConfig       display_config      = 5;
//           State               state               = 6;
//           Notify              notify              = 7;
//           Update              update              = 8;
//           Ping                ping                = 9;
//           ...
//       }
//   }
//   message Request_Authentication {
//       bool   reverse_auto_register = 1;
//       uint32 permissions           = 2;     // bitmap (Video=2, AudioIn=4, AudioOut=8, Cursor=0x10, Input=0x20, Gamepad=0x40, Clipboard=0x80, FileTransfer=0x100)
//       string streamingtoken         = 3;
//       string client_id              = 4;
//       string sessionUniqueId        = 5;
//       string connectionUniqueId     = 6;
//   }

#pragma once

#include "hid_lock.h"   /* HID1: hid_locks */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHADOW_PERM_VIDEO         0x002
#define SHADOW_PERM_AUDIO_IN      0x004
#define SHADOW_PERM_AUDIO_OUT     0x008
#define SHADOW_PERM_CURSOR        0x010
#define SHADOW_PERM_INPUT         0x020
#define SHADOW_PERM_GAMEPAD       0x040
#define SHADOW_PERM_CLIPBOARD     0x080
#define SHADOW_PERM_FILE_TRANSFER 0x100
/* S4 2026-08-21: the desktop client sends permissions = 0x1fe, i.e. all eight
 * bits. We were omitting two of them (AUDIO_IN 0x004, FILE_TRANSFER 0x100),
 * which gave 250 instead of 510. Source: a diff of our own dumped
 * Authentication (SHADOW_DUMP_AUTH) against the LD_PRELOAD capture of the
 * official client. */
#define SHADOW_PERM_ALL           0x1fe

/* Capabilities request - the FIRST message sent on SslCtrlChanV2, BEFORE
 * Authentication. Reverse-engineered with the LD_PRELOAD hook on 2026-05-06
 * (see memory project_sslctrlchanv2_wire_format_FOUND).
 *
 * Structure captured from the official desktop app:
 *   field 2 (sub) {
 *       field 3 (sub) {
 *           field 1 (string) = ""   // empty
 *       }
 *   }
 *   field 4 (sub) {
 *       field 1 (varint) = 2                      // version=2
 *       field 2 (string) = client_version         // e.g. "12.3.3"
 *       field 3 (string) = full_user_agent        // e.g. "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3"
 *       field 5 (sub) {
 *           field 1 (varint) = 1
 *           field 3 (string) = "OCapture"
 *       }
 *   }
 *
 * We reproduce that exact skeleton so the server accepts us - only the UA and
 * client_version are parameters.
 */
int ctrl_build_capabilities(uint8_t *out, size_t out_cap,
                              const char *client_version,
                              const char *user_agent);

/* Request_Authentication wrapped in a ControlProto::Message envelope
 * (= Request.authentication). The caller supplies the destination buffer.
 * Returns the number of bytes written, or -1 on error (buffer too small).
 */
int ctrl_build_authentication(uint8_t *out, size_t out_cap,
                                bool reverse_auto_register,
                                uint32_t permissions,
                                const char *streamingtoken,
                                const char *client_id,
                                const char *session_unique_id,
                                const char *connection_unique_id);

/* RegisterSession - announces the display config + resolution.
 *
 * === SRV-DOC 2026-10-02 - THIS IS THE SERVER'S DisplayConfig =================
 * Request field 6 of the envelope below is field 10 of the Request, and
 * `DispatchControlMessage_` @0x140b2ca30 routes field 10 to
 * `ProcessDisplayConfigRequestMessage_` - which is why its body carries an
 * EDID, a resolution and a scale. Two consequences for any future caller:
 *   - it validates (`Invalid resolution : w=%u, h=%u`, and it overrides a
 *     non-zero position for the primary display to (0,0));
 *   - it is the ONE request subject to `VerifyDisplayConfigCooldown_`
 *     @0x140b3b2d0, which does not reject a second one inside the cooldown but
 *     SLEEPS for the remainder - blocking the control reader for that long.
 * So a resolution change must be rate-limited on our side. Sent once per
 * session today, which is why this has never bitten.
 *
 * Wire format byte-exact against the desktop app (LD_PRELOAD capture 2026-05-08):
 *   Message {
 *       field 1 = 3                               <- version
 *       field 2 (sub) = Request {
 *           field 10 (sub) = RegisterSession {
 *               field 2 (sub) = display_info {
 *                   field 1 (bytes 128) = raw EDID
 *               }
 *               field 3 (sub) = resolution {
 *                   field 1 (varint) = width  (e.g. 1920)
 *                   field 2 (varint) = height (e.g. 1080)
 *                   field 3 (fixed32) = refresh_hz_int? (seen 0x43100726 - plainly not a float)
 *               }
 *               field 4 (sub) = scale {
 *                   field 3 (fixed32 float) = 1.0 (= 0x3f800000)
 *               }
 *           }
 *       }
 *       field 4 (sub) = caps meta
 *       field 5 (sub) = OCapture
 *   }
 */
int ctrl_build_register_session(uint8_t *out, size_t out_cap,
                                  uint32_t width, uint32_t height);

/* SRV5 2026-10-02 - the eight announcement body indices and the map from the
 * server's channel numbers. In their own pure header so the map is tested
 * (tests/test_vid_uplink.c): confusing the two orders re-announces the wrong
 * channel. */
#include "ctrl_msgs_chan.h"

/* Channel announcement message (8 of them, f1=5..12).
 * Byte-exact wire against the desktop app: Message{f1=seq, f2=Request{f8=ChannelInfo}, f4=caps, f5=OCapture}.
 * `seq` must run 5..12 in sequence after RegisterSession (which was f1=3, then msg f1=4=ack).
 * `chan_idx` = index 0..7 into the internal SHADOW_CHANNEL_INFO_BODIES table.
 */
int ctrl_build_channel_announcement(uint8_t *out, size_t out_cap,
                                      int seq, int chan_idx);

/* Q1 2026-05-18 - override the video encoding parameters.
 * CI_BODY_5 (= the video channel, chan_idx=0) carries the max_bitrate + fps +
 * resolution we announce to the server. It is hardcoded to 1920x1080 @ 143.85fps
 * @ 20Mbps. This struct overrides those without touching the 7 other channels.
 *
 * To use it: pass a ctrl_video_params_t to ctrl_build_channel_announcement_ex().
 * NULL -> uses the hardcoded CI_BODY_5, as before. */
typedef struct {
    uint32_t width;          /* default 1920 */
    uint32_t height;         /* default 1080 */
    float    fps;            /* default 143.85 (= matches the desktop's hardcoded value) */
    uint32_t max_bitrate_bps;/* default 20000000 (= 20 Mbps, the desktop's hardcoded value) */
    /* === K13 2026-08-27 - THIS FIELD IS NOT THE CODEC ===
     * Working through captures/captures_settings_20260821_180409: between a
     * "Set codec H265" request and a "Set codec H264" request from the official
     * client, this submessage (`0a 06 22 04 10 02 20 01`) is STRICTLY
     * IDENTICAL - and it stays identical across every bitrate, resolution and
     * chroma change of the same session. Writing here therefore has NO effect
     * on the codec, and the values "1=H.265, 3=AV1" were guesses from campaign
     * B1, never confirmed.
     * The real selector is `codec_wire` below. This field is kept at its
     * captured value (2) because we do not know what it means: changing it
     * would depart from byte-exact without knowing why. */
    uint32_t codec;          /* garder 2 : valeur capturee, sens inconnu */
    /* === THE REAL CODEC - field 3 at the video level (tag 0x18) ===
     * WIRE enumeration (distinct from the client's internal enum):
     *   0 = H.264  -> field ABSENT (proto3 default, never emitted)
     *   1 = H.265  -> `18 01`
     *   2 = AV1    -> `18 02` (by elimination: the descriptor pool holds only
     *                 "AV1H264H265", sorted alphabetically)
     * Timestamped proof: "Set codec H265 requested" at t=367.6 s, then an
     * announcement carrying `18 01` at t=368.4; "Set codec H264 requested" at
     * t=398.4, then an announcement WITHOUT the field at t=398.5. The same
     * client's telemetry agrees.
     * CAREFUL - two enums coexist: the INTERNAL one (H264=2) feeds the
     * telemetry and the `VideoFrame` header of the UDP wire (`byte0 =
     * version<<4 | codec` = 0x02). An H.265 stream should therefore carry
     * `byte0 = 0x03`: a falsifiable prediction, never checked. */
    uint32_t codec_wire;      /* 0 = H.264 (defaut), 1 = H.265, 2 = AV1 */
    uint32_t profile;        /* default 1 (= speed/low-latency). 2 = reliability/quality? */
    /* RE3 2026-05-18 - 4 missing bools identified by Agent B RE (C75).
     * Hypothesis: adding these bools to CI_BODY_5 unlocks server features.
     * Likely field#: 8=cursor_merged, 9=high_color_fidelity, 10=hdr_enabled, 11=vr_enabled.
     * Default = false (= current behaviour). The impact is still to be A/B tested. */
    bool     cursor_merged;
    bool     high_color_fidelity;
    bool     hdr_enabled;
    bool     vr_enabled;
    /* RE8 2026-05-18 - 5 bool fields identified by decompiling sub_10E60A0 (C95 wire).
     * Wire tags: 4=0x20, 5=0x28, 6=0x30, 8=0x40, 10=0x50.
     * field 10 = multi-NAL candidate #1 (C70 - outlier at offset +52, isolated from the +44-47 block).
     * field 8 = multi-NAL candidate #2 (= late addition mid-struct).
     * Default false = byte-exact V14. A/B test field 10 first. */
    bool     re8_f4;   /* SHADOW_REG_F4 - possibly encode_low_latency */
    bool     re8_f5;   /* SHADOW_REG_F5 - possibly ref_frame_reorder */
    bool     re8_f6;   /* SHADOW_REG_F6 - possibly intra_refresh */
    bool     re8_f8;   /* SHADOW_REG_F8 — candidat multi-NAL #2 */
    bool     re8_f10;  /* SHADOW_REG_F10 — candidat multi-NAL #1 (best guess) */
} ctrl_video_params_t;

/* Variant of ctrl_build_channel_announcement with an override for chan_idx=0 (video).
 * chan_idx != 0 OR vparams == NULL -> behaves exactly like the original.
 * chan_idx == 0 AND vparams != NULL -> builds a dynamic CI_BODY_5 from the
 * custom values (width/height/fps/max_bitrate/codec/profile). */
int ctrl_build_channel_announcement_ex(uint8_t *out, size_t out_cap,
                                         int seq, int chan_idx,
                                         const ctrl_video_params_t *vparams);

/* Supported AEAD algorithms (see the SupportedAlgorithm enum in the binary) */
#define SHADOW_ALG_NONE              0
#define SHADOW_ALG_CHACHA20_POLY1305 1
/* Note: 2 and 4 are reserved for AES-GCM, but the binary does not implement them */

/* Request_Encryption - sent by the client to announce the algorithms it
 * supports. The server answers with Reply_Encryption, which carries the 32 B key.
 *
 * From the client we typically send:
 *   { supported_algorithms: [0, 2, 4, 1] }   (= mimicking the APK)
 * Field 1 = supported_algorithms (repeated SupportedAlgorithm, packed varints)
 */
/* K11: `out_client_key` (32 B, may be NULL) receives the key generated for the
 * CLIENT -> SERVER direction. Shadow encrypts each direction with its own key:
 * this one encrypts what we send, the one in the reply decrypts what the server
 * sends. See KB §3.23. */
int ctrl_build_encryption_request(uint8_t *out, size_t out_cap,
                                    const uint32_t *algos, size_t n_algos,
                                    uint8_t out_client_key[32]);

/* Reply_Encryption parser - extracts the 32 B key + nonce_extra from the reply. */
typedef struct {
    uint32_t chosen_algorithm;
    uint8_t  key[32];
    bool     has_key;
    uint8_t  nonce_extra[32];     /* variable size, capped at 32 here */
    size_t   nonce_extra_len;
    uint64_t nonce_counter;
    bool     has_tls_capability;
} shadow_encryption_reply;

bool ctrl_parse_encryption_reply(const uint8_t *buf, size_t len,
                                   shadow_encryption_reply *out);

/* Authentication reply parser - extracts the server-side 20 B hash.
 * Byte-exact wire format (LD_PRELOAD capture 2026-05-08):
 *   Message {
 *       field 1 = 1
 *       field 3 (sub) = Reply {
 *           field 4 (sub) = AuthBody {
 *               field 2 (bytes 20) = SHA-1 hash, server identifier   <- THE KEY ONE
 *               ...
 *           }
 *       }
 *   }
 * That 20 B hash is our server-side client identifier, required by the UDP
 * register sent to the :13010/:13012/:13013 streaming ports. */
typedef struct {
    bool    has_hash;
    uint8_t hash[20];
} shadow_auth_reply;

/* SRV8 2026-10-02 - the ShadowStreamer version, out of the Capabilities reply.
 * The server's handler for that request answers with exactly three integers,
 * which on our VM are 6, 3, 1. Returns false and leaves the outputs at 0 when
 * the reply does not have that shape - "not found", never a guessed version.
 * See the comment on the implementation. */
bool ctrl_parse_capabilities_reply(const uint8_t *buf, size_t len,
                                   unsigned *out_major, unsigned *out_minor,
                                   unsigned *out_patch);

bool ctrl_parse_authentication_reply_v2(const uint8_t *buf, size_t len,
                                          shadow_auth_reply *out);

/* Backward-compat - calls v2 and returns true if a hash was extracted. */
bool ctrl_parse_authentication_reply(const uint8_t *buf, size_t len,
                                       bool *out_success);

/* Build the UDP register packet - 25 B for video :13010 (and likewise for the
 * other ports).
 *   Format: `0x41 0x01 0x00 0x14 0x00 [hash 20B]`
 * out_buf must hold 25 bytes. Returns 25 on success. */
/* Chooses the codec of the OUTGOING audio channel, as its wire value: 1 = Opus
 * (default), 2 = FLAC (the official client's "High fidelity"). Must be set
 * BEFORE bootstrap - the codec is negotiated when the session opens.
 * Does not touch the microphone, which stays on Opus in the official client. */
void ctrl_msgs_set_audio_codec(uint32_t codec_wire);

/* B4 - video transport requested in the channel announcement (field f3):
 * 0 = UDP, 1 = TCP. Set by ctrl_session once per session, so the request and
 * the receive side can never disagree. */
void ctrl_msgs_set_video_tcp(int on);

int ctrl_build_udp_register(uint8_t *out_buf, size_t out_cap,
                              const uint8_t hash[20]);

/* "VideoEncodingConfig" byte-exact against the desktop app (= seq=17 in the strace).
 * RE 2026-05-13: a critical message, sent after the 8 channel announcements and
 * 4 heartbeats. Without it the server stays in minimal mode (= 1 Mbps with 92%
 * parity redundancy -> partial picture).
 *
 * Wire (S18 2026-08-21, guided `settings` capture - KB §3.24):
 *   Message {
 *       f1 (varint) = seq
 *       f2 (sub)    = Request {
 *           f13 (sub) = kUpdateSession {
 *               f1 (sub) {
 *                   f2 (varint) = 794816   (constant across every capture)
 *                   f4 (varint) = bitrate_bps
 *               }
 *           }
 *       }
 *       f4 (sub) = client_info
 *       f5 (sub) = OCapture
 *   }
 * There is NO frame-rate field: the pre-S18 `f3 fixed32 refresh_hz` does not
 * exist in the real message. The frame rate travels only in the video channel
 * announcement, i.e. once per session (CFG-4 2026-09-11). */
int ctrl_build_video_encoding_config_ex(uint8_t *out, size_t out_cap, uint32_t seq,
                                          uint32_t bitrate_bps);

/* "Ready/StartStreaming" byte-exact against the desktop app. RE 2026-05-09:
 * sent exactly once, after bootstrap. Hypothesis: it makes the server send a
 * FULL FRAME IDR (= top + bottom slices) instead of top-only.
 *
 * Wire:
 *   Message {
 *       f1 (varint) = seq
 *       f2 (sub)    = Request { f7 (sub, len=0) }    <- empty stub
 *       f4 (sub)    = client_info
 *       f5 (sub)    = OCapture
 *   } */
int ctrl_build_ready_msg(uint8_t *out, size_t out_cap, uint32_t seq);

/* Releases the session server-side (oneof field 9). Must be sent BEFORE closing
 * the control channel: without it the server keeps the previous session's
 * subscription and the side channels stay silent on reconnect. */
int ctrl_build_unregister_session(uint8_t *out, size_t out_cap, uint32_t seq);

/* S48: unregisters ONE stream, the way the official client does - it sends
 * eight of them at shutdown. `stream_id < 0` produces its first frame, the one
 * that carries no identifier. */
/* SRV5 2026-10-02 - `stream_id` widened from int to int64_t. The identifier the
 * server grants is a u32 ("sessionId %u" in its logs, a millisecond clock), and
 * roughly half of that range does not fit a non-negative int: truncating it
 * produced a NEGATIVE value, which this builder reads as its "no identifier"
 * sentinel and silently emitted the wrong message. Negative still means the
 * sentinel - deliberately, that is the official client's first frame - but the
 * full u32 can now be passed without colliding with it. */
int ctrl_build_unregister_stream(uint8_t *out, size_t out_cap, uint32_t seq,
                                 int64_t stream_id, uint32_t horodatage);

/* "DisplayReady" byte-exact against the desktop. Follows ready_msg.
 *
 * === SRV-DOC 2026-10-02 - THE NAME IS OURS, NOT THE PROTOCOL'S ===============
 * Request field 6 is the server's **Hid** request:
 * `DispatchControlMessage_` @0x140b2ca30 routes it to
 * `ProcessHidRequestMessage_`. "DisplayReady" was our label for a message we
 * had only ever seen in a capture, and §3.22's "kHid fixed" was already half
 * this discovery. The bytes are byte-exact and work - only the name misleads,
 * so it is kept and annotated rather than churned.
 *
 * Wire:
 *   Message {
 *       f1 (varint) = seq
 *       f2 (sub)    = Request {
 *           f6 (sub) = Sub {
 *               f1 (sub) = { f1 = 1 }
 *               f2 (sub, len=0) = empty
 *           }
 *       }
 *       f4, f5 the same as elsewhere
 *   } */
int ctrl_build_display_ready_msg(uint8_t *out, size_t out_cap, uint32_t seq);

/* Heartbeat / KeepAlive byte-exact against the desktop app.
 * RE 2026-05-09: sent periodically (~1Hz) on SslCtrlChanV2. Without it the
 * server stays in its degraded mode, "send only the top-slice keyframe plus
 * minimal P deltas" -> the bottom of the picture never arrives.
 *
 * Wire:
 *   Message {
 *       f1 (varint) = seq                           <- incremented on every msg
 *       f2 (sub)    = Request { f3 = Sub { f2 = "" } }   <- empty submsg
 *       f4 (sub)    = client_info { f1=2, f2="12.3.3", f3="Linux;x64;App ..." }
 *       f5 (sub)    = { f1=1, f3="OCapture" }
 *   }
 *
 * Total payload = 89 bytes. */
int ctrl_build_heartbeat(uint8_t *out, size_t out_cap, uint32_t seq);

/* HID1 2026-10-02 - Caps/Num/Scroll Lock, Request field 6. See hid_lock.h for
 * what the VM does with it and for which parts of the layout are measured and
 * which are inferred. */
int ctrl_build_hid_locks(uint8_t *out, size_t out_cap, uint32_t seq,
                         const hid_locks *locks);

/* N33 2026-05-14: NotifyResolution.UpdateDisplayConfig - the client->server
 * message that announces a new resolution after bootstrap. Per the Ghidra RE of
 * the desktop client it can force the server to reconfigure its encoder into
 * single-slice full-image mode (= instead of the multi-slice top/bottom that
 * only gives us the top 50%).
 *
 * Wire (Ghidra RE, FUN_010e7d20 NotifyResolution + FUN_010dfc20 UpdateDisplayConfig):
 *   Message {
 *     f1 (varint) = seq
 *     f2 (Request, sub) {
 *       f14 (NotifyResolution, sub) {
 *         f1 (repeated UpdateDisplayConfig, sub) {
 *           f3 (Resolution, sub) {
 *             f1 (varint) = width
 *             f2 (varint) = height
 *           }
 *         }
 *       }
 *     }
 *     f4, f5 = caps + OCapture (= appended by append_caps_and_ocapture)
 *   } */
int ctrl_build_notify_resolution(uint8_t *out, size_t out_cap, uint32_t seq,
                                   uint32_t width, uint32_t height);

/* === SRV9 2026-10-02 - DEAD: 6.3.1 HAS NO HANDLER FOR REQUEST FIELD 15 =======
 * `DispatchControlMessage_` @0x140b2ca30 switches on `field - 2`. Its cases run
 * 0..11, 14..20, 23 and 24, so the request fields it serves are 2..13, 16..22,
 * 25 and 26 - and fields **14, 15, 23 and 24 have no case at all**. They fall
 * into the `default`, which logs `invalid control message (type %d not set)`
 * and answers `SendErrorReply_`.
 * Nothing is behind this message. It has had no call site since it was written
 * and must not acquire one; it is kept only so the next person searching for
 * "NotifyVideoCommand" finds this note instead of re-deriving the message.
 * The same applies to ctrl_build_notify_resolution (field 14) above, which is
 * why SHADOW_SEND_NRES defaults to 0 - that default is now a hard fact, not the
 * stylistic argument K15 had.
 *
 * N38 2026-05-14: NotifyVideoCommand with the kFlush enum.
 * Wire (RE'd hypothesis, still to be confirmed):
 *   Message {
 *     f1 = seq
 *     f2 (Request) {
 *       f15 (NotifyVideoCommand) {
 *         f1 (varint) = command (= kFlush = 0 or 1)
 *       }
 *     }
 *     f4, f5 = caps + OCapture
 *   }
 * Used to flush the server's video encoder -> force a full-image IDR */
int ctrl_build_notify_video_command(uint8_t *out, size_t out_cap, uint32_t seq,
                                      uint32_t command);

/* N40 2026-05-14: Request_Flush (= case 7 of the Request oneof).
 * Ghidra RE confirmed that `kFlush` is case 7 of the Request oneof, NOT a
 * sub-enum of NotifyVideoCommand. Byte-exact wire:
 *   Message {
 *     f1 (varint) = seq
 *     f2 (Request, sub) {
 *       f7 (Request_Flush, sub, len=0) = empty
 *     }
 *     f4, f5 = caps + OCapture
 *   }
 * Wire bytes = `08 SEQ 12 02 3a 00 ...caps...ocapture` */
int ctrl_build_request_flush(uint8_t *out, size_t out_cap, uint32_t seq);

#ifdef __cplusplus
}
#endif
