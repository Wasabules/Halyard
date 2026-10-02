/* ctrl_session_int.h - context types shared inside streaming/.
 *
 * Extracted from ctrl_session.c on 2026-08-25, when video reassembly moved into
 * its own module and needed the session context. This is NOT a public
 * interface: nothing outside streaming/ includes it.
 */
#pragma once

#include "ctrl_session.h"
#include "ctrl_video_tcp.h"
#include "ctrl_input_tcp.h"
#include "ctrl_audio_dtls.h"
#include "ctrl_comchan.h"
#include "clip_tcp.h"
#include "encryption.h"
#include "sufp.h"
#include "audio_dedup.h"
#include "audio_loss.h"    /* AUD-DEDUP-3: audio frames lost in both copies */

/* Value of `session_ctx_t::magic` (see that field). */
#define SESSION_CTX_MAGIC 0x5344C7A1u


/* F18 NACK 2026-05-22 23h30: pending NACK queue. vid_reasm_flush detects
 * missing chunks and pushes them into this queue. The main loop drains it and
 * sends through udp_video. Rate-limited by timestamp. */
#define NACK_QUEUE_CAP 32
/* === V10 2026-08-28 - THE 32-ENTRY CAP TRUNCATED REAL LOSSES ===
 * The list held 32 indices. On a real burst, 62 chunks were missing: we asked
 * for 32 and gave up on the other 30 WITHOUT SAYING SO, so the picture stayed
 * holed even when the server answered perfectly. 128 covers a 512-chunk frame
 * that lost a quarter of itself; beyond that the frame is a write-off anyway
 * and an IDR (G26) is the right answer, not retransmission. */
#define NACK_MAX_INDICES 128
typedef struct {
    uint16_t count;           /* number of chunk_idx in indices[] (max NACK_MAX_INDICES) */
    uint8_t  indices_n;       /* alias historique, non utilise */
    uint16_t indices[NACK_MAX_INDICES];  /* chunk_idx list */
    uint8_t  subchan;         /* subchan (= frame_id rotating) */
    uint32_t frame_id;        /* contextual frame_id */
} nack_request_t;

typedef struct {
    const ctrl_session_params *p;
    ctrl_session_stats        *stats;
    shadow_cipher             *cipher;
    sufp_reasm                *sufp_cursor; /* cursor reassembler */
    uint8_t                   *plain_buf;  /* scratch decrypt output (= heap) */
    /* S36: the audio anti-duplicate window - SESSION state (audio_dedup.h).
     * It used to live in a `static`: sound only came through on the first
     * session. */
    /* === S37 2026-08-25 - CONTEXT SENTINEL ===
     *
     * The channels receive the session context as a `void *`, which accepts any
     * pointer without a word from the compiler. A function extraction let
     * `&ctx` through where `ctx` was already a pointer: the callback received
     * the address of a dead stack slot and crashed on the first dereference,
     * after having run correctly for a whole session.
     * This sentinel makes that mistake LOUD and immediate instead of fatal and
     * late. It costs one comparison per callback. */
    uint32_t                   magic;
    audio_dedup_t              aud_dedup;
    int                        aud_dup_verifies;  /* the AUD8 diagnostic budget, per session */
    /* W1 2026-09-11 - per-session caps and state of the :base+30 audio path.
     * `ctx = {0}` resets them with the session: never a function static. */
    int                        aud_desc_logged;   /* DEC-1: the [AUD2] descriptor line, 1 per session */
    int                        aud_resync_logged; /* ING-A1: [ING-A1] resync lines, 8 per session */
    int                        aud_diag_jump;     /* ING-A1 diag: 0 idle, 1 armed, 2 done */
    /* AUD-DEDUP-3 2026-09-11 - the audio loss accountant (audio_loss.h), fed by
     * aud_accept() with the numbers the window ACCEPTED, and what the [AUD17]
     * lines need. Per session through `ctx = {0}`, like the window it follows. */
    audio_loss_t               aud_loss;
    int64_t                    aud_last_accept_ms; /* arrival of the last accepted frame, 0 = none yet */
    int64_t                    aud_gap_max_ms;     /* longest arrival gap since the last D4 onset */
    uint32_t                   aud_d4_missing0;    /* lost + pending at the last D4 onset */
    int                        aud17_logged;       /* [AUD17] trou lines: 40 per session */
    ctrl_video_tcp_t          *vst;
    /* K15k: the VIDEO channel over TCP (`:base+10`), when
     * `SHADOW_VIDEO_NET_TCP=1`. Distinct from `vst`, which serves the cursor on
     * `:base+20` - same client, two instances, two ports. */
    ctrl_video_tcp_t          *vtcp;        /* VideoSslTcpChannel :base+20 (V13) */
    ctrl_input_tcp_t          *itc;        /* InputSslTcpFlatBuffersChannel :base+14 (I1 2026-05-18) */
    /* CLIP3 2026-10-02 - the clipboard channel on :base+14, TCP+TLS+STFP.
     * Distinct from `comchan`, which opens the same port and (since S52)
     * transfers nothing: see KB §3.37. */
    clip_tcp_t                *clip;
    ctrl_comchan_t            *comchan;    /* ComChan :base+14 lifecycle (V15 2026-05-16, TIER 8 U4) */
    ctrl_audio_dtls_t         *aud;        /* AudioUdpChannel :base+12 DTLS+Opus (I2 2026-05-18) */
    /* K16d: the cross-column alarm "frames are arriving, no sequence parameter
     * set" is raised only
     * ONCE per session. The flag lives here and not in a function `static`: that
     * would be the defect family this repo documents everywhere, and it would be
     * particularly ironic inside the detector meant to watch for it - an alarm
     * that goes silent from the 2nd session on. */
    int                       sps_alarm_said;

    /* F18: pending NACK queue */
    nack_request_t            nack_queue[NACK_QUEUE_CAP];
    int                       nack_head, nack_tail;  /* ring buffer indices */

    /* === SRV5 2026-10-02 - THE GRANTED STREAM HANDLES, KEPT ===================
     *
     * The server answers each of the eight announcements with a SessionInfo that
     * carries the handle it granted for that stream (`ann_reply.h`, f2). We were
     * parsing it, logging it and dropping it. It is the stream identifier the
     * unregister request (oneof field 9) needs, and therefore the only way to
     * re-announce ONE channel mid-session - the single recovery the server's own
     * code allows for a stream client it has invalidated
     * (halyard-lab/notes/findings/server-vs-halyard.md §2).
     *
     * Indexed by SHADOW_CHAN_IDX_* (ctrl_msgs.h), per session through
     * `ctx = {0}` - never a function static. */
    uint64_t                  chan_handle[8];
    int                       chan_handle_ok[8];
} session_ctx_t;
