/* ctrl_session - high-level orchestrator for Shadow native streaming.
 *
 * Full loop:
 *   1. Direct TCP+TLS to vm_host:13011 (NO ALPN, NO SNI)
 *   2. Capabilities -> Authentication (extracts the server's 20 B hash)
 *   3. Encryption -> extracts the 32 B chacha20 key
 *   4. RegisterSession + 8 channel announcements
 *   5. UDP register :13010 (video) + :13030 (cursor) + :13013 (input) with the 20 B hash
 *   6. UDP receive loop: chacha20 decrypt + VideoFrame parse + H.264 reassembly
 *   7. Callbacks: on_video_frame(NAL bytes, ts, ...), on_cursor, on_audio
 *
 * Nothing is hardcoded, except the channel announcement bodies - those are
 * byte-exact captures of the official desktop app, because the protocol is
 * proprietary; they are documented in ctrl_msgs.c.
 *
 * Caller pre-conditions:
 *   - oauth flow done (refresh_token -> access_token)
 *   - launcher_start_vm + launcher_get_vm_ip + launcher_proximus_credentials
 *   - proximus_create_launcher_client + proximus_create_main_client
 *   - proximus_sse_start (DUAL: launcher_jwt + main_jwt) - this is what makes
 *     the server bind :13011
 *
 * The caller passes vm_host (from VmConnectionInfo.ip), streaming_token
 * (msess.streaming_token), client_id (msess.id), main_jwt (creds.main_jwt).
 * See the examples in connecting_activity.cpp / main_test.c.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An H.264 NAL bytestream frame, ready to feed to libavcodec.
 * `nal_data` = one or more NAL units with `00 00 00 01` start codes.
 * The caller must copy the bytes during the callback (they may vanish after).
 * `pts_ms` = timestamp in milliseconds (derived from the Shadow timestamp / 90).
 * `is_keyframe` = true when the frame carries SPS/PPS/IDR. */
typedef void (*ctrl_session_video_cb)(const uint8_t *nal_data, size_t len,
                                        uint64_t pts_ms, bool is_keyframe,
                                        void *user);

/* An Opus audio packet (already decrypted).
 * `payload` = raw Opus frame (not RTP-encapsulated).
 * I3 2026-05-18: signature aligned with audio_decoder::audio_decoder_feed.
 * Shadow audio arrives raw over DTLS (not RTP-wrapped), hence rtp_ts=0. */
typedef void (*ctrl_session_audio_cb)(const uint8_t *payload, size_t len,
                                        uint32_t rtp_ts, void *user);

/* Cursor frame (chacha20-decrypted, Shadow-specific format TBD).
 * For now we just log the raw bytes - decoding the content (cursor bitmap /
 * x,y / shape) will come in a second pass. */
typedef void (*ctrl_session_cursor_cb)(const uint8_t *payload, size_t len,
                                         void *user);

/* Progress / log callback - called all along the bootstrap to report where we
 * are. step = "M8.connect", "M9.auth", "M10.encrypt", "M11.streaming", etc.
 * Also called for errors, with step = "error". */
typedef void (*ctrl_session_progress_cb)(const char *step, const char *detail,
                                            void *user);

/* Input parameters. Every string must stay valid for the whole duration of the
 * ctrl_session_run() call (which blocks). */
typedef struct {
    /* Connection - from launcher_get_vm_ip + proximus_credentials */
    const char *vm_host;          /* ipv6-gpu-XXX.frsbg01.compute.shadow.tech */
    const char *streaming_token;  /* msess.streaming_token (34 chars typically) */
    const char *client_id;        /* msess.id ("<user-id-B>-XXX-main") */
    const char *bearer_jwt;       /* creds.main_jwt (= the JWT for SSE + REST) */

    /* Display config - announced via RegisterSession.resolution */
    int display_width;            /* default 1920 */
    int display_height;           /* default 1080 */

    /* Q1 2026-05-18 - quality params announced on the video channel (CI_BODY_5).
     * 0 or unset = use the hardcoded desktop defaults (=1920x1080 @ 144fps @ 20Mbps).
     * Anything else overrides them. Also readable from the env vars
     * SHADOW_BITRATE_MBPS / SHADOW_FPS. */
    uint32_t max_bitrate_mbps;    /* default 0 = use the hardcoded 20 Mbps */
    float    target_fps;          /* default 0 = use the hardcoded 143.85 */

    /* B4 - video transport. 0 = UDP (the default and the measured one),
     * 1 = TCP, which is the official client's `reliability` profile: the request
     * (announcement field f3) and the receive side (STFP framing on the same
     * port, in TLS) are ONE decision - asking without receiving gives a session
     * with no picture. `SHADOW_VIDEO_NET_TCP` still wins when it is set. */
    int      video_tcp;

    /* port_base is determined by the caller (= vm.port + 7000 per observation).
     * If 0 -> ctrl_session fails cleanly with progress "M8.fail".
     * The caller must read `conn.port` from the /vm/ip response and compute it. */
    int port_base;

    /* Callbacks */
    ctrl_session_video_cb     on_video;
    ctrl_session_audio_cb     on_audio;
    ctrl_session_cursor_cb    on_cursor;
    ctrl_session_progress_cb  on_progress;
    void                      *user;

    /* abort_flag: set to 1 by the caller to leave the loop cleanly.
     * Polled by ctrl_session_run at ~100 ms granularity. */
    volatile int *abort_flag;
} ctrl_session_params;

typedef struct {
    bool     bootstrap_ok;
    uint32_t udp_video_pkts;
    uint64_t udp_video_bytes;
    uint32_t udp_cursor_pkts;
    uint32_t frames_decoded;
    uint32_t decrypt_ok;
    uint32_t decrypt_fail;
    /* H1 patches (cf. tools/ida/out/H1_SUFP_analysis.md + H1_SUFP_deep_v2.md) */
    uint32_t parity_skip;        /* F.1: flag10==0 chunks dropped (= FEC parity, not data) */
    /* ING-A4 2026-09-11: udp_audio_pkts counts every audio frame, Opus or FLAC,
     * that reaches the anti-duplicate window, duplicates included, and
     * audio_dup_skipped the ones it refuses - so the difference is the frames
     * played. Two of the three paths used to count AFTER the window: with split
     * FLAC frames in the stream the difference under-read by ~24 %. */
    uint32_t udp_audio_pkts;     /* AUD2/ING-A4: audio frames at the window, duplicates included */
    uint32_t audio_dup_skipped;  /* AUD8: duplicates rejected by the window */
    uint64_t udp_audio_bytes;    /* useful audio bytes, Opus or FLAC (after deduplication) */
    /* AUD-DEDUP-3 2026-09-11 - audio frames lost in BOTH copies, counted from
     * the numbers the window accepted (audio_loss.h), and the refusals that were
     * too old rather than duplicates. audio_dup_skipped keeps its meaning:
     * every refusal, stale ones included. */
    uint32_t audio_frames_lost;  /* FINAL: never arrived, 64 frames later */
    uint32_t audio_holes;        /* forward gaps in the numbering */
    uint32_t audio_hole_max;     /* longest gap, in frames */
    uint32_t audio_renum;        /* renumberings followed */
    uint32_t audio_stale;        /* refused as too old (64+ behind): the S36 class, made visible */
    uint32_t parity_decrypt_ok;  /* RE8: parity chunks that decrypted OK (= data in disguise) */
    uint32_t parity_decrypt_fail;/* RE8 : chunks parity dont tag poly1305 fail (= vraie FEC ou key diff) */
    /* === V12 2026-08-28 - FIVE COUNTERS REMOVED, NEVER WIRED UP ===
     * `chunks_late`, `chunks_backward`, `seq_window_drop`,
     * `chunks_tail_recovered` and `reasm_size_change` were declared, copied
     * into the glue layer, printed in the statistics line AND in the
     * measurement JSON - and incremented NOWHERE. Commit 19229d0 (2026-08-25)
     * deleted their increments along with the old reassembly machinery and left
     * the fields behind.
     *
     * Their zero READ like a measurement: my V10 analysis of 2026-08-28 quoted
     * "chunks_late and chunks_tail_recovered at zero" as proof that
     * retransmissions arrived in time. That sentence was worth nothing.
     *
     * They are REMOVED rather than wired: three of them no longer have any
     * referent in the code, and the other two only lived in the log line.
     * Inventing an increment site for a notion the code no longer produces
     * would yield a meaningless number - the same defect, only worse. If a
     * concept comes back, its counter comes back with it.
     *
     * A counter that is displayed is not a counter that is written. */
    uint32_t chunks_missing;     /* morceaux absents au flush */
    uint32_t frames_miss1;       /* images auxquelles UN SEUL morceau manquait */
    uint32_t chunks_orphan;      /* G9c: stragglers with no picture to join */
    uint32_t chunks_orphan_dup;  /* ... dont exemplaires en double (inoffensifs) */
    uint32_t chunks_orphan_lost; /* ... of which data never received (real loss) */
    uint32_t chunks_orphan_stale;/* ... of which a chunk of ANOTHER picture (different max) */
    uint32_t frames_duplicate;   /* G10: pictures identical to the previous one */
    uint32_t frames_dropped_trunc; /* G16 : images tronquees NON emises */
    uint32_t nack_sent;          /* retransmissions reellement demandees */
    uint32_t chunks_redundant;   /* V10b: chunks received for a slot already filled
                                  * = network duplicate OR a reply to our `rG`. The only
                                  * direct proof that a retransmission does arrive. */
    uint32_t chunks_expected;    /* chunks expected, for the rate */
    uint32_t reasm_abandoned;    /* F.2: frames abandoned (= SoF received before the previous one completed) */
    uint32_t incomplete_at_flush;/* G.1: flushes triggered by the last-chunk fast path, with holes */
    uint32_t got_last_chunk;     /* G.4: count of chunks received with chunk_idx == max_chunks-1 (= last chunk) */
    /* H1 V8 - NAL types seen before feeding the decoder. Lets us check that the bottom
     * NAL slices really do reach the pipeline. nal_bottom == 0 -> the bug is upstream.
     * nal_bottom > 0 but a partial picture -> the bug is in the decoder.
     * Desktop reference: 89% top + 11% bottom. */
    uint32_t nal_top;            /* NAL type 1/5 with first_mb=0 (= top slice) */
    uint32_t nal_bottom;         /* NAL type 1/5 with first_mb!=0 (= bottom slice) */
    uint32_t nal_idr_top;        /* NAL type 5 (IDR) with first_mb=0 */
    uint32_t nal_idr_bottom;     /* NAL type 5 (IDR) with first_mb!=0 */
    uint32_t nal_sps;            /* NAL type 7 */
    uint32_t nal_pps;            /* NAL type 8 */
    int      session_seconds;
    int      exit_reason;        /* 0=normal, 1=bootstrap_fail, 2=server_kicked, 3=abort */
} ctrl_session_stats;

/* Runs the streaming session (blocking). Returns true if the stream got as far
 * as receiving at least one video frame (= bootstrap fully OK).
 * `out_stats` is filled in even on failure (for debugging). */
/* UDP registration on the input channel `:base+13`. Pushed from the UI: this
 * module is C and also links into the headless binary, which does not carry the
 * C++ layer. See Settings::udp_register_input. */
void ctrl_session_set_udp_register_input(int on);

bool ctrl_session_run(const ctrl_session_params *params,
                       ctrl_session_stats *out_stats);

/* === B1 2026-09-02 - THE LINKAGE GUARD CLOSED HERE, IN THE MIDDLE ===
 *
 * Everything below used to sit OUTSIDE it: eight functions of the public API
 * declared with C++ linkage in every C++ file that included this header. The
 * two screens that call them worked only because each had wrapped the include
 * in its OWN `extern "C" { }` - so the header's contract depended on every call
 * site remembering to repair it, and the first one that forgot got an undefined
 * reference at LINK time, far from the cause. That is exactly the defect
 * `shadow/http.h` documents: "the guard belongs to the header".
 *
 * The block now closes at the end of the file. */

/* D1 - picture refresh request (developer menu bar).
 * This is what the official client sends when its window is resized. */
void ctrl_session_request_refresh(void);

/* Applies a bitrate cap to the running session without restarting it - the way
 * the official client does. It takes effect on the next iteration of the
 * session loop; with no active session, the call does nothing. */
void ctrl_session_set_bitrate(uint32_t mbps);

/* Applies a bitrate to the running session without restarting it.
 * CFG-4 2026-09-11: it used to take a frame rate too, and ignored it - the live
 * message (kUpdateSession f13) has carried no frame-rate field since S18. The
 * frame rate, like the resolution (KB §3.21), travels only in the channel
 * announcement and waits for the next connection. The parameter is gone so
 * that the compiler finds every caller that still believes otherwise.
 *
 * Three values, and the third is the one B1 was missing:
 *   0                          leave this field unchanged
 *   SHADOW_VIDEO_CFG_DEFAULT   go back to the client's default value
 *   anything else              that value
 *
 * WITHOUT THE SENTINEL, "Auto" COULD NOT BE APPLIED. It resolves to a real
 * number on the wire (the announcement always carries one), but the UI passed
 * 0 for it - which this function read as "unchanged". Choosing Auto mid-session
 * therefore left the previous value in force while the screen said Auto, and
 * nothing in the log said otherwise. A caller that resolved Auto itself would
 * have worked too, and would have put the client's default in a fourth place;
 * this way it stays in exactly one (`bitrate.h`). */
#define SHADOW_VIDEO_CFG_DEFAULT 0xFFFFFFFFu
void ctrl_session_set_video_config(uint32_t mbps);

/* Asks the server for a key frame. Only call this on a reliable signal: the
 * decoder no longer producing pictures. Asking too often degrades the image - a
 * key frame costs several times an ordinary frame, and the encoder compensates
 * by lowering quality (see G12). */
void ctrl_session_request_idr(void);

/* True as long as a streaming session is running. */
bool ctrl_session_active(void);

#ifdef __cplusplus
}
#endif
