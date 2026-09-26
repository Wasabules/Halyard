#include "ctrl_msgs.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>   /* rand() */

/* K11 - source of randomness for the upstream key. wolfSSL is already linked
 * everywhere (Switch included), so we use its RNG; fall back to rand() if the
 * init fails. */
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>
static int shadow_random_bytes(uint8_t *out, size_t n) {
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0) return -1;
    int rc = wc_RNG_GenerateBlock(&rng, out, (word32)n);
    wc_FreeRng(&rng);
    return rc == 0 ? 0 : -1;
}
#include <string.h>
#include <time.h>

#include "../common/log.h"
/* S81 - the category is DECLARED here, not inferred from the message text.
 * `mlog` stays at INFO: the existing calls do not disappear. `mdbg` is there for the
 * bulky lines, which migrate to it one at a time. */
#define mlog(...) JOURNAL_INFO_(JOURNAL_CAT_SESSION, __VA_ARGS__)
#define mdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SESSION, __VA_ARGS__)
/* Field numbers in ControlProto:
 *   Message {
 *       Request request = 1;
 *       Reply   reply   = 2;
 *   }
 *   Request {
 *       Authentication  authentication      = 1;
 *       RegisterSession register_session    = 2;
 *       Encryption      encryption          = 3;
 *       ...
 *   }
 *   Reply { same schema mirror }
 */
#define FN_MSG_REQUEST   1
#define FN_REQ_AUTH      1
#define FN_REQ_REGSESS   2
#define FN_REQ_ENCRYPT   3

/* Request_Authentication fields */
#define FN_AUTH_REVERSE_AUTO    1   /* bool */
#define FN_AUTH_PERMISSIONS     2   /* uint32 */
#define FN_AUTH_STREAMINGTOKEN  3   /* string */
#define FN_AUTH_CLIENT_ID       4   /* string */
#define FN_AUTH_SESSION_ID      5   /* string */
#define FN_AUTH_CONNECTION_ID   6   /* string */

/* Encryption fields */

/* --- Encoders ------------------------------------------------------------ */

int ctrl_build_capabilities(uint8_t *out, size_t out_cap,
                              const char *client_version,
                              const char *user_agent) {
    if (!out || !client_version || !user_agent) return -1;

    /* Top-level field 2 (sub): { field 3 (sub) { field 1 (string) = "" } } */
    uint8_t f2_inner_inner[8];
    int f2io = pb_write_string(f2_inner_inner, sizeof(f2_inner_inner), 0, 1, "");
    if (f2io < 0) return -1;
    uint8_t f2[16];
    int f2o = pb_write_submsg(f2, sizeof(f2), 0, 3, f2_inner_inner, f2io);
    if (f2o < 0) return -1;

    /* Top-level field 4 (sub):
     *   field 1 (varint) = 2          (version=2)
     *   field 2 (string) = client_version
     *   field 3 (string) = user_agent
     */
    uint8_t f4[256];
    int f4o = 0;
    f4o = pb_write_uint(f4, sizeof(f4), f4o, 1, 2);
    if (f4o < 0) return -1;
    f4o = pb_write_string(f4, sizeof(f4), f4o, 2, client_version);
    if (f4o < 0) return -1;
    f4o = pb_write_string(f4, sizeof(f4), f4o, 3, user_agent);
    if (f4o < 0) return -1;

    /* Top-level field 5 (sub): { field 1 = 1, field 3 = "OCapture" }
     * A sibling of f2/f4, not nested inside them. */
    uint8_t f5[256];
    int f5o = 0;
    f5o = pb_write_uint(f5, sizeof(f5), f5o, 1, 1);
    if (f5o < 0) return -1;

    /* RE12 2026-05-18 - Capabilities_Streaming sub-message (= probably f5.f2).
     * Agent RE4 finding (C70): the desktop sends this sub-message with 3
     * fields (= codec, chroma, hdr) that our client does not send. Turn it on
     * with SHADOW_CAP_STREAMING=1 to test. Default OFF = byte-exact V14 baseline.
     *
     * Hypothesised layout:
     *   f2 = Capabilities_Streaming {
     *     f2 (varint) = codec enum (= H.264=2 assumed)
     *     f3 (varint) = chroma enum (= 420=0 assumed)
     *     f4 (varint) = hdr_format enum (= None=0 assumed)
     *   }
     */
    static int g_cap_streaming = -1;
    if (g_cap_streaming < 0) {
        const char *e = getenv("SHADOW_CAP_STREAMING");
        g_cap_streaming = e ? atoi(e) : 0;
    }
    if (g_cap_streaming) {
        uint8_t cap_streaming[32]; int cso = 0;
        cso = pb_write_uint(cap_streaming, sizeof(cap_streaming), cso, 2, 2); /* codec=H.264 */
        cso = pb_write_uint(cap_streaming, sizeof(cap_streaming), cso, 3, 0); /* chroma=420 */
        cso = pb_write_uint(cap_streaming, sizeof(cap_streaming), cso, 4, 0); /* hdr=None */
        if (cso > 0) {
            f5o = pb_write_submsg(f5, sizeof(f5), f5o, 2, cap_streaming, cso);
            if (f5o < 0) return -1;
        }
    }

    f5o = pb_write_string(f5, sizeof(f5), f5o, 3, "OCapture");
    if (f5o < 0) return -1;

    /* Top-level: f2 + f4 + f5, in that order, as observed */
    int off = 0;
    off = pb_write_submsg(out, out_cap, off, 2, f2, f2o);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 4, f4, f4o);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 5, f5, f5o);
    return off;
}

/* Helper: append the standard client_info (f4) + OCapture (f5) tail to a message. */
static int append_caps_and_ocapture(uint8_t *out, size_t out_cap, int off) {
    uint8_t f4[256];
    int f4o = 0;
    f4o = pb_write_uint(f4, sizeof(f4), f4o, 1, 2);
    if (f4o < 0) return -1;
    f4o = pb_write_string(f4, sizeof(f4), f4o, 2, "12.3.3");
    if (f4o < 0) return -1;
    f4o = pb_write_string(f4, sizeof(f4), f4o, 3,
                          "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (f4o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 4, f4, f4o);
    if (off < 0) return -1;

    uint8_t f5[64];
    int f5o = 0;
    f5o = pb_write_uint(f5, sizeof(f5), f5o, 1, 1);
    if (f5o < 0) return -1;
    f5o = pb_write_string(f5, sizeof(f5), f5o, 3, "OCapture");
    if (f5o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 5, f5, f5o);
    return off;
}

int ctrl_build_video_encoding_config_ex(uint8_t *out, size_t out_cap, uint32_t seq,
                                          uint32_t bitrate_bps) {
    if (!out) return -1;
    /* === S18 2026-08-21 - STRUCTURE CORRECTED ===
     * Guided capture `captures_settings_20260821_180409`: we filmed the official
     * client while changing the bitrate in its own menu, and decoded the
     * `kUpdateSession` messages it emits. The real message is:
     *     f13 { f1 { f2 = 794816, f4 = bitrate_bps } }
     * with `f2` CONSTANT (identical across both changes) and the bitrate in
     * **f4**. Two measurements: "low" bitrate -> f4 = 1000000, "unlimited"
     * bitrate -> f4 = 70000000.
     * We were writing the bitrate into **f2** and adding a float `f3` fps that
     * does not exist in the real message: the server was therefore reading our
     * bitrate as the constant field, and never saw any value in f4. Our bitrate
     * requests could never have taken effect - which also invalidates the
     * "server ceiling ~14 Mbps" measurement of project_quality_params.
     * CFG-4 2026-09-11: the `fps` parameter is gone. It had been ignored since
     * S18, yet callers kept passing one and logging it as sent. With no
     * parameter the compiler finds every such caller. A live frame rate is an
     * RE item (KB §3.24: kUnregisterSession + re-announcement). */
    uint8_t inner_inner[16];
    int iio = 0;
    iio = pb_write_uint(inner_inner, sizeof(inner_inner), iio, 2, 794816u);
    if (iio < 0) return -1;
    iio = pb_write_uint(inner_inner, sizeof(inner_inner), iio, 4, bitrate_bps);
    if (iio < 0) return -1;

    /* Inner: f1 sub = inner_inner */
    uint8_t inner[24];
    int io = pb_write_submsg(inner, sizeof(inner), 0, 1, inner_inner, iio);
    if (io < 0) return -1;

    /* f2 sub: f13 = inner */
    uint8_t f2[32];
    int f2o = pb_write_submsg(f2, sizeof(f2), 0, 13, inner, io);
    if (f2o < 0) return -1;

    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, f2, f2o);
    if (off < 0) return -1;
    return append_caps_and_ocapture(out, out_cap, off);
}


/* AUD6 2026-08-21 - releasing the session.
 *
 * We used to close our sockets without telling the server anything. The
 * official client emits `kUnregisterSession` (field 9 of the oneof) at the end
 * of a session - the MASTER capture counts eight of them at shutdown.
 *
 * Observed consequence: on reconnect the cursor/audio channel `:base+30` stayed
 * silent, as though the server still held the previous subscription. Video
 * reconnected; the side channels did not. Hence a session that works after a
 * cold start, and silent reconnections.
 */
int ctrl_build_unregister_session(uint8_t *out, size_t out_cap, uint32_t seq) {
    if (!out) return -1;
    /* f2 = Request { f9 (empty sub-message) } */
    uint8_t f2[8];
    int f2o = 0;
    f2[f2o++] = (9 << 3) | 2;  /* champ 9, wire 2 */
    f2[f2o++] = 0;              /* longueur 0 */

    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, f2, f2o);
    if (off < 0) return -1;
    return append_caps_and_ocapture(out, out_cap, off);
}

/* === S48 2026-08-26 - UNREGISTER EACH STREAM, ONE BY ONE ===
 *
 * `ctrl_build_unregister_session` above emits an EMPTY `f9`. The capture of the
 * official client (same VM, with sound) shows it sends not one but EIGHT of
 * them at shutdown, each naming a stream:
 *
 *     08 b9 02  12 0a 4a 08 0a 06 08 03 10 be fa 46    <- stream 3
 *     08 ba 02  12 0a 4a 08 0a 06 08 01 10 da fa 46    <- stream 1
 *     ...                                                 then 2, 5, 6, 4, 7
 *
 * that is, `f9 { f1 { f1 = stream id, f2 = timestamp } }`. An empty message
 * tells the server NOTHING about what to release.
 *
 * This is the only explanation compatible with every measurement we have: the
 * audio channel `:base+30` starts on only one session in three, and the
 * server's status report then names a stream still attached to an earlier
 * session (8/8 correlation, KB §3.35). If we release nothing, the next
 * session inherits a subscription still held elsewhere.
 *
 * `stream_id < 0` produces the official client's first frame, the one with no
 * identifier: `f9 { f1 { f2 = timestamp, f3 = 0 } }`. */
int ctrl_build_unregister_stream(uint8_t *out, size_t out_cap, uint32_t seq,
                                 int stream_id, uint32_t horodatage) {
    if (!out) return -1;
    uint8_t inner[24];
    int io = 0;
    if (stream_id >= 0) {
        io = pb_write_uint(inner, sizeof(inner), io, 1, (uint32_t)stream_id);
        if (io < 0) return -1;
    }
    io = pb_write_uint(inner, sizeof(inner), io, 2, horodatage);
    if (io < 0) return -1;
    if (stream_id < 0) {
        io = pb_write_uint(inner, sizeof(inner), io, 3, 0);
        if (io < 0) return -1;
    }
    uint8_t f9[32];
    int f9o = pb_write_submsg(f9, sizeof(f9), 0, 1, inner, io);
    if (f9o < 0) return -1;

    uint8_t f2[48];
    int f2o = pb_write_submsg(f2, sizeof(f2), 0, 9, f9, f9o);
    if (f2o < 0) return -1;

    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, f2, f2o);
    if (off < 0) return -1;
    return append_caps_and_ocapture(out, out_cap, off);
}

int ctrl_build_ready_msg(uint8_t *out, size_t out_cap, uint32_t seq) {
    if (!out) return -1;
    /* f2 = Request { f7 (sub len=0) } */
    uint8_t f2[8];
    int f2o = 0;
    f2[f2o++] = (7 << 3) | 2;  /* tag field 7 wire 2 */
    f2[f2o++] = 0;              /* len 0 */

    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, f2, f2o);
    if (off < 0) return -1;
    return append_caps_and_ocapture(out, out_cap, off);
}

int ctrl_build_display_ready_msg(uint8_t *out, size_t out_cap, uint32_t seq) {
    if (!out) return -1;
    /* f2 = Request { f6 = Sub { f1 = { f1=1 }, f2 = empty } } */
    uint8_t f2_inner[16];
    int f2io = 0;
    /* inner f1 (sub len=2) = { f1 = 1 } */
    f2_inner[f2io++] = (1 << 3) | 2; f2_inner[f2io++] = 2;
    f2_inner[f2io++] = (1 << 3) | 0; f2_inner[f2io++] = 1;
    /* inner f2 (sub len=0) */
    f2_inner[f2io++] = (2 << 3) | 2; f2_inner[f2io++] = 0;
    /* inner f3 (sub len=0) - S5 2026-08-21.
     * The LD_PRELOAD capture of the official client (fd=318 :14011, seq 14 and
     * 16, 99 B frame) gives `32 08 0a 02 08 01 12 00 1a 00`, i.e. THREE
     * sub-fields inside kHid: f1{f1=1}, f2{}, f3{}. We emitted only two, so our
     * frame was 97 B instead of 99 B. kHid (= case 6 of the Request oneof, see
     * memory/project_ctrl_oneof_enum_VERIFIED.md) is the message that declares
     * the input devices; it was the last remaining divergence on an
     * input-related message. SHADOW_HID_F3=0 restores the old behaviour. */
    {
        static int g_hid_f3 = -1;
        if (g_hid_f3 < 0) {
            const char *e = getenv("SHADOW_HID_F3");
            g_hid_f3 = e ? atoi(e) : 1;
        }
        if (g_hid_f3) {
            f2_inner[f2io++] = (3 << 3) | 2; f2_inner[f2io++] = 0;
        }
    }

    uint8_t f2[16];
    int f2o = pb_write_submsg(f2, sizeof(f2), 0, 6, f2_inner, f2io);
    if (f2o < 0) return -1;

    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, f2, f2o);
    if (off < 0) return -1;
    return append_caps_and_ocapture(out, out_cap, off);
}

int ctrl_build_heartbeat(uint8_t *out, size_t out_cap, uint32_t seq) {
    if (!out) return -1;

    /* Top-level f2 (sub): Request { f3 (sub) { f2 = "" } } */
    uint8_t f2_inner_inner[8];
    int f2io = pb_write_string(f2_inner_inner, sizeof(f2_inner_inner), 0, 2, "");
    if (f2io < 0) return -1;
    uint8_t f2[16];
    int f2o = pb_write_submsg(f2, sizeof(f2), 0, 3, f2_inner_inner, f2io);
    if (f2o < 0) return -1;

    /* Top-level f4 = client info (= same as Capabilities) */
    uint8_t f4[256];
    int f4o = 0;
    f4o = pb_write_uint(f4, sizeof(f4), f4o, 1, 2);
    if (f4o < 0) return -1;
    f4o = pb_write_string(f4, sizeof(f4), f4o, 2, "12.3.3");
    if (f4o < 0) return -1;
    f4o = pb_write_string(f4, sizeof(f4), f4o, 3,
                          "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (f4o < 0) return -1;

    /* Top-level f5 = OCapture (= same as Capabilities) */
    uint8_t f5[64];
    int f5o = 0;
    f5o = pb_write_uint(f5, sizeof(f5), f5o, 1, 1);
    if (f5o < 0) return -1;
    f5o = pb_write_string(f5, sizeof(f5), f5o, 3, "OCapture");
    if (f5o < 0) return -1;

    /* Wire: f1=seq, f2=Request, f4=client, f5=OCapture */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, f2, f2o);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 4, f4, f4o);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 5, f5, f5o);
    return off;
}

/* N33 2026-05-14: NotifyResolution.UpdateDisplayConfig.
 * Wire RE'd with Ghidra:
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
 *     f4, f5 = caps + OCapture
 *   }
 */
int ctrl_build_notify_resolution(uint8_t *out, size_t out_cap, uint32_t seq,
                                   uint32_t width, uint32_t height) {
    if (!out) return -1;

    /* Resolution sub: f1=width, f2=height */
    uint8_t res[16]; int ro = 0;
    ro = pb_write_uint(res, sizeof(res), ro, 1, width);
    if (ro < 0) return -1;
    ro = pb_write_uint(res, sizeof(res), ro, 2, height);
    if (ro < 0) return -1;

    /* UpdateDisplayConfig sub: f3=Resolution */
    uint8_t udc[24]; int udco = 0;
    udco = pb_write_submsg(udc, sizeof(udc), udco, 3, res, ro);
    if (udco < 0) return -1;

    /* NotifyResolution sub: f1=UpdateDisplayConfig (repeated, 1 elem) */
    uint8_t nr[32]; int nro = 0;
    nro = pb_write_submsg(nr, sizeof(nr), nro, 1, udc, udco);
    if (nro < 0) return -1;

    /* Request sub: f14=NotifyResolution */
    uint8_t req[48]; int reqo = 0;
    reqo = pb_write_submsg(req, sizeof(req), reqo, 14, nr, nro);
    if (reqo < 0) return -1;

    /* Top-level Message */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, req, reqo);
    if (off < 0) return -1;
    return append_caps_and_ocapture(out, out_cap, off);
}

/* N38 2026-05-14: NotifyVideoCommand (Request case 15).
 * Per the RE of the Android client, the VideoCommand enum has kFlush as one of
 * its values. */
int ctrl_build_notify_video_command(uint8_t *out, size_t out_cap, uint32_t seq,
                                      uint32_t command) {
    if (!out) return -1;

    /* NotifyVideoCommand sub: f1 varint = command */
    uint8_t nvc[8]; int nvco = 0;
    nvco = pb_write_uint(nvc, sizeof(nvc), nvco, 1, command);
    if (nvco < 0) return -1;

    /* Request sub: f15 = NotifyVideoCommand */
    uint8_t req[16]; int reqo = 0;
    reqo = pb_write_submsg(req, sizeof(req), reqo, 15, nvc, nvco);
    if (reqo < 0) return -1;

    /* Top-level Message */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, req, reqo);
    if (off < 0) return -1;
    return append_caps_and_ocapture(out, out_cap, off);
}

/* N40 2026-05-14: Request_Flush (= case 7 of the Request oneof, KFlush).
 * Empty body, just the f7 sub tag with len=0. */
int ctrl_build_request_flush(uint8_t *out, size_t out_cap, uint32_t seq) {
    if (!out) return -1;
    /* Request sub: f7 sub len 0 (empty Flush) */
    uint8_t req[8]; int reqo = 0;
    req[reqo++] = (7 << 3) | 2;  /* tag field 7 wire 2 */
    req[reqo++] = 0;              /* len 0 */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, req, reqo);
    if (off < 0) return -1;
    return append_caps_and_ocapture(out, out_cap, off);
}

int ctrl_build_authentication(uint8_t *out, size_t out_cap,
                                bool reverse_auto_register,
                                uint32_t permissions,
                                const char *streamingtoken,
                                const char *client_id,
                                const char *session_unique_id,
                                const char *connection_unique_id) {
    (void)connection_unique_id;  /* the desktop app does not send it - drop */

    /* Authentication wire format, byte-exact against the desktop app
     * (LD_PRELOAD capture 2026-05-08, line 13180):
     *
     *   Message {
     *       field 1 (varint) = 1                  <- version/marker
     *       field 2 (sub) = Request {
     *           field 4 (sub) = AuthenticationBody {
     *               field 2 (varint) = 1          <- reverse_auto_register
     *               field 3 (varint) = permissions  (= 0x1fe = 510)
     *               field 4 (string) = streaming_token
     *               field 5 (string) = sessionUniqueId  (UUID 36 chars)
     *               field 6 (string) = timestamp_ms ASCII (e.g. "1778274158715")
     *               field 7 (sub) = {field 1 (sub) = {field 1 = 1, field 2 = 1}}
     *               field 8 (string) = client_id  ("<user-id-A>-...-main")
     *           }
     *       }
     *       field 4 (sub) = client capabilities meta
     *       field 5 (sub) = OCapture
     *   } */

    /* Step 1: AuthenticationBody (inner of Request.field4) */
    uint8_t body[1024];
    int bo = 0;
    bo = pb_write_uint(body, sizeof(body), bo, 2, reverse_auto_register ? 1 : 1);
    if (bo < 0) return -1;
    bo = pb_write_uint(body, sizeof(body), bo, 3, permissions);
    if (bo < 0) return -1;
    if (streamingtoken && *streamingtoken) {
        bo = pb_write_string(body, sizeof(body), bo, 4, streamingtoken);
        if (bo < 0) return -1;
    }
    if (session_unique_id && *session_unique_id) {
        bo = pb_write_string(body, sizeof(body), bo, 5, session_unique_id);
        if (bo < 0) return -1;
    }
    /* timestamp_ms ASCII */
    {
        char ts[24];
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
        long long ms = (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000LL;
        snprintf(ts, sizeof(ts), "%lld", ms);
        bo = pb_write_string(body, sizeof(body), bo, 6, ts);
        if (bo < 0) return -1;
    }
    /* field 7 = channels_per_stream map (StreamType -> set<ChannelType>).
     * RE V9 (sub_853770 + L30352) confirms this is a map<StreamType, set<ChannelType>>.
     * Our historical code sent a SINGLE entry, Video -> UDP, so the server read
     * us as an "entry-tier" client and downgraded us. F3 fix: declare 9 stream
     * types.
     *
     * Byte-exact wire encoding observed (desktop 2026-05-08) for 1 entry:
     *   `3a 06 0a 04 08 01 10 01`
     *   = f7 (3a = tag field 7 wire type 2) len=6 contains:
     *     f1 (0a = tag field 1 wire type 2) len=4 contains:
     *       f1=1 (Video) f2=1 (UDP)
     *
     * F3 expansion: repeat the pattern for 9 stream types.
     * Toggle with SHADOW_AUTH_FULL_MAP=0 to go back to the V1 behaviour.
     * See tools/ida/out/H1_V9_server_discrimination.md sections D.4-D.5 */
    {
        static int g_full_map = -1;
        if (g_full_map < 0) {
            const char *e = getenv("SHADOW_AUTH_FULL_MAP");
            /* S4 2026-08-21: the default is now OFF. A byte-by-byte dump of
             * our Authentication next to the desktop's (captures /tmp/boot/
             * desktop_plaintext.log) shows the official client sends ONLY ONE
             * entry, `Video -> UDP` (f4 = 156 B). F3 sent 9, i.e. exactly
             * +64 B, and declared `Input -> UDP` when input actually runs over
             * TCP :base+14. F3 was a hypothesis ("look entry-tier") that was
             * never checked against the capture. */
            g_full_map = e ? atoi(e) : 0;
        }
        /* StreamType enum mirror (= per V9 §D.3):
         *   1=Video, 2=Audio, 3=AudioIn, 4=Cursor, 5=Input,
         *   6=Mic, 7=Clipboard, 8=Gamepad, 9=FileTransfer.
         * ChannelType: 1=UDP, 2=SSL_TCP, 3=STFP_SSL_TCP, 4=STFP_TCP. */
        struct { uint8_t stream; uint8_t channel; } map_entries[] = {
            {1, 1}, /* Video    → UDP */
            {2, 1}, /* Audio    → UDP */
            {3, 1}, /* AudioIn  → UDP */
            {4, 1}, /* Cursor   → UDP */
            {5, 1}, /* Input    → UDP */
            {6, 1}, /* Mic      → UDP */
            {7, 2}, /* Clipboard → SSL_TCP */
            {8, 2}, /* Gamepad  → SSL_TCP */
            {9, 2}, /* FileTransfer → SSL_TCP */
        };
        int n_entries = g_full_map ? (int)(sizeof(map_entries)/sizeof(map_entries[0])) : 1;
        for (int i = 0; i < n_entries; i++) {
            uint8_t inner_inner[8];
            int iio = 0;
            iio = pb_write_uint(inner_inner, sizeof(inner_inner), iio, 1, map_entries[i].stream);
            iio = pb_write_uint(inner_inner, sizeof(inner_inner), iio, 2, map_entries[i].channel);
            if (iio < 0) return -1;
            uint8_t inner[16];
            int io = pb_write_submsg(inner, sizeof(inner), 0, 1, inner_inner, iio);
            if (io < 0) return -1;
            bo = pb_write_submsg(body, sizeof(body), bo, 7, inner, io);
            if (bo < 0) return -1;
        }
    }
    if (client_id && *client_id) {
        bo = pb_write_string(body, sizeof(body), bo, 8, client_id);
        if (bo < 0) return -1;
    }

    /* Step 2: wrap in Request {field 4 = AuthenticationBody} */
    uint8_t req[1100];
    int ro = pb_write_submsg(req, sizeof(req), 0, 4, body, bo);
    if (ro < 0) return -1;

    /* Step 3: top-level Message {field 1 = 1, field 2 = Request, field 4 = caps, field 5 = OCapture}
     * We embed the same Capabilities meta + OCapture as in the Capabilities step. */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, 1);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, req, ro);
    if (off < 0) return -1;

    /* field 4 capabilities meta */
    uint8_t f4[256]; int f4o = 0;
    f4o = pb_write_uint(f4, sizeof(f4), f4o, 1, 2);
    f4o = pb_write_string(f4, sizeof(f4), f4o, 2, "12.3.3");
    f4o = pb_write_string(f4, sizeof(f4), f4o, 3,
        "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (f4o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 4, f4, f4o);
    if (off < 0) return -1;

    /* field 5 OCapture */
    uint8_t f5[32]; int f5o = 0;
    f5o = pb_write_uint(f5, sizeof(f5), f5o, 1, 1);
    f5o = pb_write_string(f5, sizeof(f5), f5o, 3, "OCapture");
    if (f5o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 5, f5, f5o);
    return off;
}

/* S3 2026-08-21 - FULL EDID (384 B) captured from the official client 13.1.5.
 * The desktop sends the REAL EDID of the attached panel: valid header
 * `00 ff ff ff ff ff ff 00`, `fc` descriptor = "PL2771Q" (Iiyama), and 384
 * bytes = base block + 2 extensions. We were sending only the 128 bytes of the
 * base block, hence a 243 B RegisterSession against 413 B on the desktop side
 * (see KB §3.20). Hypothesis: an incomplete session registration is
 * still served video, but is never granted input.
 * SHADOW_EDID_FULL=0 goes back to the 128 B block. */
static const uint8_t SHADOW_EDID_384B[384] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x26, 0xcd, 0xe4, 0x66,
    0x01, 0x01, 0x01, 0x01, 0x14, 0x23, 0x01, 0x03, 0x80, 0x3c, 0x22, 0x78,
    0x2a, 0x74, 0x15, 0xa6, 0x56, 0x51, 0x9e, 0x26, 0x0e, 0x50, 0x54, 0x37,
    0x4f, 0x00, 0x81, 0xc0, 0x81, 0x80, 0xd1, 0xc0, 0x95, 0x00, 0xb3, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0,
    0x29, 0x50, 0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1e,
    0x00, 0x00, 0x00, 0xff, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x50,
    0x4c, 0x32, 0x37, 0x37, 0x31, 0x51, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x00, 0x00, 0x00, 0xfd, 0x00, 0x31, 0xc8, 0x1e, 0xf0, 0x3c, 0x00, 0x0a,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0xa9, 0x02, 0x03, 0x42, 0xf1,
    0xe2, 0x78, 0x02, 0x4e, 0x3f, 0x40, 0x90, 0x1f, 0x22, 0x21, 0x20, 0x04,
    0x13, 0x12, 0x11, 0x03, 0x02, 0x01, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01,
    0x00, 0x00, 0x67, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x38, 0x44, 0x6d, 0xd8,
    0x5d, 0xc4, 0x01, 0x78, 0x80, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xe3, 0x05, 0xc1, 0x01, 0xe6, 0x06, 0x05, 0x01, 0x59, 0x59, 0x1c, 0xe2,
    0x00, 0xea, 0x6f, 0xc2, 0x00, 0xa0, 0xa0, 0xa0, 0x55, 0x50, 0x30, 0x20,
    0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1a, 0x17, 0xea, 0x00, 0xa0,
    0xa0, 0xa0, 0x5a, 0x50, 0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00,
    0x00, 0x1a, 0x2a, 0x44, 0x80, 0xa0, 0x70, 0x38, 0x27, 0x40, 0x30, 0x20,
    0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xe9, 0x70, 0x12, 0x79, 0x03, 0x00, 0x03, 0x01, 0x14,
    0x5b, 0xc1, 0x00, 0x04, 0x7f, 0x07, 0x9f, 0x00, 0x2f, 0x80, 0x1f, 0x00,
    0x37, 0x04, 0x6d, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x99, 0x90,
};

/* 128-byte EDID captured from the desktop app (display 1920x1080).
 * H5 fix V8 (see tools/ida/out/H1_V8_unexplored.md): the initial capture was
 * off by one. Byte 126 (= 0x7E, "extension block count") held `0xc1` (= 193
 * bogus blocks) and byte 127 (= 0x7F, checksum) was an implicit 0x00 (= the
 * default, because the initializer was only 127 bytes long). String found in
 * the desktop binary:
 *   "EDID declared inconsistent number of extension blocks {}"
 * -> the server may then fall back to a safe encoder (= top slice only).
 * Fix: byte 126 = 0x00 (= 0 extensions), byte 127 = 0xc1 (= checksum recomputed
 * so that sum(0..127) mod 256 == 0). */
static const uint8_t SHADOW_EDID_128B[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x06, 0xaf, 0xed, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x1c, 0x01, 0x04, 0xa5, 0x22, 0x13, 0x78, 0x02, 0x6a, 0x75, 0xa4, 0x56, 0x52, 0x9c, 0x27,
    0x0b, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xce, 0x8f, 0x80, 0xb6, 0x70, 0x38, 0x88, 0x40, 0x30, 0x20,
    0xa5, 0x00, 0x58, 0xc2, 0x10, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x4b, 0x30,
    0x35, 0x35, 0x47, 0x80, 0x42, 0x31, 0x35, 0x36, 0x48, 0x41, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x41, 0x0f, 0x9e, 0x00, 0x11, 0x00, 0x00, 0x0b, 0x01, 0x0a, 0x20, 0x20, 0x00, 0x00, 0xc1,
};

/* 8 ChannelInfo bodies captured byte by byte (LD_PRELOAD 2026-05-08, lines
 * 14020-14077). Each body = the inner ChannelInfo (without the field 8 wrap and
 * without the Message). seq f1 = 5..12; body[0..7] maps to seq 5..12. */
typedef struct {
    const uint8_t *body;
    size_t len;
} chan_info_t;

#define CI(b) {(const uint8_t[])b, sizeof((const uint8_t[])b)}

static const uint8_t CI_BODY_5[] = {
    0x0a, 0x1a, 0x0a, 0x06, 0x22, 0x04, 0x10, 0x02, 0x20, 0x01, 0x12, 0x0b,
    0x08, 0x80, 0x0f, 0x10, 0xb8, 0x08, 0x1d, 0xe1, 0xda, 0x0f, 0x43,
    0x38, 0x80, 0xda, 0xc4, 0x09
};
static const uint8_t CI_BODY_6[] = {
    0x22, 0x0a, 0x0a, 0x08, 0x22, 0x06, 0x10, 0x04, 0x18, 0x01, 0x20, 0x01
};
static const uint8_t CI_BODY_7[] = {
    0x1a, 0x0a, 0x0a, 0x06, 0x22, 0x04, 0x08, 0x04, 0x20, 0x01, 0x12, 0x00
};
static const uint8_t CI_BODY_8[] = {
    0x12, 0x0e, 0x0a, 0x04, 0x22, 0x02, 0x20, 0x01, 0x10, 0x80, 0xf7, 0x02,
    0x18, 0x10, 0x20, 0x01
};
static const uint8_t CI_BODY_9[] = {
    0x32, 0x06, 0x0a, 0x04, 0x22, 0x02, 0x20, 0x01
};
static const uint8_t CI_BODY_10[] = {
    0x3a, 0x08, 0x0a, 0x06, 0x22, 0x04, 0x18, 0x01, 0x20, 0x01
};
static const uint8_t CI_BODY_11[] = {
    0x2a, 0x0e, 0x0a, 0x04, 0x22, 0x02, 0x20, 0x01, 0x10, 0x80, 0xf7, 0x02,
    0x18, 0x10, 0x20, 0x01
};
static const uint8_t CI_BODY_12[] = {
    0x42, 0x0a, 0x0a, 0x08, 0x22, 0x06, 0x08, 0x05, 0x18, 0x01, 0x20, 0x01
};

/* Requested audio codec, as its wire value: 1 = Opus (default), 2 = FLAC.
 * Set before bootstrap by ctrl_msgs_set_audio_codec(). */
static uint32_t g_audio_codec_wire = 1;

void ctrl_msgs_set_audio_codec(uint32_t codec_wire)
{
    g_audio_codec_wire = (codec_wire == 2) ? 2u : 1u;
}

/* B4 - video transport requested in the announcement. Set once per session by
 * ctrl_session, for the reason spelled out at the call site below: the REQUEST
 * and the RECEIVE side must agree, and they used to be two independent
 * `getenv` caches frozen on their first call. A setting changed between two
 * sessions would have moved one and not the other - that is a session with no
 * picture, produced by the fix rather than by the bug. */
static int g_video_net_tcp = 0;

void ctrl_msgs_set_video_tcp(int on)
{
    g_video_net_tcp = on ? 1 : 0;
}

static const chan_info_t SHADOW_CHANNEL_INFO_BODIES[8] = {
    {CI_BODY_5,  sizeof(CI_BODY_5)},
    {CI_BODY_6,  sizeof(CI_BODY_6)},
    {CI_BODY_7,  sizeof(CI_BODY_7)},
    {CI_BODY_8,  sizeof(CI_BODY_8)},
    {CI_BODY_9,  sizeof(CI_BODY_9)},
    {CI_BODY_10, sizeof(CI_BODY_10)},
    {CI_BODY_11, sizeof(CI_BODY_11)},
    {CI_BODY_12, sizeof(CI_BODY_12)},
};

/* Q1 2026-05-18 - builder for the video channel body (chan_idx=0) with custom
 * parameters. Reproduces the byte-exact structure of CI_BODY_5, but with
 * overridable values.
 *
 * Decoded protobuf layout (= 28 bytes in total with the desktop values):
 *   f1 (Sub, len 26) {                   <- outer wrapper, contents =
 *     f1 (Sub, len 6) {                  <- codec_config sub
 *       f4 (Sub, len 4) {                <- inner codec_enum
 *         f2 varint = codec              <- 2 in CI_BODY_5 (= H.264 assumed)
 *         f4 varint = profile            <- 1 in CI_BODY_5
 *       }
 *     }
 *     f2 (Sub, len 11) {                 <- resolution+fps sub
 *       f1 varint = width
 *       f2 varint = height
 *       f3 fixed32 = fps
 *     }
 *     f7 varint = max_bitrate_bps        <- the cap, inside the outer, not top-level
 *   }
 */
static int build_video_chan_body(uint8_t *out, size_t cap,
                                 const ctrl_video_params_t *p) {
    if (!out || !p) return -1;

    /* === K15 2026-08-28 - THIS SUB-MESSAGE IS `StreamingProtocol`, AND WE WERE
     * WRITING THE PROFILE INTO ITS ENCRYPTION FLAG ===
     *
     * Schema read from the serialiser inside the official binary, cross-checked
     * against our eight byte-exact bodies and against what the server grants:
     *
     *   f1 = protoType    (SUFP=0 so absent, SSUFP=1, SSP=2, SCP=3,
     *                      FlatBuffers=4, SFTP=5)
     *   f2 = unidentified (literally: video=2, cursor=4, gamepad=4)
     *   f3 = networkType   (UDP=0 so absent, TCP=1, QUIC=2)  <-- THE TRANSPORT
     *   f4 = "channel is encrypted" boolean (equals 1 on ALL EIGHT channels)
     *   f5 = granted port  (filled in by the SERVER: 7000 + offset)
     *
     * The previous comment called this "codec_enum", and we were writing
     * `p->profile` into f4. But f4 is `20 01` on all eight captured channels,
     * including the three the official client is granted over TCP: it is a
     * boolean, not a profile. Choosing "Maximum quality" (profile 2) therefore
     * emitted `20 02` into the encryption flag - a value that does not exist -
     * instead of requesting anything at all.
     *
     * This is EXACTLY the shape of K13, where our codec selector wrote into a
     * field with no effect for months. Second occurrence inside the same
     * sub-message: the lesson is not "check the codec", it is "check EVERY field
     * against the serialiser, not against a comment".
     *
     * So f4 goes back to being the byte-exact constant. The real transport is
     * f3, but ASKING for it without being able to RECEIVE it would kill the
     * picture: in `reliability` mode the server stops opening the UDP socket,
     * the framing moves from SUFP to STFP (there is no `sufp_tcp_io_channel.cpp`
     * on the official side) and the `rG` NACK disappears.
     * `SHADOW_VIDEO_NET_TCP=1` sends the request anyway: that is the experiment
     * of §5.2 of the K15 report - it answers "does the server grant TCP
     * on this account?" without writing the ~500 lines of the port, at the cost
     * of a session with no picture. */
    uint8_t inner_f4[8]; int i4 = 0;
    i4 = pb_write_uint(inner_f4, sizeof(inner_f4), i4, 2, p->codec);
    if (i4 < 0) return -1;
    /* B4 - the value comes from ctrl_session, which resolved it ONCE for this
     * session (env var, else the setting). It used to be a second, independent
     * `getenv` cache here: two caches for one decision, each frozen on its own
     * first call. As long as the toggle only lived in `env.txt` they could not
     * disagree; driven by a setting they would, and disagreeing is precisely
     * the failure this whole block warns about. */
    if (g_video_net_tcp) {
        i4 = pb_write_uint(inner_f4, sizeof(inner_f4), i4, 3, 1);  /* TCP */
        if (i4 < 0) return -1;
    }
    /* f4: the encryption boolean, 1 on all eight captured channels. Do NOT put
     * `p->profile` back here - that is exactly what K15 just fixed. */
    i4 = pb_write_uint(inner_f4, sizeof(inner_f4), i4, 4, 1);
    if (i4 < 0) return -1;
    (void)p->profile;   /* the UI selector is waiting for its real field (K15 §5.3) */

    /* Inner f1 sub wrapping f4 */
    uint8_t inner_f1[16];
    int i1 = pb_write_submsg(inner_f1, sizeof(inner_f1), 0, 4, inner_f4, i4);
    if (i1 < 0) return -1;

    /* Inner f2 sub = resolution+fps */
    uint8_t inner_f2[20]; int i2 = 0;
    i2 = pb_write_uint(inner_f2, sizeof(inner_f2), i2, 1, p->width);
    if (i2 < 0) return -1;
    i2 = pb_write_uint(inner_f2, sizeof(inner_f2), i2, 2, p->height);
    if (i2 < 0) return -1;
    if (i2 + 5 > (int)sizeof(inner_f2)) return -1;
    inner_f2[i2++] = (3 << 3) | 5;  /* tag f3 wire fixed32 */
    memcpy(inner_f2 + i2, &p->fps, 4);
    i2 += 4;

    /* Combine inner_f1 + inner_f2 + inner f7 (=max_bitrate) into outer f1 wrapper */
    uint8_t combined[128]; int co = 0;
    co = pb_write_submsg(combined, sizeof(combined), co, 1, inner_f1, i1);
    if (co < 0) return -1;
    co = pb_write_submsg(combined, sizeof(combined), co, 2, inner_f2, i2);
    if (co < 0) return -1;
    co = pb_write_uint(combined, sizeof(combined), co, 7, p->max_bitrate_bps);
    if (co < 0) return -1;

    /* RE8 2026-05-18 - 5 bool fields identified by decompiling sub_10E60A0 (C95 wire).
     * Wire tags: f4=0x20, f5=0x28, f6=0x30, f8=0x40, f10=0x50.
     * field 10 = multi-NAL candidate #1 (= an isolated outlier at offset +52).
     * field 8 = multi-NAL candidate #2 (= a late addition mid-struct).
     * Each can be switched on alone for A/B testing (= avoids breaking bootstrap).
     * Encoded ONLY when true (= keeps the V14 byte-exact baseline when all false).
     *
     * NOTE: the RE3 fields (cursor_merged/high_color_fidelity/hdr_enabled/vr_enabled)
     * are NOT encoded here, because field numbers 8/9/10/11 either collide with
     * RE8 or are simply wrong. Kept for API compatibility, no wire effect. */
    /* K13 - THE codec, the real one. Field 3 at the video level, emitted ONLY
     * when non-zero: H.264 is the proto3 default, and the official client then
     * writes nothing at all. Emitting `18 00` would depart from byte-exact. */
    if (p->codec_wire)
        co = pb_write_uint(combined, sizeof(combined), co, 3, p->codec_wire);
    if (co < 0) return -1;

    if (p->re8_f4)  co = pb_write_uint(combined, sizeof(combined), co, 4, 1);
    /* K13 - `re8_f5` IS the 4:4:4 toggle (`high_color_fidelity`), now identified.
     * "Set chroma VC444 requested" -> an announcement carrying `28 01`; "VC420"
     * -> the field is absent. Static RE already names it `high_color_fidelity`
     * in the format string "... cursor merged: {}, high color fidelity: {}, hdr:
     * {}, vr: {}". There is NO "chroma" field on the wire at all: 4:4:4 is this
     * single bit.
     * WHICH EXPLAINS WHY F20 FAILED: that campaign set it to 1 by default, then
     * reverted, with the GUI stuck on "Waiting for video..." for an "unknown
     * reason". The reason is measurable in the official capture - its telemetry
     * goes from `"chroma":"VC420","mode":"CUDA"` to
     * `"chroma":"VC444","mode":"Software"`: turning 4:4:4 on LOSES hardware
     * decoding, even for the official client on an NVIDIA machine.
     * On Switch that is final: the Tegra X1's NVDEC decodes 4:4:4 on NO codec,
     * and the installed FFmpeg does not even offer the pixel format. */
    if (p->re8_f5)  co = pb_write_uint(combined, sizeof(combined), co, 5, 1);
    if (p->re8_f6)  co = pb_write_uint(combined, sizeof(combined), co, 6, 1);
    if (p->re8_f8)  co = pb_write_uint(combined, sizeof(combined), co, 8, 1);
    if (p->re8_f10) co = pb_write_uint(combined, sizeof(combined), co, 10, 1);
    /* Suppress unused warning for RE3 fields (kept for ABI compat) */
    (void)p->cursor_merged;
    (void)p->high_color_fidelity;
    (void)p->hdr_enabled;
    (void)p->vr_enabled;
    if (co < 0) return -1;

    /* Outer f1 wraps everything */
    int off = 0;
    off = pb_write_submsg(out, cap, off, 1, combined, co);
    if (off < 0) return -1;
    return off;
}

int ctrl_build_channel_announcement_ex(uint8_t *out, size_t out_cap,
                                         int seq, int chan_idx,
                                         const ctrl_video_params_t *vparams) {
    if (!out || chan_idx < 0 || chan_idx >= 8) return -1;

    /* Scratch buffer for the body. chan_idx=0 plus vparams -> custom build. */
    uint8_t custom_body[128];
    const uint8_t *body_ptr;
    size_t body_len;

    if (chan_idx == 0 && vparams) {
        int cb = build_video_chan_body(custom_body, sizeof(custom_body), vparams);
        if (cb < 0) return -1;
        body_ptr = custom_body;
        body_len = (size_t)cb;
    } else {
        const chan_info_t *ci = &SHADOW_CHANNEL_INFO_BODIES[chan_idx];
        body_ptr = ci->body;
        body_len = ci->len;

        /* === K14 2026-08-27 - THE AUDIO CODEC FITS IN ONE BYTE ===
         *
         * `CI_BODY_8` is the AUDIO announcement (its first byte `0x12` = field 2
         * of the outer message, and the bootstrap mapping gives f2 = Audio). Its
         * LAST byte is the codec: `01` = Opus, `02` = FLAC.
         *
         * Diffing the two official captures: between an Opus session (21/08) and
         * a FLAC session (26/08) this is the ONLY byte that changes in the whole
         * message - everything else, including the 87 bytes of capabilities, is
         * identical. The server echoes the granted value back in its reply,
         * along with the port (`28 f6 36` = 7030).
         *
         * So we patch a COPY, never the constant: the eight bodies are
         * byte-exact captures and must stay that way - that is what lets
         * `test_ctrl_msgs.c` catch a one-byte drift.
         *
         * The microphone (`CI_BODY_11`) is NOT touched: in that same official
         * session it stays on Opus while the output is on FLAC. */
        if (chan_idx == 3 && g_audio_codec_wire > 1 && body_len <= sizeof custom_body) {
            memcpy(custom_body, body_ptr, body_len);
            custom_body[body_len - 1] = (uint8_t)g_audio_codec_wire;
            body_ptr = custom_body;
        }
    }

    /* Request {f8 = ChannelInfo body} */
    uint8_t req[256];
    int ro = pb_write_submsg(req, sizeof(req), 0, 8, body_ptr, body_len);
    if (ro < 0) return -1;

    /* Top-level Message {f1=seq, f2=Request, f4=caps, f5=OCapture} */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, req, ro);
    if (off < 0) return -1;

    uint8_t f4[256]; int f4o = 0;
    f4o = pb_write_uint(f4, sizeof(f4), f4o, 1, 2);
    f4o = pb_write_string(f4, sizeof(f4), f4o, 2, "12.3.3");
    f4o = pb_write_string(f4, sizeof(f4), f4o, 3,
        "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (f4o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 4, f4, f4o);
    if (off < 0) return -1;

    uint8_t f5[64]; int f5o = 0;
    f5o = pb_write_uint(f5, sizeof(f5), f5o, 1, 1);
    f5o = pb_write_string(f5, sizeof(f5), f5o, 3, "OCapture");
    if (f5o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 5, f5, f5o);
    return off;
}

int ctrl_build_channel_announcement(uint8_t *out, size_t out_cap,
                                      int seq, int chan_idx) {
    /* Backward-compat wrapper - no custom params, keeps the original behaviour. */
    return ctrl_build_channel_announcement_ex(out, out_cap, seq, chan_idx, NULL);
}

/* (= The old implementation below is switched off - the work is delegated to
 *    ctrl_build_channel_announcement_ex.) */
#if 0
int ctrl_build_channel_announcement_legacy(uint8_t *out, size_t out_cap,
                                      int seq, int chan_idx) {
    if (!out || chan_idx < 0 || chan_idx >= 8) return -1;
    const chan_info_t *ci = &SHADOW_CHANNEL_INFO_BODIES[chan_idx];

    /* Request {f8 = ChannelInfo body} */
    uint8_t req[64];
    int ro = pb_write_submsg(req, sizeof(req), 0, 8, ci->body, ci->len);
    if (ro < 0) return -1;

    /* Top-level Message {f1=seq, f2=Request, f4=caps, f5=OCapture} */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, seq);
    off = pb_write_submsg(out, out_cap, off, 2, req, ro);
    if (off < 0) return -1;

    uint8_t f4[256]; int f4o = 0;
    f4o = pb_write_uint(f4, sizeof(f4), f4o, 1, 2);
    f4o = pb_write_string(f4, sizeof(f4), f4o, 2, "12.3.3");
    f4o = pb_write_string(f4, sizeof(f4), f4o, 3,
        "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (f4o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 4, f4, f4o);

    uint8_t f5[32]; int f5o = 0;
    f5o = pb_write_uint(f5, sizeof(f5), f5o, 1, 1);
    f5o = pb_write_string(f5, sizeof(f5), f5o, 3, "OCapture");
    if (f5o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 5, f5, f5o);
    return off;
}
#endif  /* legacy ancien builder */

int ctrl_build_register_session(uint8_t *out, size_t out_cap,
                                  uint32_t width, uint32_t height) {
    /* S3: full EDID by default (see the SHADOW_EDID_384B block). */
    static int g_edid_full = -1;
    if (g_edid_full < 0) {
        const char *e = getenv("SHADOW_EDID_FULL");
        g_edid_full = e ? atoi(e) : 1;
    }
    if (!out) return -1;

    /* display_info { field 1 (bytes) = EDID } */
    /* S3: sized for the full 384 B EDID (+3 B of tag/len).
     * The old [256] made the build silently fail - the message was then NOT
     * sent, and bootstrap stopped on "RegisterSession FAIL". */
    uint8_t display[512]; int do_off = 0;
    do_off = pb_write_bytes(display, sizeof(display), do_off, 1,
                             g_edid_full ? SHADOW_EDID_384B : SHADOW_EDID_128B,
                             g_edid_full ? sizeof(SHADOW_EDID_384B)
                                         : sizeof(SHADOW_EDID_128B));
    if (do_off < 0) return -1;

    /* resolution { f1=w, f2=h, f3=fixed32 0x43100726 (as seen) } */
    uint8_t res[16]; int ro = 0;
    ro = pb_write_uint(res, sizeof(res), ro, 1, width);
    ro = pb_write_uint(res, sizeof(res), ro, 2, height);
    /* field 3 fixed32 - captured bytes `26 07 10 43`. */
    if (ro + 5 > (int)sizeof(res)) return -1;
    res[ro++] = (3 << 3) | 5;  /* tag field 3 wire 5 */
    res[ro++] = 0x26; res[ro++] = 0x07; res[ro++] = 0x10; res[ro++] = 0x43;

    /* scale { field 3 fixed32 = 1.0 float = 0x3f800000 } */
    uint8_t scale[8]; int so = 0;
    if (so + 5 > (int)sizeof(scale)) return -1;
    scale[so++] = (3 << 3) | 5;
    scale[so++] = 0x00; scale[so++] = 0x00; scale[so++] = 0x80; scale[so++] = 0x3f;

    /* RegisterSession { f2=display, f3=resolution, f4=scale } */
    uint8_t reg[640]; int rgo = 0;
    rgo = pb_write_submsg(reg, sizeof(reg), rgo, 2, display, do_off);
    rgo = pb_write_submsg(reg, sizeof(reg), rgo, 3, res, ro);
    rgo = pb_write_submsg(reg, sizeof(reg), rgo, 4, scale, so);
    if (rgo < 0) return -1;

    /* Request { f10=RegisterSession } */
    uint8_t req[768];
    int reqo = pb_write_submsg(req, sizeof(req), 0, 10, reg, rgo);
    if (reqo < 0) return -1;

    /* Top-level Message { f1=4, f2=Request, f4=caps, f5=OCapture }
     * RE 2026-05-09: seq=4 (= AFTER the seq=3 heartbeat), not 3. Desktop order:
     * Capabilities -> Auth(1) -> Encryption(2) -> Heartbeat(3) -> Register(4). */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, 4);
    off = pb_write_submsg(out, out_cap, off, 2, req, reqo);
    if (off < 0) return -1;

    uint8_t f4[256]; int f4o = 0;
    f4o = pb_write_uint(f4, sizeof(f4), f4o, 1, 2);
    f4o = pb_write_string(f4, sizeof(f4), f4o, 2, "12.3.3");
    f4o = pb_write_string(f4, sizeof(f4), f4o, 3,
        "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (f4o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 4, f4, f4o);

    uint8_t f5[32]; int f5o = 0;
    f5o = pb_write_uint(f5, sizeof(f5), f5o, 1, 1);
    f5o = pb_write_string(f5, sizeof(f5), f5o, 3, "OCapture");
    if (f5o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 5, f5, f5o);
    return off;
}

int ctrl_build_encryption_request(uint8_t *out, size_t out_cap,
                                    const uint32_t *algos, size_t n_algos,
                                    uint8_t out_client_key[32]) {
    (void)algos; (void)n_algos;

    /* Byte-exact format from the desktop app (LD_PRELOAD capture 2026-05-08,
     * line 13935):
     *
     *   Message {
     *       field 1 (varint) = 2
     *       field 2 (sub) = Request {
     *           field 12 (sub) = Encryption {
     *               field 1 (varint) = 32   <- key size (chacha20)
     *               field 2 (varint) = 12   <- nonce_extra size
     *               field 3 (varint) = 16   <- AEAD tag size
     *               field 4 (bytes 32) = client_random / DH share
     *           }
     *       }
     *       field 4 (sub) = caps meta
     *       field 5 (sub) = OCapture
     *   } */

    /* Step 1: Encryption body */
    uint8_t enc_body[64]; int eo = 0;
    eo = pb_write_uint(enc_body, sizeof(enc_body), eo, 1, 32);
    eo = pb_write_uint(enc_body, sizeof(enc_body), eo, 2, 12);
    eo = pb_write_uint(enc_body, sizeof(enc_body), eo, 3, 16);
    /* === K11 2026-08-21 - THESE 32 BYTES ARE THE UPSTREAM KEY ===
     * The original comment described them as an unimportant "client_random /
     * DH share", and we threw them away. That is wrong: Shadow encrypts each
     * DIRECTION WITH ITS OWN KEY. Proof, from the 2026-08-21 capture:
     *   - video (:base+10) and cursor (:base+30), server->client, decrypt with
     *     the key from the Encryption REPLY;
     *   - the 42 B packet on :base+13, client->server, does not decrypt with
     *     that one, but decrypts perfectly with the key sent HERE, in the
     *     REQUEST. Payload obtained:
     *         04 00 05 00 00 00 00 00 00 00 00 00 00 00
     * So we hand it back to the caller, who uses it as the Tx key.
     * The draw moves from rand() (unseeded, predictable) to a proper source:
     * this is now a real key. */
    uint8_t rand32[32];
    if (shadow_random_bytes(rand32, sizeof(rand32)) != 0) {
        for (int i = 0; i < 32; i++) rand32[i] = (uint8_t)(rand() & 0xFF);
    }
    if (out_client_key) memcpy(out_client_key, rand32, 32);
    eo = pb_write_bytes(enc_body, sizeof(enc_body), eo, 4, rand32, 32);
    if (eo < 0) return -1;

    /* Step 2: Request {field 12 = Encryption} */
    uint8_t req[128];
    int ro = pb_write_submsg(req, sizeof(req), 0, 12, enc_body, eo);
    if (ro < 0) return -1;

    /* Step 3: top-level Message */
    int off = 0;
    off = pb_write_uint(out, out_cap, off, 1, 2);
    if (off < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 2, req, ro);
    if (off < 0) return -1;

    /* field 4 capabilities meta */
    uint8_t f4[256]; int f4o = 0;
    f4o = pb_write_uint(f4, sizeof(f4), f4o, 1, 2);
    f4o = pb_write_string(f4, sizeof(f4), f4o, 2, "12.3.3");
    f4o = pb_write_string(f4, sizeof(f4), f4o, 3,
        "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (f4o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 4, f4, f4o);
    if (off < 0) return -1;

    /* field 5 OCapture */
    uint8_t f5[32]; int f5o = 0;
    f5o = pb_write_uint(f5, sizeof(f5), f5o, 1, 1);
    f5o = pb_write_string(f5, sizeof(f5), f5o, 3, "OCapture");
    if (f5o < 0) return -1;
    off = pb_write_submsg(out, out_cap, off, 5, f5, f5o);
    return off;
}

/* --- Decoders ------------------------------------------------------------ */

/* A callback-driven walk (enc_reply_field_cb) used to live here: replaced by
 * the manual loop in ctrl_parse_encryption_reply, below. Deleted on 2026-08-25
 * - it had no caller AND carried wrong field numbers (it announced the key at
 * field 3, whereas the live parser and the byte-exact capture both read it at
 * field 4). Dead code that contradicts the live code is worse than no code. */


/* Byte-exact top-level wire format (LD_PRELOAD capture 2026-05-08):
 *
 *   Message {
 *       field 1 (varint) = 2
 *       field 3 (sub) = Reply {
 *           field 12 (sub) = Encryption {
 *               field 1 (varint) = 32   <- key size
 *               field 2 (varint) = 12   <- nonce extra size
 *               field 3 (varint) = 16   <- AEAD tag size
 *               field 4 (bytes 32) = SERVER_KEY, chacha20-poly1305, directly usable
 *           }
 *       }
 *       field 4 (sub) = caps
 *       field 5 (sub) = OCapture
 *   }
 */
bool ctrl_parse_encryption_reply(const uint8_t *buf, size_t len,
                                   shadow_encryption_reply *out) {
    if (!buf || !out) return false;
    memset(out, 0, sizeof(*out));

    int off = 0;
    while ((size_t)off < len) {
        uint32_t fn = 0, wt = 0;
        off = pb_read_tag(buf, len, off, &fn, &wt);
        if (off < 0) return false;

        if (fn == 3 && wt == PB_WIRE_LENDELIM) {  /* Reply (was field 2) */
            const uint8_t *reply_buf = NULL;
            size_t reply_len = 0;
            off = pb_read_lendelim(buf, len, off, &reply_buf, &reply_len);
            if (off < 0) return false;

            int ro = 0;
            while ((size_t)ro < reply_len) {
                uint32_t rfn = 0, rwt = 0;
                ro = pb_read_tag(reply_buf, reply_len, ro, &rfn, &rwt);
                if (ro < 0) return false;
                if (rfn == 12 && rwt == PB_WIRE_LENDELIM) {  /* Encryption (was field 3) */
                    const uint8_t *enc_buf = NULL;
                    size_t enc_len = 0;
                    ro = pb_read_lendelim(reply_buf, reply_len, ro, &enc_buf, &enc_len);
                    if (ro < 0) return false;

                    /* Parse the inner Encryption. field 1=key_size,
                     * 2=nonce_extra_size, 3=tag_size, 4=server_key bytes. */
                    int eo = 0;
                    while ((size_t)eo < enc_len) {
                        uint32_t efn = 0, ewt = 0;
                        eo = pb_read_tag(enc_buf, enc_len, eo, &efn, &ewt);
                        if (eo < 0) return false;
                        if (efn == 4 && ewt == PB_WIRE_LENDELIM) {
                            const uint8_t *key_buf = NULL;
                            size_t key_len = 0;
                            eo = pb_read_lendelim(enc_buf, enc_len, eo, &key_buf, &key_len);
                            if (eo < 0) return false;
                            if (key_len == 32) {
                                memcpy(out->key, key_buf, 32);
                                out->has_key = true;
                                out->chosen_algorithm = SHADOW_ALG_CHACHA20_POLY1305;
                            }
                        } else {
                            eo = pb_skip_field(enc_buf, enc_len, eo, ewt);
                            if (eo < 0) return false;
                        }
                    }
                    return out->has_key;
                } else {
                    ro = pb_skip_field(reply_buf, reply_len, ro, rwt);
                    if (ro < 0) return false;
                }
            }
            return false;
        } else {
            off = pb_skip_field(buf, len, off, wt);
            if (off < 0) return false;
        }
    }
    return false;
}

int ctrl_build_udp_register(uint8_t *out_buf, size_t out_cap,
                              const uint8_t hash[20]) {
    if (!out_buf || out_cap < 25 || !hash) return -1;
    /* Byte-exact format from the desktop app (capture 2026-05-08):
     *   [0x41 ('A')][0x01 (ver)][0x00 (res)][0x14 (len=20)][0x00 (res)][hash 20B]
     */
    out_buf[0] = 0x41;
    out_buf[1] = 0x01;
    out_buf[2] = 0x00;
    out_buf[3] = 0x14;
    out_buf[4] = 0x00;
    memcpy(out_buf + 5, hash, 20);
    return 25;
}

bool ctrl_parse_authentication_reply_v2(const uint8_t *buf, size_t len,
                                          shadow_auth_reply *out) {
    if (!buf || !out) return false;
    memset(out, 0, sizeof(*out));

    /* Walk Message -> field 3 (Reply) -> field 4 (AuthBody) -> field 2 (hash) */
    int off = 0;
    while ((size_t)off < len) {
        uint32_t fn = 0, wt = 0;
        off = pb_read_tag(buf, len, off, &fn, &wt);
        if (off < 0) return false;
        if (fn == 3 && wt == PB_WIRE_LENDELIM) {
            const uint8_t *reply_buf = NULL;
            size_t reply_len = 0;
            off = pb_read_lendelim(buf, len, off, &reply_buf, &reply_len);
            if (off < 0) return false;
            int ro = 0;
            while ((size_t)ro < reply_len) {
                uint32_t rfn = 0, rwt = 0;
                ro = pb_read_tag(reply_buf, reply_len, ro, &rfn, &rwt);
                if (ro < 0) return false;
                if (rfn == 4 && rwt == PB_WIRE_LENDELIM) {
                    const uint8_t *ab_buf = NULL;
                    size_t ab_len = 0;
                    ro = pb_read_lendelim(reply_buf, reply_len, ro, &ab_buf, &ab_len);
                    if (ro < 0) return false;
                    int abo = 0;
                    while ((size_t)abo < ab_len) {
                        uint32_t afn = 0, awt = 0;
                        abo = pb_read_tag(ab_buf, ab_len, abo, &afn, &awt);
                        if (abo < 0) return false;
                        if (afn == 2 && awt == PB_WIRE_LENDELIM) {
                            const uint8_t *h = NULL; size_t hl = 0;
                            abo = pb_read_lendelim(ab_buf, ab_len, abo, &h, &hl);
                            if (abo < 0) return false;
                            if (hl == 20) {
                                memcpy(out->hash, h, 20);
                                out->has_hash = true;
                                return true;
                            }
                        } else {
                            abo = pb_skip_field(ab_buf, ab_len, abo, awt);
                            if (abo < 0) return false;
                        }
                    }
                } else {
                    ro = pb_skip_field(reply_buf, reply_len, ro, rwt);
                    if (ro < 0) return false;
                }
            }
            return false;
        } else {
            off = pb_skip_field(buf, len, off, wt);
            if (off < 0) return false;
        }
    }
    return false;
}

bool ctrl_parse_authentication_reply(const uint8_t *buf, size_t len,
                                       bool *out_success) {
    /* Compatibility wrapper: it DELEGATES to version 2, exactly as
     * ctrl_msgs.h says it does.
     *
     * It used to reimplement its own walk, still on the old field numbers
     * (Reply at field 2, AuthBody at field 1) even though the server had moved
     * them to 3 and 4 - the "was field 2" comments in
     * ctrl_parse_encryption_reply date from that same move. It could therefore
     * NEVER succeed again on real traffic, and its only effect would have been
     * to report a permanent authentication failure to whoever trusted it. It
     * had no callers; the defect surfaced from a test (tests/test_ctrl_msgs.c)
     * on 2026-08-25. */
    shadow_auth_reply r;
    bool ok = ctrl_parse_authentication_reply_v2(buf, len, &r) && r.has_hash;
    if (out_success) *out_success = ok;
    return ok;
}
