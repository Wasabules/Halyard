/* vid_reasm — see vid_reasm.h. Extracted from ctrl_session.c on 2026-08-25. */

#include <errno.h>
#include "ctrl_session.h"
#include "ctrl_tcp.h"
#include "ctrl_video_tcp.h"
#include "ctrl_input_tcp.h"  /* I1 2026-05-18 */
#include "ctrl_audio_dtls.h" /* I2 2026-05-18 */
#include "ctrl_comchan.h"   /* V15 2026-05-16 — :base+14 lifecycle bus + focus events */
#include "ctrl_gamepad.h"   /* gamepad on :base+13 (KB §3.25) */
#include "native_input.h"    /* I1 phase 3 2026-05-18 */
#include "ctrl_msgs.h"
#include "encryption.h"
#include "sufp.h"
#include "../services/jwt.h"   /* LIB2: jwt_instance, no longer in smoke_test */
#include "../services/log.h"
#include "../services/stats.h"
#include "../services/sockets_compat.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ctrl_session_int.h"
#include "vid_reasm.h"
#include "vid_wire.h"
#include "latency.h"   /* L5: instrumentation of the video path */

/* S81 — the category is DECLARED here, not inferred from the message text.
 * `clog` stays at INFO: the existing calls do not disappear. `cdbg` is there for
 * the bulky lines, which move over to it one at a time. */
#define clog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
#define cdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
/* === Video chunk reassembly — SUFP v3 header, 11 bytes ===
 *
 *   byte 0     : version:4 | type:4   (0x13 or 0x23 observed)
 *   byte 1     : subchannel
 *   bytes 2-3  : chunk_idx (u16 LE)   **0-based**
 *   bytes 4-5  : max      (u16 LE)    **LAST INDEX, not a count**: the server
 *                                     sends 0..max, hence max+1 chunks (G4,
 *                                     confirmed by G21)
 *   bytes 6-9  : frame_id (u32 LE)
 *   byte 10    : flags                bit 0 = SELF-CONTAINED chunk, encrypted
 *                                     `[ct][nonce 12][tag 16]`. Zero = a
 *                                     PLAINTEXT continuation, to be concatenated
 *                                     as-is — and not a parity: there is no FEC
 *                                     here at all (F31, KB §3.15).
 *   bytes 11+  : payload
 *
 * Reassembly: per subchannel, chunks are filed by index then concatenated in
 * ascending order, the Shadow `VideoFrame` header (`[0x02][ts u32][flags]`) is
 * stripped, and Annex-B is handed to the decoder.
 *
 * WARNING. This header long described a SIX-byte format with no frame_id and a
 * 1-based index, calling the 11-byte format "obsolete". It was the exact
 * opposite: the 11-byte format is the one on the wire, and the index starts at
 * zero. Corrected on 2026-08-25.
 *
 * VOCABULARY — a trap. This whole file calls the `byte10 == 0` chunks "parity".
 * THAT IS A HISTORICAL NAME AND IT IS WRONG: they are not parity, there is no
 * error-correcting code in this protocol (F31). They are plaintext
 * CONTINUATIONS, and the last one carries the tail of the bottom slice —
 * precisely the chunk G4 was dropping. The name survives because it is baked
 * into documented toggles (`SHADOW_PARITY_RAW`, `_TRIM`, `_DECRYPT`) and into
 * statistics fields the UI reads; renaming it would break public names for a
 * gain in vocabulary. So read "parity" as "continuation", everywhere.
 *
 * The three rules that each cost an RE campaign — `max` is the last index, an
 * access unit starts at a `first_mb == 0` slice, and a picture missing only its
 * tail is emitted truncated rather than dropped — live in vid_wire.c, where they
 * are TESTED with their counter-cases (tests/test_vid_wire.c). This file applies
 * them, it does not redefine them. */

#define VID_HDR_LEN 11   /* sufp v3 header: type+sub+chunk_idx+max_chunks+4B+flag */
/* 2026-05-18: raised 256->512 for the large 1080p bottom slices.
 * A 1080p bottom IDR can reach 200-400 KB and need 200-300+ chunks. At 256 we
 * dropped the chunks at idx >= 256 -> truncated bottom slice -> blurry taskbar
 * (= MB row 60-67 partial). 512 leaves headroom for the heavy key frames. */
#define VID_MAX_CHUNKS 512
#define VID_MAX_SUBCHANS 256  /* ver 2 uses wide subchans (up to 0xbb observed) */

typedef struct {
    uint8_t *chunks[VID_MAX_CHUNKS];
    size_t   chunk_lens[VID_MAX_CHUNKS];
    bool     slot_seen[VID_MAX_CHUNKS]; /* H1 V5: marked for EVERY chunk (data or
                                            parity), so recv_count can reach
                                            max_chunks even with a parity at the
                                            "last" idx -> fast path G.1 fires. */
    bool     chunk_is_data[VID_MAX_CHUNKS]; /* RE6 2026-05-22: track data vs parity per
                                                slot, for the SHADOW_PARITY_AS_NAL
                                                experiment + diagnostics. */
    uint16_t max_chunks;
    uint16_t recv_count;     /* count of slot_seen[] true (data + parity) */
    uint16_t data_count;     /* H1 V5: count of slots actually holding data */
    uint16_t highest_idx;
    bool     active;
    bool     expect_sof;     /* H1 V2 G.3: set after an abandon; until the next SoF we
                                drop every non-SoF chunk, to avoid orphan chunks from
                                the frame we have just dropped. */
    uint32_t nack_frame_id;  /* V10: the picture a request has already gone out for,
                                so we do not ask twice (detection, then flush). */
    bool     nack_done;
    uint32_t cur_frame_id;   /* N56 2026-05-18: track the frame_id of the reassembly in
                                progress. Detecting the switch via the frame_id change
                                (= wire bytes 6-9 LE) is more robust than
                                `is_data && chunk_idx==0`, which misses the cases where
                                chunk 0 is sent as parity or lost to UDP reordering.
                                Without this tracking we accumulated the new frame's
                                chunks in the old frame's buffer -> a flush with
                                first_hole_idx=0 every time. */
    int64_t  first_seen_ms;  /* G24 2026-08-22: instant of the 1st chunk (reorder buffer). */
    /* L5 2026-08-29 — same instant, in MICROSECONDS. A picture's burst lasts a
     * few milliseconds: measured in ms it would return 0, 1 or 2, which allows
     * neither an average nor a percentile. A separate field rather than a
     * conversion of `first_seen_ms`: that one serves the reorder buffer (G24)
     * and its unit is read elsewhere. */
    int64_t  first_seen_us;
    /* === V11 2026-08-28 — MEMORY OF THE LAST PICTURE EMITTED ON THIS SUBCHANNEL ===
     * It deliberately SURVIVES vid_reasm_clear(): it is the only thing that can
     * tell whether a chunk arriving with no picture to join is the second copy
     * of the tail (the server resends it ~3 ms later) or data from a picture
     * whose opening was lost. State that outlives its object = this repo's
     * defect family: so it is zeroed by vid_reasm_reset_session(), and its age
     * is BOUNDED (recency window), never assumed fresh. */
    uint16_t emitted_max;      /* chunk count of the last picture emitted */
    bool     emitted_complete; /* it was complete (so all its indices were seen) */
    uint32_t emitted_ord;      /* global flush number at that instant */
    /* REASM-1 2026-09-11 - opening number (g_pic_ord) of the picture in this
     * buffer, set at its opening chunk. Read only while `active`: it is what
     * tells a predecessor still being received from a previous lap's leftover. */
    uint32_t open_pic;
} vid_reasm_t;

static vid_reasm_t g_vid_reasm[VID_MAX_SUBCHANS];

/* V11: sequence number of the current flush. Its only purpose is to measure the
 * AGE of the memory above, in pictures rather than milliseconds — the subchannel
 * is reused every 256 pictures, which is the only scale that matters here.
 * Wrapping is harmless: the comparison is an unsigned subtraction over a window
 * of 4. */
static uint32_t g_flush_ord = 0;

/* REASM-1 2026-09-11 - sequence number of the current picture OPENING: chunk 0
 * of a multi-chunk picture, or a single-chunk picture. The G43 trigger's age
 * guard reads it (see vid_flush_prev_incomplete). Counted in openings rather
 * than milliseconds, like g_flush_ord - and because vid_now_ms() is the real
 * clock, which no offline test can drive. Session state: zeroed by
 * vid_reasm_reset_session(), like the two [G43] line budgets below. */
static uint32_t g_pic_ord         = 0;
static uint32_t g_g43_single_logs = 0;   /* "[G43] image precedente videe..." lines, this session */
static uint32_t g_g43_lost_logs   = 0;   /* "[G43] picture never emitted..." lines, this session */

/* Picture waiting for its tail (G9). One is enough: the straggler arrives within
 * the following picture, never later. */

/* === G24 2026-08-22 — REORDER BUFFER (jitter buffer), modelled on the official
 * client (RE of SufpUdpIOChannel/udp_packet_receiver). The official client keeps
 * pictures in a list ordered by subchan, WAITS ~300 ms for late chunks (network
 * jitter/reordering) before declaring a picture lost, and emits STRICTLY IN
 * sequence ORDER — complete pictures only. On real loss (picture still
 * incomplete after the delay) it drops + requests an IDR (no NACK, no FEC).
 * Before G24 we emitted as soon as a picture was complete (i.e. in COMPLETION
 * order = out of order under jitter) and dropped a picture as soon as one chunk
 * was ~66 ms late -> blocks of artefacts. SHADOW_REORDER_BUFFER=0 restores the
 * old behaviour; SHADOW_REORDER_MS sets the delay (default 300, like the
 * official client). */
int      g_emit_next_sub = -1;   /* next subchan to emit (-1 = uninitialised) */

/* === G36 2026-08-22 — REQUEST AN IDR ON A SEQUENCE GAP (bounds the drift) ===
 * Emission at completion is IN ORDER (measured: subchans 0,1,2… consecutive) BUT
 * ~1 % of pictures never complete (tail lost) and are skipped. The following
 * P-frames reference them -> decoder drift (a fog) until the next key frame.
 * Since the server sends very few spontaneously (idr_t=2), the drift
 * accumulates. We detect the gap (emitted subchan != previous+1) and request an
 * IDR: the drift is then bounded to ~one recovery, without freezing the picture
 * (unlike the reorder buffer). SHADOW_IDR_ON_GAP=0 disables it. */
int      g_emit_last_sub = -1;
/* CONC-4 2026-09-11 - [G42] subchannel gaps, this session: the line budget
 * lived in a function static, spent by the first session of a process. */
static uint32_t g_sub_gaps = 0;
int64_t  g_head_since_ms = 0;    /* instant this subchan became the head */

int64_t vid_now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int vid_reorder_enabled(void) {
    /* G24 default 0 for now: the reorder buffer is correct (modelled on the
     * official client) BUT our SYNCHRONOUS decode on the receive thread creates
     * processing bursts that expire the head wrongly (~1 %), each one = a ~300 ms
     * freeze. Making it the default first requires decoding on a SEPARATE THREAD
     * (the RE's 2nd recommendation) so reception runs in real time. Until then:
     * opt-in via SHADOW_REORDER_BUFFER=1. */
    /* G35: default ON. Emission IN subchan ORDER, and on a late tail we emit
     * truncated (never skip) -> no more global decoder drift.
     * SHADOW_REORDER_BUFFER=0 returns to emission at completion. */
    static int g = -1;
    if (g < 0) { const char *e = getenv("SHADOW_REORDER_BUFFER"); g = e ? atoi(e) : 0; }
    return g;
}

/* V11: classify the orphans. Default YES — this is pure measurement, it changes
 * NO decision to keep or drop. At 0 you get exactly the previous statistics
 * line: `orph` still counts all three causes together, and the sub-counters stay
 * at zero. The toggle restores the old path, it does not disable the new one. */
static int vid_orphan_classify(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("SHADOW_ORPHAN_CLASSIFY"); on = e ? atoi(e) : 1; }
    return on;
}

/* Reassembly debugging logs (arrival sequence, picture opening, contents of an
 * incomplete flush). They served to find G7/G9; kept behind SHADOW_DIAG_REASM=1
 * because the next anomaly of that kind will be diagnosed with them, and because
 * they are capped. */
static int shadow_diag_reasm(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SHADOW_DIAG_REASM");
        on = (e && atoi(e) == 1) ? 1 : 0;
    }
    return on;
}

/* Fingerprint of the last picture emitted: a straggler whose index is already in
 * it is a DUPLICATE, harmless. If it is not in it, it is data we are dropping —
 * and for a last chunk, it is the end of the bottom slice, hence artefacts on
 * motion. Aggregate counters cannot tell the two apart; this fingerprint can. */
static bool     g_last_slots[VID_MAX_CHUNKS];
static uint16_t g_last_max = 0;
static bool     g_last_valid = false;

/* Last truncated picture: used to tell reordering from real loss. */
static uint32_t g_flushed_fid = 0;

/* === AUD/G9 — MEASURING CONGESTION ===
 *
 * Bytes 6-9 of the video header are the SEND timestamp in microseconds (verified
 * on the capture: 60 distinct values over 60 packets, increasing at the rate of
 * emission). Subtracting it from our receive clock gives an offset whose
 * absolute value means nothing — the clocks differ — but whose VARIATION says
 * everything: it grows when packets pile up in a queue, i.e. when the link
 * saturates.
 *
 * That is what field 2 of the `gE` message carries in the official client (small
 * signed values: +280, -1, +13, +23, -45). We always sent zero there, which
 * amounts to telling the server "all is well" whatever happens — it then has no
 * reason to slow down. */
int64_t g_delay_offset_us = 0;   /* last offset observed */
/* V9 — MEASURING the size of the defect that was fixed: the number of packets
 * arriving on `:base+10` long enough to pass the old `n >= 10` guard but
 * rejected by the header validation (n <= 39). Every one of those poisoned the
 * congestion feedback clock. Reset per session: see vid_reasm_reset_all. */
uint32_t g_vid_short_pkts = 0;
uint32_t g_vid_total_pkts = 0;
int     g_delay_valid     = 0;

/* G6 2026-06-02 — event-driven IDR: set to 1 when an incomplete frame is
 * detected at flush time (= UDP loss). The main loop then sends ONE IFR (key
 * frame request), rate-limited, to recover WITHOUT forcing a periodic IDR (=
 * without G5's autofocus). A clean stream = no loss = no IFR = stable. */
volatile int g_idr_needed = 0;   /* G6: declared up here for G13 */

/* Last frame_id seen on UDP video — bytes 6-9 of the last chunk received.
 * Echoed back in gE feedback packets to keep the flow going. */
volatile uint32_t g_last_frame_id = 0;

/* DISPLAY-LEVEL buffer: multi-subchan multi-slice cross reassembly.
 * RE 2026-05-09: each subchan = one spatial slice of the same picture. They
 * share the SAME timestamp in the plaintext header (= bytes 1-4).
 * Reassembly: accumulate each flush's Annex-B NALs into g_display_buf, then emit
 * to the decoder when a NEW timestamp is detected. */
#define VID_DISPLAY_BUF_SIZE (8 * 1024 * 1024)
static uint8_t  g_display_buf[VID_DISPLAY_BUF_SIZE];
static size_t   g_display_off = 0;
/* === L7 2026-08-29 — THE MISSING MILESTONE ===
 *
 * `latency.h` has said it since its first line: the costliest link of the video
 * path — the finished picture waiting in `g_display_buf` — is the ONLY one
 * nothing measures. L5 instrumented nine stages and still did not cover it:
 * `video/file-aff` runs from "picture PUSHED" to "picture drawn", i.e. AFTER
 * this wait. The two instruments bracket it without ever touching it.
 *
 * What we keep here is the instant of the LAST byte added to the picture in
 * progress. The difference with the flush gives the exact wait, because the
 * flush is only triggered by the arrival of the NEXT picture: structurally, this
 * item is therefore worth a whole inter-picture interval — ~21 ms at 46 fps,
 * which would make it the largest of the entire console chain. That is a
 * PREDICTION, and it has exactly the shape of those this repo has already
 * watched fall; hence this measurement before any fix.
 *
 * Cost: one clock read per NAL append, only when the measurement is on. */
static int64_t  g_display_last_us = 0;

/* === L10 2026-08-29 — KNOWING A PICTURE IS FINISHED, ON UDP TOO ===
 *
 * L8 removed the wait on the TCP path, where the framing says where the picture
 * ends. On UDP nothing says it: `emit_legacy` only flushes when the FIRST slice
 * of the next picture arrives. L7 quantifies what that costs — p50 = 25.6 ms at
 * 46 fps on desktop, i.e. a whole inter-picture interval, and three quarters of
 * the video chain.
 *
 * What can be known without the stream saying it: HOW MANY slices make up a
 * picture. The server is regular — 1 in HEVC 720p, 2 in H.264 1080p (top=1279
 * bottom=1279, measured). The slices occupy CONSECUTIVE subchannels, drained in
 * order by `vid_drain_ordered`. Once that number has been observed stable, the
 * last slice of a picture is recognisable the moment it arrives.
 *
 * === WHAT MAKES THIS BET ACCEPTABLE: IT CHECKS ITSELF ===
 *
 * Getting it wrong here means cutting an access unit in two and reopening G40 —
 * the blur under heavy motion, invisible on a static desktop, that cost a whole
 * campaign. An unverifiable bet would therefore be unacceptable.
 *
 * But it does check itself: after an early flush, the next slice MUST open a new
 * picture. If it does not, it belonged to the one we have just flushed, and we
 * know it at that very instant. The mechanism then switches itself off
 * DEFINITIVELY for the session and logs it. The cost of a mistake is therefore
 * bounded to ONE picture, once — not to a whole session of silent degradation.
 *
 * Two more guards: it takes `L10_STABLE` consecutive pictures with the same
 * count before daring, and any change of count resets the learning.
 * `SHADOW_FLUSH_UDP=0` disables the whole mechanism. */
#define L10_STABLE 30
static int  g_slices_seen      = 0;   /* count of the previous picture */
static int  g_slices_stable    = 0;   /* how many consecutive pictures hold that count */
static int  g_slices_expected  = 0;   /* 0 = we do not dare yet */
static bool g_early_flush      = false;
static int  g_l10_off          = 0;   /* 1 = proven wrong, never again */
static uint32_t g_display_ts = 0;
static bool     g_display_keyframe = false;
static int      g_display_subchans_seen = 0;

/* Forward decl */
/* === CFG-3 2026-09-11 — THE NAL GRAMMAR BELONGS TO THE PICTURE, NOT TO THE PROCESS ===
 *
 * K16 gave the reassembly ONE grammar flag, shared by the IDR detection of
 * F11/G43 (vid_decide_truncation) and by G40's access-unit split and the NAL
 * counters (emit_legacy), so that the two could never read the same stream two
 * different ways. That part stays. What did not hold: the flag was resolved
 * lazily from SHADOW_CODEC at the first emission of the PROCESS and never reset,
 * while the decoder and the announcement re-read SHADOW_CODEC every session and
 * the Quality screen rewrites it live. After an H.265 session, an H.264 session
 * was parsed with the HEVC grammar - on the real dump, 0 picture starts, 0 IDR
 * and 0 SPS against 1928 / 8 / 4: G40's split, F11 and G43 silently off, the
 * NAL counters at 0, a false [K16d] alarm, and L10 switching itself off. Its -1
 * sentinel was also TRUTHY: the first incomplete picture of a process was
 * judged with the HEVC rules whatever the codec.
 *
 * Every picture states its own codec: the HIGH nibble of VideoFrame byte 0
 * (RE-3, [C95+] - the official client picks its NAL grammar from exactly that
 * value). So emit_legacy takes the grammar from the picture it parses and leaves
 * it in g_hevc_nal, where vid_decide_truncation reads it for the NEXT incomplete
 * picture - it runs before emit_legacy has seen that picture's header. All four
 * are resolved EAGERLY by vid_reasm_reset_session: never -1, never carried over
 * from another session, and the latch starts each session on what SHADOW_CODEC
 * asked for. SHADOW_NAL_FROM_STREAM=0 takes the grammar from SHADOW_CODEC alone
 * (the old rule, minus the freeze). */
static int g_codec_env   = 0;   /* SHADOW_CODEC as the decoder reads it: 0 H.264, 1 H.265, 2 AV1 */
static int g_hevc_env    = 0;   /* g_codec_env == 1 */
static int g_hevc_nal    = 0;   /* grammar of the last picture parsed; F11/G43 read it */
static int g_codec_noted = 0;   /* the [K16] stream/request mismatch was said this session */

static void emit_legacy(session_ctx_t *ctx, uint8_t *plain, int ct_len);

static void vid_reasm_clear(vid_reasm_t *r) {
    for (int i = 0; i < VID_MAX_CHUNKS; i++) {
        free(r->chunks[i]);
        r->chunks[i] = NULL;
        r->chunk_lens[i] = 0;
        r->slot_seen[i] = false;
        r->chunk_is_data[i] = false;
    }
    r->data_count = 0;
    r->max_chunks = 0;
    r->recv_count = 0;
    r->highest_idx = 0;
    r->active = false;
    r->cur_frame_id = 0;  /* N56: release the frame_id lock */
    /* V10: without this reset, `nack_done` would stay true and the NEXT picture
     * would never be requested — exactly the defect family this repo knows by
     * heart (session state outliving its object). */
    r->nack_done = false;
    r->nack_frame_id = 0;
}

/* Concatenate the plaintexts present in chunk_idx order (skipping holes, i.e.
 * byte10==0 chunks / FEC parity / lost). Emit via emit_legacy. */
/* Accounts for what is missing from an incomplete picture and decides whether to
 * request a key frame.
 *
 * Gathers four results that all answer the same question — "does this loss
 * justify an IDR?": the NACK queueing (F18), the loss rate counted in CHUNKS
 * rather than pictures (G6), the record of WHICH chunks are missing — averages
 * say "2 % lost" when it is always the same chunk going missing — and G12, which
 * refuses to request a key frame for a tail loss, the one case where the picture
 * stays usable truncated.
 *
 * Extracted from vid_reasm_flush on 2026-08-25: 117 lines, no early return, and
 * nothing on input but the context and the reassembly state. */
/* === V10 2026-08-28 — ASKING WHILE IT IS STILL THE CURRENT PICTURE ===
 *
 * G15 had established half the problem: the `rG` packet carries NO picture
 * identifier, so the indices it lists can only designate the current picture ON
 * THE SERVER'S SIDE. G15 concluded that it had to be sent without delay — but
 * the request was still BUILT at flush time, i.e. once the picture had been
 * concatenated, emitted, and its buffer returned. So we were asking for chunks of
 * a picture we had already dropped, from a server already encoding one or two
 * more. The reply did arrive — and landed in the void (`chunks_orphan`).
 *
 * So the request is built at the moment the hole becomes CERTAIN: when the LAST
 * chunk of the picture arrives while some are still missing. At that instant the
 * server is still on that picture, and the indices mean what we think they
 * mean.
 *
 * `nack_done` prevents the double request: the flush calls this function again
 * as a safety net for the picture whose last chunk never arrives.
 *
 * SHADOW_NACK_AT_GAP=0 restores queueing at flush time only. */
static void vid_nack_enqueue(session_ctx_t *ctx, vid_reasm_t *r)
{
    static int g_nack_on = -1;
    if (g_nack_on < 0) {
        const char *e = getenv("SHADOW_NACK");
        g_nack_on = e ? atoi(e) : 1;
    }
    if (!g_nack_on) return;
    if (r->recv_count >= r->max_chunks || r->max_chunks == 0) return;
    if (r->nack_done && r->nack_frame_id == r->cur_frame_id) return;

    int next = (ctx->nack_tail + 1) % NACK_QUEUE_CAP;
    if (next == ctx->nack_head) return;            /* queue full */

    nack_request_t *req = &ctx->nack_queue[ctx->nack_tail];
    req->count = 0;
    req->frame_id = r->cur_frame_id;
    for (int i = 0; i < (int)r->max_chunks && req->count < NACK_MAX_INDICES; i++)
        if (!r->slot_seen[i]) req->indices[req->count++] = (uint16_t)i;

    if (req->count > 0) {
        ctx->nack_tail   = next;
        r->nack_done     = true;
        r->nack_frame_id = r->cur_frame_id;
    }
}

static void vid_account_loss(session_ctx_t *ctx, vid_reasm_t *r)
{
    /* G6: incomplete frame (UDP loss) -> request an event-driven IDR (the main
     * loop will send it, rate-limited). Recovery without the autofocus. */
    /* Real loss rate, in CHUNKS rather than pictures: an incomplete picture may
     * have lost one chunk out of twenty. This is the figure that says whether
     * the artefacts come from the network or from us — a near-zero rate with
     * many incomplete pictures would point at our reassembly. */
    /* Which chunks are missing, concretely? The averages say we expect 1.6 times
     * more than we receive, which cannot be network loss at 2.8 Mbit/s. The
     * pattern of the absent indices will settle it: a missing tail points at a
     * premature flush, a missing start at a mid-picture startup, an isolated
     * hole at a real loss. */
    {
        static int g_flush_dbg = 0;
        if (g_flush_dbg < 15 && r->max_chunks > 0 && r->recv_count < r->max_chunks
            && shadow_diag_reasm()) {
            /* INCOMPLETE flushes only: the complete ones teach us nothing, and
             * they were the ones filling up the sample. */
            g_flush_dbg++;
            char miss[200]; int mo = 0; miss[0] = 0;
            int nmiss = 0;
            for (int i = 0; i < (int)r->max_chunks && i < VID_MAX_CHUNKS; i++) {
                if (!r->slot_seen[i]) {
                    nmiss++;
                    if (mo < (int)sizeof(miss) - 8)
                        mo += snprintf(miss + mo, sizeof(miss) - mo, "%s%d",
                                       mo ? "," : "", i);
                }
            }
            clog("[FLUSH] INCOMPLETE max=%u recv=%u missing=%d [%s]",
                 r->max_chunks, r->recv_count, nmiss, miss);
        }
    }

    if (r->max_chunks > 0) {
        memcpy(g_last_slots, r->slot_seen, sizeof(g_last_slots));
        g_last_max = r->max_chunks;
        g_last_valid = true;
        ctx->stats->chunks_expected += r->max_chunks;
        if (r->recv_count < r->max_chunks) {
            const uint32_t missing = (uint32_t)(r->max_chunks - r->recv_count);
            ctx->stats->chunks_missing += missing;
            if (missing == 1) ctx->stats->frames_miss1++;
            /* We remember the picture we have just truncated: if its chunks
             * arrive right after, that is REORDERING and not a loss, and a short
             * grace period would recover them. */
            g_flushed_fid = r->cur_frame_id;
        }
    }

    /* === G12 2026-08-22 — DO NOT REQUEST A KEY FRAME FOR A LATE TAIL ===
     *
     * G6 requested a full refresh as soon as a picture was incomplete. But the
     * measurement shows that our incomplete pictures ALL miss exactly one chunk,
     * the last one — the tail that arrives after the start of the next picture
     * (G9), and not a loss.
     *
     * So we were requesting a key frame roughly once per second (`idr_t=22` in
     * 45 s). Each one costs a frame several times heavier; under a bitrate cap
     * the encoder compensates by lowering quality during and just after. Those
     * are the blocks that appear then clean themselves up — and it explains why
     * 95 % of the pictures with artefacts are assembled NORMALLY on our side:
     * the defect is encoded at the source, at our request.
     *
     * === G26 2026-08-22 — REQUEST AN IDR ON ANY REAL LOSS (tail INCLUDED) ===
     * G12 removed the IDR request when ONLY the tail was missing, on the grounds
     * that it was a late tail (G9), not a loss. But this path is only reached
     * AFTER g_pending's grace period: a picture still incomplete here has
     * already had time for its tail to arrive — so it is a REAL loss. Removing
     * the request let the corruption (reference propagation on motion, the
     * bottom slice above all) settle in until the next spontaneous key frame
     * (several seconds): exactly the "global when the scene moves" artefacts
     * reported, with the taskbar (static) intact. So we request an IDR on any
     * real loss, rate-limited (~500 ms) downstream so as not to spam.
     * SHADOW_IDR_ON_LOSS=0 disables it; =3 restores the old tail exemption. */
    if (r->recv_count < r->max_chunks && r->max_chunks > 0) {
        static int g_idr_on_loss = -1;
        if (g_idr_on_loss < 0) {
            const char *e = getenv("SHADOW_IDR_ON_LOSS");
            g_idr_on_loss = e ? atoi(e) : 1;
        }
        const uint16_t missing = (uint16_t)(r->max_chunks - r->recv_count);
        const bool tail_only =
            (missing == 1 && r->max_chunks > 0
             && !r->slot_seen[r->max_chunks - 1]);
        if (g_idr_on_loss == 1 || g_idr_on_loss == 2
            || (g_idr_on_loss == 3 && !tail_only))
            g_idr_needed = 1;
    }
    /* V10: the queueing now lives in vid_nack_enqueue(), called AS SOON AS the
     * hole is detected. This here is the safety net, for the picture whose last
     * chunk never arrived. */
    vid_nack_enqueue(ctx, r);
}

/* Decides what to do with an incomplete picture: drop it, truncate it at the
 * last complete NAL, or emit it as-is.
 *
 * Three results cross here. F8 truncates before the last started NAL, otherwise
 * the decoder trips on a cut NAL. F11 DROPS an incomplete IDR: a corrupted key
 * frame poisons every reference that follows it, whereas a truncated P-frame
 * only bothers itself. G43 does the opposite for a TAIL loss — received
 * contiguous from 0, little missing, no IDR: we emit truncated, because dropping
 * breaks the reference chain and makes the decoder drift until the next key
 * frame. The exact rule lives in vid_wire.c, where it is tested with its
 * counter-case.
 *
 * Extracted from vid_reasm_flush on 2026-08-25. Returns false when the picture
 * is dropped, in which case the caller stops; `*off_io` may have been reduced by
 * the truncation, and `*out_tail_loss` says whether what is about to be emitted
 * is a missing tail. */
static bool vid_decide_truncation(vid_reasm_t *r, const uint8_t *big,
                                  size_t *off_io, int last_contiguous,
                                  int last_idx, int holes, int concat_mode,
                                  bool *out_tail_loss)
{
    const int g_concat_mode = concat_mode;
    (void)last_idx; (void)holes;
    size_t off = *off_io;
    /* F8 step 2: on an INCOMPLETE frame (= recv_count < max_chunks), truncate
     * big[] at the end of the LAST complete NAL (= step back to the previous
     * start code). Without that, the truncated slice makes libavcodec decode
     * partially and fill the rest with zero/black (= visible green blocks). By
     * truncating at the NAL boundary we send the decoder WHOLE slices only ->
     * the MBs not covered by the NALs are left unchanged (= reference frame N-1
     * preserved = stable taskbar). */
    /* === G43 2026-08-22 — TAIL LOSS: EMIT TRUNCATED INSTEAD OF DROPPING ===
     *
     * Decisive G42 measurement: ~1.7 % of pictures lose ONLY their LAST chunk
     * (the tail of the bottom slice, ~489 B); everything else (SoF + top slice +
     * nearly all of the bottom) is there and contiguous. We dropped them
     * entirely -> the decoder's reference (max_num_ref_frames=1) went stale ->
     * drift/fog over the whole picture until the next IDR, worse under heavy
     * motion (bigger pictures = more chunks = more risk of losing the tail).
     * Offline test: emitting the picture TRUNCATED (top + nearly complete
     * bottom, without removing the incomplete bottom NAL) gives MAE ~0 (a nearly
     * perfect reference, only the last MB rows lightly concealed) against a
     * persistent corrupted block (max ~200) when dropping. So we keep the
     * truncated bottom NAL (we do NOT step back to the last complete NAL = F8)
     * and emit. Applies only to TAIL losses (all received contiguous from 0,
     * little missing) and NOT to IDRs (an incomplete IDR is still dropped by
     * F11). SHADOW_EMIT_TAIL_TRUNC=0 restores the old drop. */
    bool tail_loss = false;
    {
        static int g_emit_tail = -1;
        if (g_emit_tail < 0) { const char *e = getenv("SHADOW_EMIT_TAIL_TRUNC"); g_emit_tail = e ? atoi(e) : 1; }
        tail_loss = g_emit_tail && vid_wire_is_tail_loss(r->recv_count, r->max_chunks,
                                                        last_contiguous, false);
    }

    bool has_idr = false;
    if (r->recv_count < r->max_chunks && off > 0 && g_concat_mode != 1) {
        /* Find the last occurrence of 00 00 [00] 01 in big[] (= the start of an
         * incomplete NAL). We truncate JUST before it.
         * F11 2026-05-22 22h55: during that scan, also detect whether we have a
         * type 5 NAL (= IDR slice). An incomplete IDR is catastrophic (= a
         * corrupted reference frame -> indefinite propagation). */
        size_t last_nal_start = 0;
        for (size_t k = 4; k < off; k++) {
            bool sc4 = (big[k-3]==0 && big[k-2]==0 && big[k-1]==0 && big[k]==1);
            bool sc3 = (!sc4 && big[k-2]==0 && big[k-1]==0 && big[k]==1);
            if (sc4 || sc3) {
                size_t start = sc4 ? k - 3 : k - 2;
                last_nal_start = start;
                /* Check NAL type at position k+1 */
                if (k + 1 < off) {
                    /* K16 — the same trap as at the decoder's gate: in HEVC the
                     * type lives on six bits starting at bit 1, and the
                     * instantaneous-refresh pictures run from 16 to 21. Under the
                     * H.264 rule, `has_idr` stayed ALWAYS false and G43 emitted
                     * truncated IDRs — precisely what it forbids. */
                    const uint8_t nt = g_hevc_nal
                        ? (uint8_t)((big[k+1] >> 1) & 0x3F)
                        : (uint8_t)(big[k+1] & 0x1f);
                    if (g_hevc_nal ? (nt >= 16 && nt <= 21) : (nt == 5))
                        has_idr = true;
                }
            }
        }
        /* F11: drop entire incomplete IDR frames. The reference frame stays the
         * previous COMPLETE IDR (= the taskbar stays stable). Set
         * SHADOW_DROP_INCOMPLETE_IDR=0 to disable (= bypass = legacy F8). */
        static int g_drop_incomplete_idr = -1;
        if (g_drop_incomplete_idr < 0) {
            const char *e = getenv("SHADOW_DROP_INCOMPLETE_IDR");
            g_drop_incomplete_idr = e ? atoi(e) : 1;
        }
        if (g_drop_incomplete_idr && has_idr) {
            static int g_drop_idr_dbg = 0;
            if (g_drop_idr_dbg < 10) {
                clog("F11 DROP_INCOMPLETE_IDR recv=%u/%u (incomplete IDR not used as reference)",
                     r->recv_count, r->max_chunks);
                g_drop_idr_dbg++;
            }
            vid_reasm_clear(r);
            return false;  /* picture dropped: never feed the decoder a corrupted IDR */
        }
        /* G43: an incomplete IDR is never emitted truncated. We re-evaluate now
         * that has_idr is known (the pure function carries that rule). */
        tail_loss = tail_loss && vid_wire_is_tail_loss(r->recv_count, r->max_chunks,
                                                      last_contiguous, has_idr);
        if (!tail_loss && last_nal_start > 0 && last_nal_start < off) {
            static int g_trunc_dbg = 0;
            if (g_trunc_dbg < 20) {
                clog("F8 TRUNC last_nal_start=%zu off=%zu (removed %zu trailing bytes)",
                     last_nal_start, off, off - last_nal_start);
                g_trunc_dbg++;
            }
            off = last_nal_start;  /* remove incomplete trailing NAL */
        }
    }
    static int g_dbg_flush = 0;
    if (g_dbg_flush < 30) {
        clog("FLUSH max=%u recv=%u highest=%d holes=%d total=%zu",
             r->max_chunks, r->recv_count, last_idx, holes, off);
        g_dbg_flush++;
    }

    *off_io = off;
    *out_tail_loss = tail_loss;
    return true;
}

/* === K19 2026-08-28 — THE SEQUENCE TRACKER IGNORED SINGLE-PACKET PICTURES ===
 *
 * `g_emit_last_sub` was only updated inside `vid_reasm_flush()`. But the fast
 * path for SINGLE-CHUNK pictures ("case A") decrypts, emits and RETURNS before
 * reaching the flush. Every picture fitting in one packet was therefore INVISIBLE
 * to G36's gap detector.
 *
 * The consequence closes on itself: a run of small pictures freezes the counter;
 * the first picture big enough to be multi-chunk arrives with a far-away
 * subchannel, which reads as a GAP; we request a key frame; the key frame is
 * big, hence visible, hence it "proves" the next gap. The loop never ends — same
 * family as G5 (autofocus) and G18/G37 (key frame on a phantom error).
 *
 * THE MEASUREMENT THAT SETTLES IT, in a real log between t=10 s and t=20 s: 358
 * pictures decoded, 358 decryptions, ZERO continuations, `lost` frozen at 4/1132
 * — and `g_emit_last_sub` stuck at 83 for 21 seconds. Then "[G42] GAP subchan
 * 83->54 (jump 226)". A jump of 226 pictures at 38 fps would be SIX SECONDS of
 * silence, while the decoder was running at 35.8 fps without interruption. It is
 * not the server skipping: it is the counter that was blind.
 *
 * So the tracking lives in its own function, called by BOTH emission paths. `r`
 * may be null — the single-chunk path has no reassembly buffer, and the G42
 * diagnostic does without it. */
static void vid_note_emitted_sub(session_ctx_t *ctx, int this_sub)
{
    (void)ctx;
        static int g_idr_on_gap = -1;
        if (g_idr_on_gap < 0) { const char *e = getenv("SHADOW_IDR_ON_GAP"); g_idr_on_gap = e ? atoi(e) : 1; }
        if (this_sub >= 0) {
            if (g_emit_last_sub >= 0 && this_sub != ((g_emit_last_sub + 1) & 0xff)) {
                if (g_idr_on_gap) g_idr_needed = 1;   /* picture skipped -> resync IDR */
                /* G42 diag: count the subchan GAPS (= pictures entirely
                 * unemitted = a direct candidate for the frame_num hole / fog).
                 * A gap may skip several subchans; we log the distance. */
                g_sub_gaps++;   /* CONC-4: per session, reset in vid_reasm_reset_session */
                int gap = (this_sub - g_emit_last_sub - 1) & 0xff;
                /* G42+: state of the first skipped subchan = the incomplete
                 * picture never emitted. Says WHICH chunk is missing
                 * (SoF/middle/tail) -> which remedy (emit the complete top slice,
                 * or just an IDR). */
                int ss = (g_emit_last_sub + 1) & 0xff;
                vid_reasm_t *sr = &g_vid_reasm[ss];
                int first_missing = -1, sof_ok = -1, contig_top = 0;
                if (sr->active && sr->max_chunks > 0) {
                    sof_ok = sr->slot_seen[0] ? 1 : 0;
                    for (int q = 0; q < (int)sr->max_chunks && q < VID_MAX_CHUNKS; q++) {
                        if (!sr->slot_seen[q]) { first_missing = q; break; }
                        contig_top = q + 1;  /* number of chunks contiguous from 0 */
                    }
                }
                if (g_sub_gaps <= 20 || (g_sub_gaps % 25) == 0)
                    clog("[G42] GAP subchan %d->%d (jump %d, total %u) | picture[%d]: "
                         "active=%d recv=%u/%u sof=%d first_missing=%d contig=%d",
                         g_emit_last_sub, this_sub, gap, g_sub_gaps, ss,
                         sr->active, sr->recv_count, sr->max_chunks,
                         sof_ok, first_missing, contig_top);
            }
            g_emit_last_sub = this_sub;
        }
}

static void vid_reasm_flush(session_ctx_t *ctx, vid_reasm_t *r) {
    if (!r->active || r->recv_count == 0) { vid_reasm_clear(r); return; }
    /* === L5 2026-08-29 — DURATION OF A PICTURE'S BURST ===
     * From the first chunk to the moment the picture leaves the reassembler.
     * This is the only item of the video path that contains any NETWORK, hence
     * the only one whose upper tail says something about the link rather than
     * about us. Taken here and not on receipt of the last chunk: every exit from
     * this function goes through `vid_reasm_clear()`, which wipes `first_seen`.
     * It also counts truncated and abandoned pictures — their burst is precisely
     * the one that lasted too long, and excluding it would make the histogram
     * optimistic. */
    if (r->first_seen_us > 0 && latency_enabled())
        latency_add(LAT_VID_BURST, latency_now_us() - r->first_seen_us);
    /* V11: this picture leaves the buffer, whatever the outcome (emitted,
     * emitted truncated, or dropped). We keep WHAT IS NEEDED to classify the
     * chunks that will arrive next on this subchannel — see
     * vid_wire_orphan_is_dup(). Set here and not lower down because every exit
     * from this function calls vid_reasm_clear(), which wipes exactly what we
     * read. */
    r->emitted_max      = r->max_chunks;
    r->emitted_complete = (r->recv_count >= r->max_chunks);
    r->emitted_ord      = ++g_flush_ord;
    size_t total = 0;
    int last_idx = (int)r->highest_idx;

    /* RE3 2026-05-19 — drop partial frames BEFORE emit_legacy.
     * libavcodec with OUTPUT_CORRUPT=0 does NOT mark concealed frames as
     * corrupt -> our `decode_error_flags` check does not catch them. The clean
     * solution = NEVER send an incomplete bytestream to the decoder.
     *
     * Default OFF (= V11 compatible = include partial). SHADOW_DROP_PARTIAL=1
     * to enable = the picture freezes on a hole but ZERO visible artefact.
     *
     * Heuristic: if recv_count < max_chunks -> at least 1 chunk is missing ->
     * skip the emit. Frame lost, we wait for the next complete one. */
    /* F5 REVERT 2026-05-22 22h32: default OFF (= was 1, which caused GUI
     * FLICKER because dropped frames = effective fps down to 15 on scenes with
     * frequent UDP loss). Replaced by the F7 break-on-first-hole strategy (see
     * the concat loop in vid_reasm_flush). */
    static int g_drop_partial = -1;
    if (g_drop_partial < 0) {
        const char *e = getenv("SHADOW_DROP_PARTIAL");
        g_drop_partial = e ? atoi(e) : 0;
    }
    if (g_drop_partial && r->recv_count < r->max_chunks) {
        ctx->stats->incomplete_at_flush++;
        static int g_dbg_drop = 0;
        if (g_dbg_drop < 20) {
            clog("DROP_PARTIAL recv=%u/%u -> skip emit", r->recv_count, r->max_chunks);
            g_dbg_drop++;
        }
        vid_reasm_clear(r);
        return;
    }

    int holes = 0;
    for (int i = 0; i <= last_idx; i++) {
        if (r->chunks[i] && r->chunk_lens[i] > 0) total += r->chunk_lens[i];
        else holes++;
    }
    /* Diagnostic (off by default): on every picture emission, details the
     * reassembly state — chunks received out of expected, holes, index of the
     * last chunk and its size. This is the view that established G4 (the last
     * chunk was systematically missing); 12 lines at most, so as not to drown
     * the log. */
    if (getenv("SHADOW_DBG_FLUSH")) {
        static int df=0;
        if (df<12){ df++;
            int lastnull = (r->max_chunks>0 && r->max_chunks<=VID_MAX_CHUNKS) ? (r->chunks[r->max_chunks-1]==NULL) : -1;
            size_t lastlen = (r->max_chunks>0 && r->max_chunks<=VID_MAX_CHUNKS && r->chunks[r->max_chunks-1]) ? r->chunk_lens[r->max_chunks-1] : 0;
            clog("DBGFLUSH max=%u recv=%u highest=%d holes=%d last_idx=%d chunks[max-1]_null=%d last_len=%zu total=%zu",
                 r->max_chunks, r->recv_count, (int)r->highest_idx, holes, last_idx, lastnull, lastlen, total);
        }
    }
    if (total == 0 || total > 4 * 1024 * 1024) { vid_reasm_clear(r); return; }
    static uint8_t big[4 * 1024 * 1024];
    size_t off = 0;

    /* Byte-by-byte RE 2026-05-19 — dump every chunk with its metadata, to
     * identify the garbage bytes offline. Format: one line per chunk.
     * SHADOW_DUMP_CHUNKS=1 (= writes to ./halyard-data/chunks.log). */
    static int g_dump_chunks = -1;
    static FILE *g_dump_chunks_f = NULL;
    static int g_chunk_dump_count = 0;
    if (g_dump_chunks < 0) {
        const char *e = getenv("SHADOW_DUMP_CHUNKS");
        g_dump_chunks = e ? atoi(e) : 0;
        if (g_dump_chunks) {
            g_dump_chunks_f = fopen("./halyard-data/chunks.log", "wb");
        }
    }

    /* RE5 2026-05-22 — SHADOW_PARITY_AS_NAL: insert 00 00 00 01 before every
     * continuation at concat time.
     *
     * The hypothesis that motivated this attempt — "these chunks are not
     * Reed-Solomon but slices of the tail" — has since been SETTLED by F31:
     * there is no FEC, they really are continuations. Inserting a start code in
     * front of each one is nonetheless WRONG in the general case, because a
     * continuation resumes in the middle of a NAL and not at its boundary.
     * Default 0, and there is no known reason to change it. */
    static int g_parity_as_nal = -1;
    if (g_parity_as_nal < 0) {
        const char *e = getenv("SHADOW_PARITY_AS_NAL");
        g_parity_as_nal = e ? atoi(e) : 0;
    }
    static const uint8_t k_nal_start[4] = {0x00, 0x00, 0x00, 0x01};
    /* F8 SKIP-INCOMPLETE-PARITY 2026-05-22 22h35: if the tail parity (= bottom
     * slice) has a single hole -> emit ONLY the chunks contiguous from idx 0 up
     * to the first hole. The incomplete slice 2 is omitted entirely ->
     * libavcodec does not touch the bottom MBs -> keeps reference frame N-1 (= a
     * stable taskbar from the last complete IDR, no green/black blocks).
     *
     * Strategy step 1: compute the LAST chunk_idx reachable contiguously from
     * idx 0 with no hole. Beyond that, we cut.
     * Modes: SHADOW_CONCAT_MODE=
     *   0 = break-on-hole (= F7, the current default)
     *   1 = concat-thru-holes (= legacy V11)
     *   2 = drop-tail-on-any-hole (= F8's next step) */
    static int g_concat_mode = -1;
    if (g_concat_mode < 0) {
        const char *e = getenv("SHADOW_CONCAT_MODE");
        g_concat_mode = e ? atoi(e) : 0;  /* default break-on-hole */
    }
    /* Find the last chunk_idx contiguous from 0 (= common to F7 / F8) */
    int last_contiguous = -1;
    for (int i = 0; i <= last_idx; i++) {
        if (r->chunks[i] && r->chunk_lens[i] > 0) last_contiguous = i;
        else break;
    }
    /* F8: if we do not have the last expected chunk (max-1), the tail slice is
     * incomplete. In that case we step back to the LAST known NAL boundary (=
     * start code) in big[], so as not to send a truncated slice to the decoder.
     * That step is applied after the concat below. */
    for (int i = 0; i <= last_idx; i++) {
        if (g_concat_mode == 0 && (!r->chunks[i] || r->chunk_lens[i] == 0)) {
            break;
        }
        if (g_concat_mode == 1) { /* legacy: skip holes, continue */ }
        if (r->chunks[i] && r->chunk_lens[i] > 0) {
            /* RE5: insert a start code before EVERY parity chunk when the mode
             * is on. Skips the first chunk so as not to break the slice
             * header's natural start code (= typically already in chunk 0). */
            if (g_parity_as_nal && i > 0 && !r->chunk_is_data[i]
                && off + 4 <= sizeof(big)) {
                memcpy(big + off, k_nal_start, 4);
                off += 4;
            }
            /* Dump the chunk with its metadata before concatenating */
            if (g_dump_chunks_f && g_chunk_dump_count < 5000) {
                size_t pos_in_concat = off;
                fprintf(g_dump_chunks_f,
                        "CHUNK fid=0x%08x idx=%d max=%u kind=%s len=%zu pos=%zu first16=",
                        r->cur_frame_id, i, r->max_chunks,
                        r->chunk_is_data[i] ? "DATA  " : "PARITY",
                        r->chunk_lens[i], pos_in_concat);
                size_t n = r->chunk_lens[i] < 16 ? r->chunk_lens[i] : 16;
                for (size_t k = 0; k < n; k++)
                    fprintf(g_dump_chunks_f, "%02x ", r->chunks[i][k]);
                fprintf(g_dump_chunks_f, "last16=");
                if (r->chunk_lens[i] > 16) {
                    size_t start = r->chunk_lens[i] - 16;
                    for (size_t k = start; k < r->chunk_lens[i]; k++)
                        fprintf(g_dump_chunks_f, "%02x ", r->chunks[i][k]);
                }
                fprintf(g_dump_chunks_f, "\n");
                fflush(g_dump_chunks_f);
                g_chunk_dump_count++;
            }
            memcpy(big + off, r->chunks[i], r->chunk_lens[i]);
            off += r->chunk_lens[i];
        }
    }
    /* Loss accounting and IDR request — see vid_account_loss(). */
    vid_account_loss(ctx, r);

    /* Drop, truncate or emit as-is — see vid_decide_truncation(). */
    bool tail_loss = false;
    if (!vid_decide_truncation(r, big, &off, last_contiguous, last_idx, holes,
                               g_concat_mode, &tail_loss)) return;

    /* 2026-05-18: log the holes when flushing incomplete — blurry-taskbar
     * diagnostic. Holes at the end of a slice (= small last_idx - hole_pos)
     * corrupt the last MBs (= the 1080p taskbar area). Capped at 50 lines so as
     * not to spam. */
    if (holes > 0) {
        static int g_hole_dbg = 0;
        if (g_hole_dbg < 50) {
            /* Find the first hole index, to know where the corruption starts */
            int first_hole = -1;
            for (int i = 0; i <= last_idx; i++) {
                if (!r->chunks[i] || r->chunk_lens[i] == 0) { first_hole = i; break; }
            }
            clog("HOLES holes=%d/%d first_hole_idx=%d max=%u total_bytes=%zu",
                 holes, last_idx + 1, first_hole, r->max_chunks, off);
            g_hole_dbg++;
        }
    }
    /* F23: when the reassemble-then-decrypt mode is on, big[] now holds the raw
     * concatenation of every chunk. Expected format (per the RE doc):
     *   [ciphertext_N | nonce_12B | tag_16B]
     * SHADOW_REASSEMBLE_REVERSE=1 if the chunk order must be reversed (=
     * chunk_idx 0 holds the FINAL bytes of the ciphertext, incl. nonce+tag). */
    static int g_reasm_dec = -1;
    if (g_reasm_dec < 0) {
        const char *e = getenv("SHADOW_REASSEMBLE_DECRYPT");
        g_reasm_dec = e ? atoi(e) : 0;
    }
    if (g_reasm_dec && off > 28) {
        static int g_rev = -1;
        if (g_rev < 0) {
            const char *e = getenv("SHADOW_REASSEMBLE_REVERSE");
            g_rev = e ? atoi(e) : 0;
        }
        /* If REVERSE: rebuild big[] in reverse order */
        if (g_rev) {
            static uint8_t reversed[4 * 1024 * 1024];
            size_t roff = 0;
            for (int i = last_idx; i >= 0; i--) {
                if (r->chunks[i] && r->chunk_lens[i] > 0
                    && roff + r->chunk_lens[i] <= sizeof(reversed)) {
                    memcpy(reversed + roff, r->chunks[i], r->chunk_lens[i]);
                    roff += r->chunk_lens[i];
                }
            }
            memcpy(big, reversed, roff);
            off = roff;
        }
        int ct_len = (int)off - 28;
        if (ct_len > 0 && ct_len < (int)sizeof(big) - 28) {
            const uint8_t *nonce = big + ct_len;
            const uint8_t *tag   = big + ct_len + 12;
            static uint8_t plain[4 * 1024 * 1024];
            memcpy(plain, big, ct_len);
            bool ok = shadow_cipher_decrypt_unsafe(ctx->cipher, plain, ct_len, nonce, tag);
            static int g_rd_log = 0;
            if (g_rd_log < 20) {
                clog("F23 REASM_DECRYPT recv=%u/%u ct_len=%d reverse=%d -> %s",
                     r->recv_count, r->max_chunks, ct_len, g_rev,
                     ok ? "OK" : "FAIL");
                g_rd_log++;
            }
            if (ok) {
                ctx->stats->decrypt_ok++;
                emit_legacy(ctx, plain, ct_len);
                vid_reasm_clear(r);
                return;
            } else {
                ctx->stats->decrypt_fail++;
            }
        }
    }
    /* The Reed-Solomon decoder has been REMOVED: F31 established that there is
     * NO FEC in this protocol (the so-called "parity" chunks are the rest of the
     * NAL, in plaintext). It had been off by default since. KB §3.15. */

    /* Correlation log: sequence number of the emitted picture and the state of
     * its assembly. Offline analysis gives the numbers of the pictures carrying
     * an artefact; it is then enough to look them up here to know whether they
     * had anything abnormal — incomplete, held in the wait, or perfectly normal.
     * In that last case the defect would not be in the assembly at all, and we
     * would stop looking here.
     *
     * The number follows the one in the `SHADOW_DUMP_H264` dump: one emission,
     * one picture. */
    if (shadow_diag_reasm()) {
        static uint32_t emitted = 0;
        clog("[EMIT] n=%u chunks=%u/%u %s", emitted++, r->recv_count,
             r->max_chunks,
             r->recv_count >= r->max_chunks ? "complete" : "INCOMPLETE");
    }

    /* === G16 2026-08-22 — SHOULD A TRUNCATED PICTURE BE EMITTED? ===
     *
     * Since May (F9) we emit what we have rather than dropping, on the grounds
     * that a truncated picture beats a jump to an old key frame. That choice was
     * made when the picture was broken across the board; it has never been
     * re-examined since the rest became clean.
     *
     * But a cut slice makes decoding fail at the point of the cut — ffmpeg
     * reports it on our dumps (`bytestream -6`) — and the result enters the
     * reference pictures: the defect propagates until the next key frame.
     * Emitting nothing, by contrast, leaves a clean reference, albeit one
     * picture behind.
     *
     * Both options are defensible, and no measurement has separated them.
     * SHADOW_EMIT_TRUNCATED=1 restores May's behaviour. */
    if (r->recv_count < r->max_chunks && r->max_chunks > 0 && !tail_loss) {
        static int g_emit_trunc = -1;
        if (g_emit_trunc < 0) {
            const char *e = getenv("SHADOW_EMIT_TRUNCATED");
            g_emit_trunc = e ? atoi(e) : 0;
        }
        if (!g_emit_trunc) {
            ctx->stats->frames_dropped_trunc++;
            vid_reasm_clear(r);
            return;
        }
    }

    /* === G36 — sequence-gap detection -> IDR request (bounds the drift) === */
    vid_note_emitted_sub(ctx,
                         (r >= g_vid_reasm && r < g_vid_reasm + VID_MAX_SUBCHANS)
                             ? (int)(r - g_vid_reasm) : -1);

    emit_legacy(ctx, big, (int)off);
    vid_reasm_clear(r);
}

/* === G24 — ORDERED DRAIN (reorder buffer) ===
 * Emits pictures IN subchan ORDER starting from g_emit_next_sub, as long as they
 * are complete. The head is held for up to g_hold_ms (default 300 ms, like the
 * official client); past that delay the picture is declared lost: we drop it
 * (never an incomplete picture to the decoder) and request an IDR. Called after
 * every stored chunk and from the main loop (for the timeout when the head
 * stalls). */
void vid_drain_ordered(session_ctx_t *ctx, int64_t now_ms, int socket_drained) {
    if (!vid_reorder_enabled() || g_emit_next_sub < 0) return;
    static int g_hold_ms = -1;
    if (g_hold_ms < 0) {
        const char *e = getenv("SHADOW_REORDER_MS");
        g_hold_ms = e ? atoi(e) : 150;   /* G35: 150 ms is enough for late tails */
    }
    for (int guard = 0; guard < VID_MAX_SUBCHANS; guard++) {
        vid_reasm_t *r = &g_vid_reasm[g_emit_next_sub & 0xff];
        int complete = (r->active && r->max_chunks > 0
                        && r->recv_count >= r->max_chunks);
        if (complete) {
            vid_reasm_flush(ctx, r);                 /* emit in order + clear */
            r->expect_sof = true;                    /* ignore stragglers/duplicates until the next SoF */
            g_emit_next_sub = (g_emit_next_sub + 1) & 0xff;
            g_head_since_ms = now_ms;
        } else if (socket_drained && r->active && now_ms - g_head_since_ms >= g_hold_ms) {
            /* We declare a loss ONLY when the socket is empty: otherwise the
             * head's tail may still be queued (our synchronous decode processes
             * packets in bursts), and we would drop it wrongly. Empty socket +
             * elapsed delay = the tail is never coming. */
            /* A head late beyond the delay = real loss. We NEVER feed an
             * incomplete picture: we drop it and request an IDR. */
            if (getenv("SHADOW_DIAG_REORDER")) {
                static int dd=0;
                if (dd<40){ dd++;
                    char miss[128]; int mo=0;
                    for (int q=0;q<(int)r->max_chunks && q<VID_MAX_CHUNKS && mo<110;q++)
                        if (!r->slot_seen[q]) mo+=snprintf(miss+mo,sizeof(miss)-mo,"%d ",q);
                    clog("[REORDER-DROP] sub=%d active=%d recv=%u/%u age_head=%lldms age_seen=%lldms missing_idx=[%s]",
                         g_emit_next_sub & 0xff, r->active, r->recv_count, r->max_chunks,
                         (long long)(now_ms - g_head_since_ms),
                         r->active ? (long long)(now_ms - r->first_seen_ms) : -1, miss);
                }
            }
            /* === G35 2026-08-22 — EMIT TRUNCATED RATHER THAN DROP ===
             * Dropping the head breaks the reference chain: the following
             * P-frames reference it and the decoder DRIFTS (fog over the whole
             * picture until the next key frame, which is rare). By emitting what
             * we have (truncated at the last complete NAL via vid_reasm_flush +
             * EMIT_TRUNCATED), the TOP slice stays a valid reference -> no more
             * global drift; only the bottom may float briefly, corrected by the
             * next complete picture. We also request an IDR to refresh. */
            ctx->stats->reasm_abandoned++;
            vid_reasm_flush(ctx, r);                 /* emit truncated (at a NAL) + clear */
            r->expect_sof = true;
            g_idr_needed = 1;
            g_emit_next_sub = (g_emit_next_sub + 1) & 0xff;
            g_head_since_ms = now_ms;
        } else {
            break;   /* head not ready but not expired either -> we wait */
        }
    }
}

/* LEGACY MODE (= what worked on run 15, ~28 fps with degraded quality).
 * The SUFP video wire still needed deep RE — the various hypotheses tried (6B
 * header, 11B, decrypt per packet, concat then decrypt) all failed except the
 * legacy `byte 10 == 0x01` filter. Legacy still decodes ~85 % of the packets
 * through that filter and emits partial frames. Picture = "first line clean +
 * the rest blurry".
 *
 * To RE properly: capture our own current UDP wire (= via the LD_PRELOAD hook on
 * halyard) then compare with the desktop app. */
/* Flush the display buffer (= the concatenation of all the NAL slices of one
 * display frame) to the H.264 decoder. */
static void flush_display_buffer(session_ctx_t *ctx) {
    if (g_display_off == 0) return;

    /* H1 V10: force-inject an H.264 Access Unit Delimiter (NAL type 9) at the
     * head of the buffer. An explicit signal to libavcodec that this is ONE
     * complete AU with potentially several slices (top + bottom). Without an AU
     * delimiter, libavcodec may "merge" top+bottom from different frames or try
     * to decode them as separate pictures -> bottom MB concealment.
     * primary_pic_type = 1 (P slice) -> payload = 0x30. SHADOW_AU_DELIM=0 to
     * disable. Default ON (= V10 test). */
    /* G29: the official client does NOT inject an AUD — it feeds the AU verbatim
     * and lets libavcodec detect the picture boundaries via first_mb_in_slice.
     * Default put back to 0 to be bit-for-bit like the official client.
     * SHADOW_AU_DELIM=1 to return to the injection (H1 V10). */
    static int g_au_delim = -1;
    if (g_au_delim < 0) {
        const char *e = getenv("SHADOW_AU_DELIM");
        g_au_delim = e ? atoi(e) : 0;
    }
    if (g_au_delim && g_display_off + 6 < VID_DISPLAY_BUF_SIZE) {
        /* Shift the existing buffer 6 bytes to insert the AU delimiter at the head. */
        memmove(g_display_buf + 6, g_display_buf, g_display_off);
        g_display_buf[0] = 0x00;
        g_display_buf[1] = 0x00;
        g_display_buf[2] = 0x00;
        g_display_buf[3] = 0x01;
        g_display_buf[4] = 0x09;  /* NAL type 9 = AU delimiter */
        g_display_buf[5] = g_display_keyframe ? 0x10 : 0x30;
        /* keyframe -> primary_pic_type=0 (I), else 1 (P) */
        g_display_off += 6;
    }

    uint64_t pts_ms = (uint64_t)g_display_ts / 90;
    /* === L5 2026-08-29 — THE CARRIER OF THE SERVER STAMP ===
     * `g_display_ts` is the timestamp the server put in the `VideoFrame` header.
     * It is the ONLY reference we share with it: published here, it is read back
     * by the glue in `on_video`, which is called just below, SYNCHRONOUSLY and on
     * the same thread — so no matching is needed at this link.
     * NOT to be confused with `pts_ms`: that one divides by 90 as if the field
     * were in 90 kHz ticks, whereas the measured gaps between pictures (median
     * 41,727 units for ~24 fps) point to microseconds. So we publish the RAW
     * field and the `latency` module verifies the unit by measuring the ratio of
     * the two clocks — rather than assuming it. */
    if (latency_enabled()) {
        const int64_t now_us = latency_now_us();
        latency_video_assembled(g_display_ts, now_us);
        /* L7 — the wait in the display buffer. The milestone is 0 as long as no
         * byte has been added since the last flush: we do not invent a sample in
         * that case. */
        if (g_display_last_us > 0)
            latency_add(LAT_VID_HOLD, now_us - g_display_last_us);
    }
    if (ctx->p->on_video) {
        ctx->p->on_video(g_display_buf, g_display_off, pts_ms,
                          g_display_keyframe, ctx->p->user);
    }
    ctx->stats->frames_decoded++;
    /* L10 — learning: how many slices did this picture have? Placed HERE because
     * it is the only place that sees a whole picture. */
    if (g_display_subchans_seen > 0) {
        if (g_display_subchans_seen == g_slices_seen) {
            if (g_slices_stable < L10_STABLE) g_slices_stable++;
        } else {
            g_slices_seen    = g_display_subchans_seen;
            g_slices_stable = 1;
        }
        g_slices_expected =
            (!g_l10_off && g_slices_stable >= L10_STABLE) ? g_slices_seen : 0;
    }

    g_display_off = 0;
    g_display_last_us = 0;   /* L7 */
    g_display_keyframe = false;
    g_display_subchans_seen = 0;
}

static void emit_legacy(session_ctx_t *ctx, uint8_t *plain, int ct_len) {
    /* === G10 — DO PICTURES ARRIVE TWICE? ===
     *
     * Every straggler is a duplicate (`dup=1255 lost=0`): so the server repeats
     * chunks, just as it repeats every audio frame. If the repetition covers
     * WHOLE pictures, we decode them twice — and replaying the residual of an
     * intermediate picture on top of itself produces exactly the blocks that
     * appear and disappear on motion, with no decoding error reported at all.
     *
     * So we compare every emitted picture to the previous one: same length and
     * same checksum = duplicate. */
    {
        static int      last_len = -1;
        static uint32_t last_sum = 0;
        uint32_t sum = 2166136261u;
        const int n_hash = ct_len < 4096 ? ct_len : 4096;
        for (int i = 0; i < n_hash; i++) sum = (sum ^ plain[i]) * 16777619u;
        if (ct_len == last_len && sum == last_sum) {
            ctx->stats->frames_duplicate++;
        }
        last_len = ct_len; last_sum = sum;
    }

    /* DEBUG: dump the raw bitstream (the whole plaintext before stripping) to
     * /tmp/shadow_dump.bin for offline analysis with ffprobe / ffmpeg. Gated by
     * SHADOW_DUMP_RAW=1 (= otherwise it pollutes /tmp on every run). */
    static int g_dump_raw = -1;
    if (g_dump_raw < 0) {
        const char *e = getenv("SHADOW_DUMP_RAW");
        g_dump_raw = e ? atoi(e) : 0;
    }
    if (g_dump_raw) {
        static FILE *fdump = NULL;
        static int g_dumped = 0;
        if (!fdump) fdump = fopen("/tmp/shadow_dump.bin", "wb");
        if (fdump && g_dumped < 2000) {
            uint32_t len = (uint32_t)ct_len;
            fwrite(&len, 4, 1, fdump);
            fwrite(plain, 1, ct_len, fdump);
            fflush(fdump);
            g_dumped++;
        }
    }
    /* === K15 — K13'S FALSIFIABLE PREDICTION ===
     * The `VideoFrame` header carries `byte0 = version<<4 | codec`. We measured
     * 0x02 on H.264; the client's INTERNAL enum gives H265 = 3, so an H.265
     * stream should carry 0x03. That has NEVER been observed. We log it once per
     * value: the first H.265 session will confirm or refute it, and both results
     * are worth writing down. */
    {
        static uint32_t seen = 0;
        const uint8_t b0 = (ct_len > 0) ? plain[0] : 0;
        if (b0 < 32 && !(seen & (1u << b0))) {
            seen |= (1u << b0);
            clog("[K15] VideoFrame header byte0 = 0x%02x (codec=%u version=%u)",
                 b0, (unsigned)(b0 >> 4), (unsigned)(b0 & 0x0f));
        }
    }
    /* === K15 — THE GUARD MUST NOT REJECT H.265 === [SUPERSEDED, see RE-3 below]
     * SUPERSEDED 2026-09-11: the nibble reading in this block is the wrong way
     * round. RE-3 read the official client's parser, and a live H.265 session
     * confirmed it (grant codec=1, byte 0 = 0x12 = codec 1, version 2). Kept as
     * the record of how the misreading happened; CFG-3 now takes the NAL grammar
     * from that codec nibble.
     * It demanded exactly 0x02. If the prediction were right, an H.265 stream
     * carries 0x03: the guard would then drop EVERY picture, and the defect would
     * look like "H.265 does not work" when it would only be a version byte.
     * MEASUREMENT OF 2026-08-28, AND IT REFUTES K13'S PREDICTION. We expected
     * 0x03 on H.265 (the CODEC nibble going from 2 to 3). That is false: the
     * server sends 0x12, i.e. **version = 1, codec = 2**. So it is not the codec
     * that moves but the frame format's VERSION — the codec nibble stays at 2 in
     * both cases, and does not distinguish the codecs.
     * So we accept versions 0 and 1. Refusing 1 dropped EVERY picture on H.265
     * (measured: normal video bitrate, zero picture displayed), and the defect
     * looked like "H.265 does not work".
     * The G42 guard keeps all its value against damaged headers. */
    /* === RE-3 2026-09-02 - THE NIBBLES WERE THE WRONG WAY ROUND, AND THE BINARY SETTLES IT ===
     *
     * Read straight out of the official client's own VideoFrame parser
     * (ShadowPCDisplay @0xbfebb0):
     *
     *     movzx eax,[rsi] ; mov edx,eax ; shr dl,4     <- the HIGH nibble
     *     cmp dl,1 -> [rdi+0x18] = 3                   <- H.265
     *     cmp dl,2 -> [rdi+0x18] = 4                   <- AV1
     *     test dl  -> [rdi+0x18] = 2                   <- H.264
     *     and eax,0xf ; mov [rdi+0x14],al              <- the LOW nibble: STORED, never compared
     *
     * and `[rdi+0x18]` is what picks the NAL grammar at @0xbfeb80: 2 gives
     * `nal[0] & 0x1F` (H.264), 3 gives `(nal[0] >> 1) & 0x3F` (HEVC).
     *
     * So HIGH = CODEC, LOW = VERSION. We had it the other way round, and it
     * survived because 0x02 reads the same under both: version 0 | codec 2 and
     * codec 0 | version 2 are the same byte. Only a second codec breaks the tie,
     * and every capture in the repo is H.264 — 53 830 VideoFrame records, byte 0
     * = 0x02 in all of them.
     *
     * The old guard `vf_ver <= 1` therefore passed H.264 (0x02) and H.265 (0x12)
     * BY COINCIDENCE and would have dropped 100 % of AV1 pictures (0x22).
     *
     * Why the census of 0x02/0x12/0x22/0x32 misled us (KB §3.46, now corrected):
     * those four are the OUTER STFP framing header, a different byte in a
     * different header. Two headers of the same shape are stacked on the TCP
     * channel and we censused the outer one while reasoning about the inner one.
     * The outer low nibble must be 2 (@0xd61030 rejects anything else) and its
     * HIGH nibble is a frame class: 0 empty, 1 key frame, 2 ordinary, 3 ping.
     *
     * The version check stays, and it is now the RIGHT nibble: 2 in every record
     * ever observed, on both transports and all three codecs. */
    const uint8_t vf_codec = (ct_len > 0) ? (uint8_t)(plain[0] >> 4)   : 0xFF;
    const uint8_t vf_ver   = (ct_len > 0) ? (uint8_t)(plain[0] & 0x0F) : 0;
    const bool vf_ok = (vf_ver == 2) && (vf_codec <= 2);
    if (ct_len < 8 || !vf_ok) {
        /* G42 diag: how many pictures emitted by the reassembly are dropped
         * here (invalid VideoFrame header) = a candidate for the frame_num
         * holes. */
        static uint32_t g_drop_magic = 0;
        g_drop_magic++;
        if (g_drop_magic <= 10 || (g_drop_magic % 20) == 0)
            clog("[G42] DROP magic: plain[0]=0x%02x ct_len=%d (total %u dropped)",
                 ct_len > 0 ? plain[0] : 0, ct_len, g_drop_magic);
        return;
    }
    /* === CFG-3 2026-09-11 — THE GRAMMAR OF *THIS* PICTURE ===
     * See g_hevc_nal. `hevc` drives G40's access-unit split and the NAL counters
     * below, and is left in g_hevc_nal for F11/G43. The version and codec-range
     * checks are the vf_ok guard just above: vid_wire_vf_is_hevc() only reads
     * the codec nibble, and is tested with its counter-case in
     * tests/test_vid_wire.c. */
    static int g_nal_from_stream = -1;
    if (g_nal_from_stream < 0) {
        const char *e = getenv("SHADOW_NAL_FROM_STREAM");
        g_nal_from_stream = e ? atoi(e) : 1;   /* 0 = SHADOW_CODEC, re-read per session */
    }
    const int hevc = g_nal_from_stream ? vid_wire_vf_is_hevc(plain[0]) : g_hevc_env;
    g_hevc_nal = hevc;
    /* The DECODER is still built from SHADOW_CODEC and cannot read the other
     * codec. A stream that differs from the request is therefore a session the
     * decoder cannot read, and this line is the only one that names the cause.
     * Once per session: the flag is reset by vid_reasm_reset_session. */
    if (!g_codec_noted && (int)vf_codec != g_codec_env) {
        g_codec_noted = 1;
        clog("[K16] stream codec %u != SHADOW_CODEC %d - NAL rules taken %s",
             (unsigned)vf_codec, g_codec_env,
             g_nal_from_stream ? "from the stream" : "from SHADOW_CODEC");
    }
    /* === K15b — SHOW THE HEADER RATHER THAN ASSUME IT ===
     * The HEVC decoder refuses the pictures ("Invalid data"), and the layout of
     * the VideoFrame header in VERSION 1 has never been observed: the one coded
     * here (magic+ts, flag, 13-byte extension if key frame) comes from version 0.
     * We dump the first bytes of one key frame and one ordinary frame, once each.
     * Two log lines beat a third hypothesis. */
    {
        static int seen_key = 0, seen_ord = 0;
        const int is_key = (ct_len > 5 && plain[5] != 0x00);
        if ((is_key && !seen_key) || (!is_key && !seen_ord)) {
            if (is_key) seen_key = 1; else seen_ord = 1;
            char hx[3 * 40 + 1]; int ho = 0;
            for (int i = 0; i < ct_len && i < 40; i++)
                ho += snprintf(hx + ho, sizeof(hx) - ho, "%02x ", plain[i]);
            clog("[K15b] VideoFrame %s len=%d: %s",
                 is_key ? "KEY" : "ordinary", ct_len, hx);
        }
    }

    uint32_t ts = (uint32_t)plain[1] | ((uint32_t)plain[2] << 8)
                | ((uint32_t)plain[3] << 16) | ((uint32_t)plain[4] << 24);
    /* H1 V10 fix: Shadow VideoFrame header layout:
     *   bytes 0..4 = magic + ts (5 bytes)
     *   byte 5     = flag (0 = P-frame, != 0 = keyframe)
     *   bytes 6..18 = ext13 IF keyframe (= 13-byte extension)
     *   bytes 6..  or 19..  = NAL data (= Annex-B start codes)
     * Scanning for `00 00 00 01` from byte 6 can hit a FALSE POSITIVE pattern
     * inside a keyframe's ext13 -> insufficient strip -> ext13 garbage included
     * as NAL -> massive decoder concealment.
     * Fix: force hdr_len = 19 for a keyframe before scanning. */
    bool key = (plain[5] != 0x00);
    int scan_start = key ? 19 : 6;
    int hdr_len = -1;
    for (int i = scan_start; i + 4 <= ct_len; i++) {
        if (plain[i] == 0x00 && plain[i+1] == 0x00
            && plain[i+2] == 0x00 && plain[i+3] == 0x01) {
            hdr_len = i; break;
        }
    }
    /* Fallback: if the scan misses (= no 4-byte start code after scan_start),
     * try from byte scan_start directly (= maybe there is no start code and the
     * NAL data begins right away). Otherwise return. */
    if (hdr_len < 0) {
        /* Try 3-byte start code 00 00 01 */
        for (int i = scan_start; i + 3 <= ct_len; i++) {
            if (plain[i] == 0x00 && plain[i+1] == 0x00 && plain[i+2] == 0x01) {
                hdr_len = i; break;
            }
        }
    }
    if (hdr_len < 0 || ct_len <= hdr_len) {
        static uint32_t g_hdr_fail = 0;
        g_hdr_fail++;
        if (g_hdr_fail <= 10 || (g_hdr_fail % 20) == 0)
            clog("[G42] DROP no-NAL: ct_len=%d key=%d byte5=%02x scan_from=%d (total %u)",
                 ct_len, (int)key, plain[5], scan_start, g_hdr_fail);
        return;
    }
    int nal_len = ct_len - hdr_len;
    uint8_t *nal_data = plain + hdr_len;

    /* === G40 2026-08-22 — ACCESS-UNIT BOUNDARY BY STRUCTURE, NOT BY TS ===
     *
     * Root cause of the residual fog (above all under HEAVY MOTION): the display
     * buffer joined slices by TIMESTAMP (`ts != g_display_ts`). That is correct
     * for joining the TOP slice (first_mb=0) and the BOTTOM slice (first_mb!=0)
     * of the same picture — they share the ts. BUT when TWO consecutive pictures
     * carry the SAME ts (server duplicate, or the same timestamp assigned by the
     * encoder to closely spaced pictures under heavy motion), their 4 slices
     * (2 top + 2 bottom) all accumulated in g_display_buf -> on_video received
     * 2 PICTURES -> `feed_annexb` made ONE access unit holding 2 pictures ->
     * libavcodec sees a second first_mb=0 slice in the same AU ->
     * `decode_slice_header error` + `Frame num change` -> a corrupted picture
     * that drifts. Proof: the 65 offending AUs captured (G39) ALL contain
     * exactly `P(mb0)+P(mb1760)+P(mb0)+P(mb1760)` = 2 pictures; and the same
     * stream, RE-SPLIT by ffmpeg offline (spec boundary = first_mb==0), decodes
     * with 0 errors.
     *
     * Fix: we flush as soon as a TOP slice (first_mb==0) arrives while a picture
     * is already in progress (the H.264 spec's access-unit boundary), in
     * addition to the ts change. Top+bottom of the same picture therefore stay
     * together, but two pictures never merge — even at an identical ts. */
    /* K15 — parsing HEVC with H.264's rules does not return an error, it returns
     * ALWAYS false — no access unit is closed and the picture never comes out.
     * CFG-3: the grammar is this picture's own (`hevc`, resolved after the vf_ok
     * guard), no longer SHADOW_CODEC frozen on the process's first session. */
    bool emit_starts_picture =
        vid_wire_starts_picture_codec(nal_data, (size_t)nal_len, hevc);
    static int g_au_by_struct = -1;
    if (g_au_by_struct < 0) {
        const char *e = getenv("SHADOW_AU_BY_TS");
        g_au_by_struct = (e && atoi(e)) ? 0 : 1;   /* =1 restores the old ts-only */
    }
    /* L10 — THE BET'S SELF-CHECK. If we have just flushed early, this slice must
     * open a picture. Otherwise it belonged to the one we flushed: the bet was
     * wrong, we cut it in two, and we will not try again this session. */
    if (g_early_flush) {
        g_early_flush = false;
        if (!emit_starts_picture) {
            g_l10_off = 1;
            g_slices_expected = 0;
            g_slices_stable   = 0;
            clog("[L10] early flush PROVEN WRONG: the next slice did not open a "
                 "picture (expected %d slice(s)). Mechanism switched off for the "
                 "session — one cut picture, no more.",
                 g_slices_seen);
        }
    }

    bool need_flush = g_display_off > 0 &&
                      ((g_au_by_struct && emit_starts_picture) || ts != g_display_ts);
    if (need_flush) {
        flush_display_buffer(ctx);
    }
    g_display_ts = ts;
    if (key) g_display_keyframe = true;
    g_display_subchans_seen++;

    /* Scan the NAL types for stats (top/bottom, IDR top/bottom, SPS/PPS counts).
     * Useful for diagnostics: `NAL top=X bot=Y` in the periodic log says whether
     * the server really is pushing the bottom slices. */
    {
        size_t pos = 0;
        while (pos + 5 < (size_t)nal_len) {
            int sc = 0;
            if (pos + 4 <= (size_t)nal_len &&
                nal_data[pos] == 0 && nal_data[pos+1] == 0 &&
                nal_data[pos+2] == 0 && nal_data[pos+3] == 1) sc = 4;
            else if (pos + 3 <= (size_t)nal_len &&
                     nal_data[pos] == 0 && nal_data[pos+1] == 0 &&
                     nal_data[pos+2] == 1) sc = 3;
            if (sc == 0) { pos++; continue; }
            /* K16 — same rule as elsewhere: in HEVC the type lives on six bits
             * starting at bit 1. These counters all stayed at ZERO on H.265,
             * which gave a silent diagnostic panel at the exact moment we needed
             * it. Mappings: SPS 7 -> 33, PPS 8 -> 34, slices 1 and 5 -> all types
             * 0 to 31, IDR 5 -> 16 to 21. */
            const uint8_t nt = hevc
                ? (uint8_t)((nal_data[pos + sc] >> 1) & 0x3F)
                : (uint8_t)(nal_data[pos + sc] & 0x1f);
            const bool is_sps = hevc ? (nt == 33) : (nt == 7);
            const bool is_pps = hevc ? (nt == 34) : (nt == 8);
            const bool is_vcl = hevc ? (nt <= 31) : (nt == 1 || nt == 5);
            const bool is_idr = hevc ? (nt >= 16 && nt <= 21) : (nt == 5);
            size_t end = nal_len;
            for (size_t s = pos + sc; s + 3 <= (size_t)nal_len; s++) {
                if (nal_data[s] == 0 && nal_data[s+1] == 0 &&
                    (nal_data[s+2] == 1 || (s+3 < (size_t)nal_len && nal_data[s+2] == 0 && nal_data[s+3] == 1))) {
                    end = s; break;
                }
            }
            if (is_sps) {
                ctx->stats->nal_sps++;
            } else if (is_pps) {
                ctx->stats->nal_pps++;
            } else if (is_vcl) {
                /* first_mb_in_slice exp-golomb: MSB=1 -> first_mb=0 (= top slice). */
                /* The first-slice flag follows the header: one byte in H.264,
                 * TWO in HEVC. Reading it in the wrong place would classify top
                 * and bottom slices at random. */
                bool first_mb_zero = vid_wire_first_mb_is_zero(
                    nal_data[pos + sc + (hevc ? 2 : 1)]);
                if (first_mb_zero) {
                    ctx->stats->nal_top++;
                    if (is_idr) ctx->stats->nal_idr_top++;
                } else {
                    ctx->stats->nal_bottom++;
                    if (is_idr) ctx->stats->nal_idr_bottom++;
                }
            }
            pos = end;
        }
    }

    /* Append the NAL bytes (= with their Annex-B start code) to the display buffer */
    if (g_display_off + (size_t)nal_len < VID_DISPLAY_BUF_SIZE) {
        memcpy(g_display_buf + g_display_off, nal_data, nal_len);
        g_display_off += nal_len;
        /* L7 — last byte received for this picture. Reset on every slice: what
         * matters is the END of the picture, not its start. */
        if (latency_enabled()) g_display_last_us = latency_now_us();

        /* L10 — the picture has its slice count: it is finished, so we show it
         * without waiting for the next one. */
        static int g_flush_udp = -1;
        if (g_flush_udp < 0) {
            const char *e = getenv("SHADOW_FLUSH_UDP");
            g_flush_udp = e ? atoi(e) : 1;
        }
        if (g_flush_udp && !g_l10_off && g_slices_expected > 0
            && g_display_subchans_seen >= g_slices_expected) {
            g_early_flush = true;
            flush_display_buffer(ctx);
        }
    }

    /* Note: frames_decoded is incremented in flush_display_buffer */
    (void)ctx;
}

/* Diagnostics for receiving a video chunk: logging the first byte, the
 * distribution of subchannels and announced sizes, binary dumps of the chunks
 * (SHADOW_DUMP_CHUNKS) and of their raw payload (SHADOW_DUMP_RAW_FULL).
 *
 * ALL off by default. Extracted from on_video_packet on 2026-08-25: they took up
 * a hundred lines in the middle of the hot path, between the header decode and
 * the chunk tracking, which made the real logic unreadable. None of them
 * produces anything the rest depends on — that is what makes the cut safe. */
static void vid_diag_chunk(const uint8_t *pkt, size_t n, uint8_t subchan,
                           uint16_t chunk_idx, uint16_t max_chunks, uint8_t flag10)
{

    /* DEBUG H1 V7: log byte0 (= type in the lower nibble + flag in the upper) + byte10. */
    uint8_t byte0 = pkt[0];
    uint8_t type_lo = byte0 & 0x0F;
    uint8_t flag_hi = byte0 >> 4;
    {
        static int g_idx_dbg = 0;
        if (g_idx_dbg < 200) {
            clog("[H1.V7 dbg] chunk: idx=%u max=%u byte0=%02x (type=%u flag=%u) "
                 "flag10=%u subchan=%u",
                 chunk_idx, max_chunks, byte0, type_lo, flag_hi, flag10, subchan);
            g_idx_dbg++;
        }
    }
    (void)type_lo; (void)flag_hi;  /* TODO V8: use flag_hi for true SoF detection */

    /* DEBUG: count ALL the subchans + max_chunks BEFORE any filtering */
    {
        static int g_pat_count = 0;
        static int g_subchan_counts[256] = {0};
        static int g_subchan_data[256] = {0};
        static int g_max_chunks_seen[64] = {0};
        g_subchan_counts[subchan]++;
        if (flag10 & 1) g_subchan_data[subchan]++;
        if (max_chunks < 64) g_max_chunks_seen[max_chunks]++;
        g_pat_count++;
        if (g_pat_count == 500 || g_pat_count == 5000 || g_pat_count == 20000) {
            clog("=== Distribution after %d pkts (= ALL subchans, no filter) ===", g_pat_count);
            for (int s = 0; s < 256; s++) {
                if (g_subchan_counts[s] > 0) {
                    clog("  subchan=%u: %d total (%d data byte10==1, %d parity)",
                         s, g_subchan_counts[s], g_subchan_data[s],
                         g_subchan_counts[s] - g_subchan_data[s]);
                }
            }
            clog("=== max_chunks distribution ===");
            for (int m = 0; m < 64; m++) {
                if (g_max_chunks_seen[m] > 0) {
                    clog("  max=%d: %d packets", m, g_max_chunks_seen[m]);
                }
            }
        }
    }

    /* RE 2026-05-14: NEW UNDERSTANDING.
     * Each subchan = ONE independent DISPLAY FRAME (= not a spatial slice).
     * subchan=0 = the initial keyframe (= max=24 chunks, 10 data + 14 parity).
     * subchan=N (N>=1) = the following P-frames (= max=12 chunks, 1-2 data +
     * 10-11 parity). To get the whole picture, process ALL subchans. No filter —
     * let them all through. */
    /* (KFCAP debug disabled after the capture) */

    /* DEBUG: dump parity + data chunks to /tmp/shadow_chunks.bin for offline
     * analysis (XOR-recovery experiments, etc.).
     * Format: [u32 size][u8 flag10][u8 subchan][u16 chunk_idx][u16 max_chunks][u32 frame_id][raw ct]
     * Gated by SHADOW_DUMP_CHUNKS=1. */
    static int g_dump_chunks = -1;
    if (g_dump_chunks < 0) {
        const char *e = getenv("SHADOW_DUMP_CHUNKS");
        g_dump_chunks = e ? atoi(e) : 0;
    }
    if (g_dump_chunks) {
        static FILE *fdump = NULL;
        static int g_chunk_dumped = 0;
        if (!fdump) fdump = fopen("/tmp/shadow_chunks.bin", "wb");
        if (fdump && g_chunk_dumped < 3000) {
            int ct_l = (int)n - 11 - 28;
            if (ct_l > 0 && ct_l < 2048) {
                /* frame_id from THIS packet's SUFP header bytes 6-9 (= not global) */
                uint32_t pkt_frame_id = (uint32_t)pkt[6] | ((uint32_t)pkt[7] << 8)
                                     | ((uint32_t)pkt[8] << 16) | ((uint32_t)pkt[9] << 24);
                uint32_t entry_size = 14 + (uint32_t)ct_l;
                fwrite(&entry_size, 4, 1, fdump);
                fwrite(&flag10, 1, 1, fdump);
                fwrite(&subchan, 1, 1, fdump);
                fwrite(&chunk_idx, 2, 1, fdump);
                fwrite(&max_chunks, 2, 1, fdump);
                fwrite(&pkt_frame_id, 4, 1, fdump);
                fwrite(pkt + 11, 1, ct_l, fdump);
                fflush(fdump);
                g_chunk_dumped++;
            }
        }
    }

    /* F27 2026-05-23: dump RAW chunks (= bytes 11..end, the full encrypted
     * payload including nonce+tag for data, raw for parity) for offline RS
     * brute-forcing.
     * Format: [u32 entry_size][u8 flag10][u16 chunk_idx][u16 max_chunks]
     *         [u32 frame_id][u8 subchan][u8 reserved][u8 raw_bytes[ct_len_full]] */
    static int g_dump_raw_full = -1;
    if (g_dump_raw_full < 0) {
        const char *e = getenv("SHADOW_DUMP_RAW_FULL");
        g_dump_raw_full = e ? atoi(e) : 0;
    }
    if (g_dump_raw_full) {
        static FILE *fraw = NULL;
        static int g_raw_dumped = 0;
        if (!fraw) fraw = fopen("./halyard-data/raw_chunks.bin", "wb");
        if (fraw && g_raw_dumped < 5000) {
            int rawlen = (int)n - 11;  /* full payload incl nonce+tag for data */
            if (rawlen > 0 && rawlen < 2048) {
                uint32_t pkt_frame_id = (uint32_t)pkt[6] | ((uint32_t)pkt[7] << 8)
                                     | ((uint32_t)pkt[8] << 16) | ((uint32_t)pkt[9] << 24);
                uint8_t reserved = 0;
                uint32_t entry_size = 14 + (uint32_t)rawlen;
                fwrite(&entry_size, 4, 1, fraw);
                fwrite(&flag10, 1, 1, fraw);
                fwrite(&chunk_idx, 2, 1, fraw);
                fwrite(&max_chunks, 2, 1, fraw);
                fwrite(&pkt_frame_id, 4, 1, fraw);
                fwrite(&subchan, 1, 1, fraw);
                fwrite(&reserved, 1, 1, fraw);
                fwrite(pkt + 11, 1, rawlen, fraw);
                fflush(fraw);
                g_raw_dumped++;
            }
        }
    }

}

/* Files ONE received chunk into its reassembly slot.
 *
 * Four cases, in this order: a self-contained chunk decrypted on the fly, a
 * continuation chunk kept in plaintext, a fallback to a late AEAD decryption,
 * and a raw fallback when the AEAD fails. Each one is commented in place — they
 * are the results of distinct campaigns (F23, RE4, RE8).
 *
 * Extracted from on_video_packet on 2026-08-25: 190 lines, a single
 * responsibility, no early return, eight input values. It writes only into `r`
 * and into `ctx->stats`'s counters. The two toggles that served only here
 * followed it. */
static void vid_store_chunk(session_ctx_t *ctx, vid_reasm_t *r,
                            const uint8_t *pkt, size_t n,
                            uint16_t chunk_idx, uint16_t max_chunks,
                            bool is_data, int ct_len_full)
{
    (void)max_chunks;
    /* "Concat then decrypt" mode: keep everything raw and decrypt only at flush
     * time. Default 0 = per-chunk decryption. SHADOW_REASSEMBLE_DECRYPT=1. */
    static int g_reasm_decrypt = -1;
    if (g_reasm_decrypt < 0) {
        const char *e = getenv("SHADOW_REASSEMBLE_DECRYPT");
        g_reasm_decrypt = e ? atoi(e) : 0;
    }


    /* === Decrypt the data chunks + store the parity chunks RAW (= no decrypt).
     * V11 VICTORY 2026-05-15: the "parity" chunks (byte10=0) in fact hold the
     * MAJORITY of the H.264 NAL bytes, as unencrypted plaintext. The
     * Reed-Solomon hypothesis is FALSE. Without including these chunks in the
     * NAL concatenation we lost ~90 % of the data -> ffprobe bytestream -7/-9 +
     * 5700 MB concealment errors on I-frames.
     * Default ON. SHADOW_PARITY_RAW=0 to return to V5 mode (decrypt only). */
    static int g_parity_raw = -1;
    if (g_parity_raw < 0) {
        const char *e = getenv("SHADOW_PARITY_RAW");
        g_parity_raw = e ? atoi(e) : 1;
    }
    /* F23 mode: store ALL chunks RAW (= no per-chunk decrypt), decrypt at flush */
    if (g_reasm_decrypt) {
        if (ct_len_full > 0 && ct_len_full <= 2048 && !r->chunks[chunk_idx]) {
            r->chunks[chunk_idx] = (uint8_t *)malloc(ct_len_full);
            if (r->chunks[chunk_idx]) {
                memcpy(r->chunks[chunk_idx], pkt + 11, ct_len_full);
                r->chunk_lens[chunk_idx] = ct_len_full;
                r->chunk_is_data[chunk_idx] = is_data;  /* track for flush logic */
                if (is_data) r->data_count++;
            }
        }
        goto f23_completion_check;
    }

    if (is_data) {
        int ct_len = ct_len_full - 28;
        if (ct_len > 0 && ct_len <= 2048 && !r->chunks[chunk_idx]) {
            uint8_t plain[2048];
            memcpy(plain, pkt + 11, ct_len);
            const uint8_t *nonce = pkt + n - 28;
            const uint8_t *tag   = pkt + n - 16;
            if (shadow_cipher_decrypt_unsafe(ctx->cipher, plain, ct_len, nonce, tag)) {
                ctx->stats->decrypt_ok++;
                r->chunks[chunk_idx] = (uint8_t *)malloc(ct_len);
                if (r->chunks[chunk_idx]) {
                    memcpy(r->chunks[chunk_idx], plain, ct_len);
                    r->chunk_lens[chunk_idx] = ct_len;
                    r->chunk_is_data[chunk_idx] = true;
                    r->data_count++;
                }
            } else {
                ctx->stats->decrypt_fail++;
            }
        }
    } else {
        ctx->stats->parity_skip++;

        /* RE8 2026-05-22 — attempted decryption of the "parity" chunks.
         *
         * Offline analysis of the 2026-05-19 baseline chunks.log:
         *   DATA   first-byte entropy = 1.69 bits (82 % = the 0x02 Shadow magic)
         *   PARITY first-byte entropy = 7.96 bits (uniform = random)
         *
         * -> PARITY chunks ARE NOT NAL plaintext (the V11 finding + agent #183
         *   refuted). But their structure (1269 B = 1241 B CT + 12 B nonce +
         *   16 B tag) is IDENTICAL to data. Hypothesis: the "parity" chunks are
         *   just encrypted data chunks flagged differently (= duplicate, FEC
         *   encoded, or continuation slice).
         *
         * Test: if the chacha20-poly1305 decrypt + tag verification passes ->
         * it really is data in the clear -> use the plaintext. Otherwise fall
         * back to RAW (= V11 legacy).
         *
         * === G22 2026-08-22 — DEFAULT PUT BACK TO 0 (RE8 CORRUPTED THE PICTURE) ===
         * Once G21 was in place (tail included), the animated wallpaper (an F1
         * car) was NOISY over its whole surface, with no ffmpeg error at all
         * (= valid but wrong bytes). Cause: this path calls
         * `shadow_cipher_decrypt_unsafe` = WITHOUT verifying the poly1305 tag.
         * On a continuation chunk (already in the clear) the "decrypt" therefore
         * verifies nothing, always "succeeds", and stores a WRONG plaintext 28 B
         * shorter than the correct raw bytes -> corrupted content that decodes
         * without error but displays as noise, and propagates across the GOP.
         * Proof: offline reconstruction (everything RAW) = a clean picture;
         * visual A/B with PARITY_DECRYPT=0 = a perfectly clean F1 car.
         * The continuation chunks are H.264 NAL IN THE CLEAR (V11): we must
         * NEVER try to decrypt them. SHADOW_PARITY_DECRYPT=1 restores the old
         * (buggy) behaviour. */
        static int g_parity_decrypt = -1;
        if (g_parity_decrypt < 0) {
            const char *e = getenv("SHADOW_PARITY_DECRYPT");
            g_parity_decrypt = e ? atoi(e) : 0;   /* G22: default 0 (= always RAW, correct) */
        }
        if (g_parity_decrypt && !r->chunks[chunk_idx]) {
            int ct_len = ct_len_full - 28;
            if (ct_len > 0 && ct_len <= 2048) {
                uint8_t plain[2048];
                /* F25 2026-05-23 11h00: try nonce = frame_id (bytes 6-9) +
                 * chunk_idx (bytes 2-3) + 6 B of zero. Hypothesis: for parity,
                 * the nonce is derived from the SUFP header (frame_id +
                 * chunk_idx). Tag = last 16 B. CT = bytes 11..n-17 (= 1253 B). */
                static int g_pn_mode = -1;
                if (g_pn_mode < 0) {
                    const char *e = getenv("SHADOW_PARITY_NONCE_MODE");
                    /* 0 = legacy last-28B, 1 = start, 2 = frame_id+idx-derived, 3 = SUFP header as nonce */
                    g_pn_mode = e ? atoi(e) : 0;
                }
                const uint8_t *nonce, *tag;
                uint8_t derived_nonce[12];
                if (g_pn_mode == 1) {
                    nonce = pkt + 11; tag = pkt + n - 16;
                    memcpy(plain, pkt + 11 + 12, ct_len);
                } else if (g_pn_mode == 2) {
                    /* nonce = [frame_id 4B | chunk_idx 2B | max_chunks 2B | byte0..byte1 2B | byte10 1B | 0 1B] */
                    memcpy(derived_nonce + 0, pkt + 6, 4);  /* frame_id */
                    memcpy(derived_nonce + 4, pkt + 2, 2);  /* chunk_idx */
                    memcpy(derived_nonce + 6, pkt + 4, 2);  /* max_chunks */
                    memcpy(derived_nonce + 8, pkt + 0, 2);  /* byte0+byte1 */
                    derived_nonce[10] = pkt[10];
                    derived_nonce[11] = 0;
                    nonce = derived_nonce; tag = pkt + n - 16;
                    int alt_ct = ct_len_full - 16;
                    if (alt_ct > 0 && alt_ct <= 2048) {
                        memcpy(plain, pkt + 11, alt_ct);
                        ct_len = alt_ct;
                    }
                } else if (g_pn_mode == 3) {
                    /* Use SUFP header bytes 0-10 as part of nonce */
                    memcpy(derived_nonce, pkt, 11);
                    derived_nonce[11] = 0;
                    nonce = derived_nonce; tag = pkt + n - 16;
                    int alt_ct = ct_len_full - 16;
                    if (alt_ct > 0 && alt_ct <= 2048) {
                        memcpy(plain, pkt + 11, alt_ct);
                        ct_len = alt_ct;
                    }
                } else {
                    nonce = pkt + n - 28; tag = pkt + n - 16;
                    memcpy(plain, pkt + 11, ct_len);
                }
                if (shadow_cipher_decrypt_unsafe(ctx->cipher, plain, ct_len, nonce, tag)) {
                    ctx->stats->parity_decrypt_ok++;
                    r->chunks[chunk_idx] = (uint8_t *)malloc(ct_len);
                    if (r->chunks[chunk_idx]) {
                        memcpy(r->chunks[chunk_idx], plain, ct_len);
                        r->chunk_lens[chunk_idx] = ct_len;
                        r->chunk_is_data[chunk_idx] = true;  /* it WAS data after all */
                        r->data_count++;
                    }
                    goto parity_done;
                } else {
                    ctx->stats->parity_decrypt_fail++;
                    /* fall through to the RAW V11 fallback */
                }
            }
        }

        if (g_parity_raw && !r->chunks[chunk_idx]) {
            /* RE4 2026-05-19 — BYTE-BY-BYTE FINDING:
             * Distribution in the chunk dump:
             *   data   chunks (post-decrypt) : len = 1241 B  (= 11 %)
             *   parity chunks (raw plaintext): len = 1269 B  (= 89 %)
             * Exact difference = 28 B = chacha20-poly1305 nonce(12) + tag(16).
             *
             * Hypothesis: the parity chunks occupy the SAME wire slot as data
             * (= the same 1280 B packet size). The server puts the last 28 bytes
             * in the SAME place as data's nonce+tag, but they are "tail" bytes
             * that are NOT valid NAL (= likely padding/zero/real RS parity).
             *
             * Test: trim the last 28 bytes of the parity chunks. If the
             * bytestream errors disappear or drop -> confirmed.
             * SHADOW_PARITY_TRIM=28 to enable (default 0 = V11 legacy). */
            /* RE4 2026-05-19 REVERT: default 0 (= V11 behaviour).
             * trim=28 cut the offline ffmpeg "bytestream -N" errors by 94 %, BUT
             * visually at runtime: the picture was ENTIRELY washed out
             * (concealment everywhere instead of localised taskbar artefacts).
             *
             * Conclusion: those last 28 bytes DO CONTAIN useful H.264 content
             * (= not the garbage padding we assumed). Without them the decoder
             * conceals massively over the whole picture. The ffmpeg "bytestream
             * -N" errors disappear because the slice looks shorter, but the
             * decoder fills in silently -> a general smear.
             *
             * Revised hypothesis: all 1269 bytes are valid NAL bytes. The real
             * 393 errors come from something else (= UDP packet loss + chunk
             * ordering edge cases + slice header reuse across frames).
             *
             * The env var stays for future experiments. Default 0. */
            static int g_parity_trim = -1;
            if (g_parity_trim < 0) {
                const char *e = getenv("SHADOW_PARITY_TRIM");
                g_parity_trim = e ? atoi(e) : 0;
            }
            int raw_len = ct_len_full - g_parity_trim;
            if (raw_len > 0 && raw_len <= 2048) {
                r->chunks[chunk_idx] = (uint8_t *)malloc(raw_len);
                if (r->chunks[chunk_idx]) {
                    memcpy(r->chunks[chunk_idx], pkt + 11, raw_len);
                    r->chunk_lens[chunk_idx] = raw_len;
                    r->chunk_is_data[chunk_idx] = false;
                }
            }
        }
parity_done: ;
    }

f23_completion_check:
}

/* === S34 2026-08-25 — RESETTING BETWEEN TWO SESSIONS ===
 *
 * This module keeps its state at file scope: slices being reassembled, subchan
 * sequencing, last emitted identifier. Nothing reset it when one stream stopped
 * and another started.
 *
 * Two consequences, both measured:
 *  - the slices still active at close kept their chunk buffers allocated, to be
 *    overwritten without being freed by the next session — one leak per session,
 *    on a console with no memory to spare;
 *  - the subchan sequencing restarted with the previous session's value, which
 *    the gap detection (G36) read as a skipped picture from the very first
 *    packet.
 *
 * Same family as S34's black-screen defect (KB.md §3.28): session state kept
 * outside the session. Call at the start of every stream. */
void vid_reasm_reset_session(void)
{
    for (int i = 0; i < VID_MAX_SUBCHANS; i++) {
        vid_reasm_clear(&g_vid_reasm[i]);   /* frees the chunk buffers */
        /* V11: vid_reasm_clear does NOT touch the memory of the last emitted
         * picture — that is intended between two pictures, never between two
         * sessions. */
        g_vid_reasm[i].emitted_max      = 0;
        g_vid_reasm[i].emitted_complete = false;
        g_vid_reasm[i].emitted_ord      = 0;
    }
    g_emit_last_sub   = -1;
    g_emit_next_sub   = -1;
    g_head_since_ms   = 0;
    g_last_max        = 0;
    g_flushed_fid     = 0;
    g_flush_ord       = 0;   /* V11: the age of another session's memory means nothing */
    g_pic_ord         = 0;   /* REASM-1 - same reason, for the G43 age guard */
    g_g43_single_logs = 0;   /* REASM-1 - the [G43] line budgets are per session */
    g_g43_lost_logs   = 0;
    g_display_ts      = 0;
    g_last_frame_id   = 0;
    g_delay_offset_us = 0;
    g_vid_short_pkts  = 0;
    g_vid_total_pkts  = 0;
    g_sub_gaps        = 0;   /* CONC-4 - the [G42] budget is per session */
    g_idr_needed      = 0;   /* the caller asks for one again right after */

    /* The assembly buffer for the picture in progress: without this purge, the
     * new session's first picture inherited the tail of the previous session's
     * last picture, along with its timestamp and its key-frame flag. The array
     * itself does not need clearing — only the offset says what is valid. */
    g_display_off      = 0;
    g_slices_seen      = 0;   /* L10 — relearn: the resolution, the codec and
                                 therefore the slice count may have changed
                                 between two sessions. */
    /* CFG-3 2026-09-11 — the NAL grammar is resolved HERE, eagerly, every
     * session: never lazily (the -1 sentinel was truthy) and never carried over
     * from the process's first session. SHADOW_CODEC is read exactly as the
     * decoder reads it; the latch starts on what was asked for, then each
     * picture's own header takes over (emit_legacy). */
    {
        const char *ec = getenv("SHADOW_CODEC");
        g_codec_env = ec ? atoi(ec) : 0;
        if (g_codec_env < 0) g_codec_env = 0;
    }
    g_hevc_env    = (g_codec_env == 1);
    g_hevc_nal    = g_hevc_env;
    g_codec_noted = 0;
    g_slices_stable    = 0;
    g_slices_expected  = 0;
    g_early_flush      = false;
    g_l10_off          = 0;
    g_display_last_us  = 0;   /* L7 — otherwise the new session's first picture
                                 would be measured from the old session's last
                                 one: a wait of several seconds, of the same kind
                                 that has already falsified four counters here. */
    g_display_keyframe      = false;
    g_display_subchans_seen = 0;
    g_last_valid            = false;
}

/* === K15j 2026-08-29 — THE SAME EMISSION PATH FOR UDP AND TCP ===
 *
 * `emit_legacy` takes the COMPLETE plaintext of a picture: the `VideoFrame`
 * header (`[0x02][stamp u32][flags]`, plus a 13-byte extension on a key frame)
 * followed by the Annex-B. It applies duplicate detection, header stripping,
 * access-unit aggregation (G40) and the hand-off to the decoder.
 *
 * The first capture in the `reliability` profile shows that **the payload of a
 * video STFP frame carries exactly that same format** (K15d): the ten-byte STFP
 * header stacks on top of it, it does not replace it. So the emission path is
 * shareable as-is, and that is what reduced the port from ~500 lines to a few
 * dozen.
 *
 * We expose it here rather than copy it: these 150 lines carry four campaigns of
 * fixes (G40, G43, deduplication, aggregation) and a copy would diverge at the
 * very next fix. */
void vid_reasm_emit_payload(void *user, uint8_t *payload, int len)
{
    session_ctx_t *ctx = (session_ctx_t *)user;
    if (!ctx || ctx->magic != SESSION_CTX_MAGIC || !payload || len <= 0) return;
    emit_legacy(ctx, payload, len);

    /* === L8 2026-08-29 — DO NOT WAIT FOR THE NEXT PICTURE TO SHOW THE ONE
     *     WE ALREADY HOLD ===
     *
     * `emit_legacy` accumulates into `g_display_buf` and flushes ONLY when a top
     * slice arrives while a picture is already sleeping there (G40). That is
     * right on UDP, where we do not know whether the picture is finished:
     * nothing in the stream says so, only the arrival of the next one proves it.
     *
     * On TCP, we DO know. The STFP framing delimits the message, and one message
     * carries a whole picture — verified over a nine-minute session: 28,678
     * pictures decoded for 28,679 messages, and `NAL top` equal to the picture
     * count. Waiting for the next one therefore teaches us nothing: it is pure
     * dead time, the picture is complete and on nobody's screen.
     *
     * What that dead time costs, measured (L7):
     *
     *     console, 720p HEVC, TCP, 60 fps: p50 = 17.4 ms  p90 = 20.5
     *     desktop, 1080p H264, UDP, 46 fps: p50 = 25.6 ms  p90 = 30.7
     *
     * It is worth exactly ONE INTER-PICTURE INTERVAL — 17.4 at 60 fps, 22.5 at
     * 46. On the 47.6 ms console chain, that is 37 % of the total and the largest
     * item, ahead of the draw cadence.
     *
     * === WHAT MAKES THIS FIX SAFE, AND WHY IT GOES NO FURTHER ===
     *
     * An automated review had proposed flushing at the end of `emit_legacy`,
     * unconditionally. That was WRONG, and measured as such: on UDP at 1080p the
     * server sends TWO slices per picture (`top=1279 bottom=1279`, i.e. 50.0 %),
     * each arriving through a separate call — flushing between the two would cut
     * the access unit down the middle and reopen G40, the blur under heavy
     * motion.
     *
     * Here the flush sits on `vid_reasm_emit_payload`, the entry point of the
     * TCP path ONLY, called once per complete picture. The two slices of one
     * picture, if there are two, are already in the same message and therefore in
     * the same buffer before we flush. The UDP path is untouched: it keeps
     * waiting, for want of knowing.
     *
     * `SHADOW_FLUSH_TCP=0` restores the wait. */
    static int g_flush_tcp = -1;
    if (g_flush_tcp < 0) {
        const char *e = getenv("SHADOW_FLUSH_TCP");
        g_flush_tcp = e ? atoi(e) : 1;
    }
    if (g_flush_tcp) flush_display_buffer(ctx);
}

/* === REASM-1 2026-09-11 - THE G43 TRIGGER, ON BOTH OPENING PATHS ===
 *
 * G43 states its trigger as "at the opening of picture N, flush N-1 if it is
 * still incomplete": the server is done with N-1, and vid_reasm_flush() then
 * emits it truncated (tail loss) instead of letting it die in its buffer. The
 * code ran that trigger on ONE opening path only - chunk 0 of a MULTI-chunk
 * picture. A single-chunk picture ("case A" in on_video_packet) was decrypted,
 * emitted and returned before reaching it: K19's one-path defect again (K19
 * fixed it for the gap detector only). With the reorder buffer off, the
 * default, nothing else flushes N-1. Reproduced with the real vid_reasm.c
 * (picture 40 loses its tail, 41 is single-chunk):
 *   - the tail's second copy lands after 41: emission order 38 39 41 40 42,
 *     three [G42] GAP lines, the decoder fed 41 before the picture it refers to;
 *   - both tail copies lost: 40 is NEVER emitted and in NO counter
 *     (chunks_missing = 0, chunks_orphan_lost = 0) - `perdus` undercounts;
 *   - the buffer outlives its picture: a lap later (~256 pictures, ~6 s at
 *     45 fps) the next multi-chunk opening on subchannel 41 flushed it, and an
 *     ancient P picture went into the decoder.
 * Both paths now call this function. Two guards keep it from opening a
 * resurrection path of its own (a flush-previous-only fix did, measured):
 *   - AGE: a predecessor opened more than SHADOW_FLUSH_PREV_MAX_AGE openings ago
 *     (default 8; 0 = no guard) is a previous lap's leftover: counted and wiped,
 *     never emitted. A real predecessor is 0 openings old, a few under
 *     reordering; a leftover is ~250.
 *   - OWN SUBCHANNEL: a single-chunk picture also wipes what a previous lap left
 *     in its own subchannel's buffer, as the multi-chunk opening always did.
 * A discarded picture is COUNTED, and only counted: vid_count_unemitted().
 * SHADOW_FLUSH_PREV_INCOMPLETE=0 restores the previous behaviour of both paths
 * exactly, counters included. Offline: tests/test_vid_reasm.c, which runs with
 * the toggle at 1 and at 0. */
static int vid_flush_prev_on(void)
{
    static int g_flush_prev = -1;   /* read-once toggle cache, not session state */
    if (g_flush_prev < 0) {
        const char *e = getenv("SHADOW_FLUSH_PREV_INCOMPLETE");
        g_flush_prev = e ? atoi(e) : 1;
    }
    return g_flush_prev;
}

static int vid_flush_prev_max_age(void)
{
    static int g_max_age = -1;      /* read-once toggle cache, not session state */
    if (g_max_age < 0) {
        const char *e = getenv("SHADOW_FLUSH_PREV_MAX_AGE");
        g_max_age = e ? atoi(e) : 8;
    }
    return g_max_age;
}

/* A picture opened and never finished: what the trigger looks for. */
static bool vid_reasm_incomplete(const vid_reasm_t *r)
{
    return r->active && r->recv_count > 0 && r->recv_count < r->max_chunks;
}

/* COUNTERS ONLY, for an incomplete picture thrown away without a flush: the
 * three counters vid_account_loss() feeds, so `perdus` stops missing it, and
 * nothing else. No key-frame request: the picture is typically a lap old, and
 * the review variant that called vid_account_loss() here raised one ~5.7 s
 * after the loss - the phantom key-frame family (G5/G37). No retransmission
 * request: an `rG` carries no picture identifier, so the server would read it
 * against its CURRENT picture (V10). No write to g_last_slots or g_flushed_fid,
 * which describe the last FLUSHED picture. Caller checks vid_reasm_incomplete. */
static void vid_count_unemitted(session_ctx_t *ctx, const vid_reasm_t *r)
{
    const uint32_t missing = (uint32_t)(r->max_chunks - r->recv_count);
    ctx->stats->chunks_expected += r->max_chunks;
    ctx->stats->chunks_missing  += missing;
    if (missing == 1) ctx->stats->frames_miss1++;
    if (g_g43_lost_logs < 5) {
        g_g43_lost_logs++;
        clog("[G43] picture never emitted, counted lost: subchannel %d, %u/%u chunks, "
             "age %u ouvertures", (int)(r - g_vid_reasm), r->recv_count, r->max_chunks,
             g_pic_ord - r->open_pic);
    }
}

/* Called at EVERY picture opening, before the opening is counted in g_pic_ord.
 * `single_chunk` only picks the log line: the plan is to measure on console how
 * often a single-chunk picture is what closes an incomplete one. */
static void vid_flush_prev_incomplete(session_ctx_t *ctx, int this_sub, bool single_chunk)
{
    if (!vid_flush_prev_on()) return;
    vid_reasm_t *pr = &g_vid_reasm[(this_sub - 1) & 0xff];
    if (!vid_reasm_incomplete(pr)) return;
    const int max_age = vid_flush_prev_max_age();
    if (max_age > 0 && g_pic_ord - pr->open_pic > (uint32_t)max_age) {
        vid_count_unemitted(ctx, pr);   /* a previous lap's leftover: never emit it */
        vid_reasm_clear(pr);
        return;
    }
    if (single_chunk && g_g43_single_logs < 5) {
        g_g43_single_logs++;
        clog("[G43] previous picture flushed by a single-chunk one: subchannel %d, "
             "%u/%u morceaux", (this_sub - 1) & 0xff, pr->recv_count, pr->max_chunks);
    }
    vid_reasm_flush(ctx, pr);   /* emits truncated on tail loss, else drops+IDR */
}

void on_video_packet(const uint8_t *pkt, size_t n, void *user) {
    /* Raw arrival sequence. The aggregate counters led to three wrong
     * hypotheses; this shows the real order of the chunks, with no intermediary
     * and no interpretation. */
    if (n >= 11 && shadow_diag_reasm()) {
        static int g_seq_dbg = 0;
        if (g_seq_dbg < 40) {
            g_seq_dbg++;
            const uint16_t di = (uint16_t)pkt[2] | ((uint16_t)pkt[3] << 8);
            const uint16_t dm = (uint16_t)pkt[4] | ((uint16_t)pkt[5] << 8);
            clog("[SEQ] sub=%u idx=%3u max=%3u flag=%02x len=%zu",
                 pkt[1], di, dm, pkt[10], n);
        }
    }

    /* V9 — how many packets did the old `n >= 10` guard let through to the clock
     * computation when they are not video chunks at all? */
    g_vid_total_pkts++;
    if (n >= 10 && n <= 39) {
        g_vid_short_pkts++;
        if (g_vid_short_pkts <= 5 || (g_vid_short_pkts % 200) == 0)
            clog("[V9] short packet on :base+10: len=%zu (%u of %u) — "
                 "poisoned the feedback clock before V9",
                 n, g_vid_short_pkts, g_vid_total_pkts);
    }

    session_ctx_t *ctx = (session_ctx_t *)user;
    if (!ctx || ctx->magic != SESSION_CTX_MAGIC) return;   /* S37: see ctrl_session_int.h */
    ctx->stats->udp_video_pkts++;
    ctx->stats->udp_video_bytes += n;

    /* DEBUG: dump every chunk of ONE complete frame. We lock onto the first
     * frame_id received (= bytes 6-9 of the first packet), then dump every
     * packet matching that frame_id (= everything needed to rebuild 1 frame). */
    static uint32_t g_target_frame_id = 0;
    static int g_dumped = 0;
    if (n >= 11) {
        uint32_t this_fid = (uint32_t)pkt[6] | ((uint32_t)pkt[7] << 8)
                          | ((uint32_t)pkt[8] << 16) | ((uint32_t)pkt[9] << 24);
        if (g_target_frame_id == 0 && this_fid != 0 && pkt[2] == 0 && pkt[3] == 0) {
            /* lock onto the first frame we see with chunk_idx=0 */
            g_target_frame_id = this_fid;
        }
        if (this_fid == g_target_frame_id && g_dumped < 30 && n <= 4096) {
            char buf[8200];
            for (size_t i = 0; i < n; i++) snprintf(buf + i*2, 4, "%02x", pkt[i]);
            clog("DEBUG_FRAME fid=0x%08x pkt#%d len=%zu hex=%s",
                 this_fid, g_dumped, n, buf);
            g_dumped++;
        }
    }
    /* === V11 2026-08-28 — REVERT TOGGLE FOR THE SHORT BOUND ===
     * `vid_wire_parse_hdr` now accepts a 12-byte continuation (a picture's TAIL,
     * in the clear, with neither nonce nor seal). Setting SHADOW_SHORT_TAIL=0
     * restores the old path identically: the single `n <= 39` rejection applied
     * here, BEFORE the header is read, hence also before the V9 feedback clock
     * and before any counter — exactly where it used to happen. The vid_wire
     * module stays pure (see vid_wire.h): the toggle lives with the caller, like
     * every other one.
     * Default 1: the AEAD bound applies only to AEAD chunks. */
    {
        static int g_short_tail = -1;
        if (g_short_tail < 0) {
            const char *e = getenv("SHADOW_SHORT_TAIL");
            g_short_tail = e ? atoi(e) : 1;
        }
        if (!g_short_tail && n <= VID_WIRE_HDR_LEN + VID_WIRE_AEAD_OVERHEAD)
            return;
    }

    /* Header decoding: vid_wire.c, a pure function verified offline by
     * tests/test_vid_wire.c (that is where the G4/G21 semantics live). */
    vid_wire_hdr_t wh;
    if (!vid_wire_parse_hdr(pkt, n, &wh)) return;
    uint8_t  flag10     = wh.flags;
    uint8_t  subchan    = wh.subchan;
    uint16_t chunk_idx  = wh.chunk_idx;
    uint16_t max_chunks;

    /* frame_id (bytes 6-9) for the gE feedback ACKs. */
    g_last_frame_id = wh.frame_id;

    /* === V9 2026-08-28 — THE CLOCK LOCKED ONTO ANY PACKET AT ALL ===
     * Bytes 6-9 are a send timestamp in microseconds. The offset computation was
     * guarded by `n >= 10` ALONE, hence BEFORE the header validation: any short
     * beacon arriving on `:base+10` (a bare 11-byte header passes `n >= 10` but
     * is rejected by `vid_wire_parse_hdr`, which required n > 39) had its bytes
     * 6-9 read as a timestamp.
     * That is not harmless: `g_delay_offset_us` goes straight into the `gE`
     * congestion-feedback message (ctrl_session.c:1918), on which the SERVER
     * sets its bitrate. An absurd value therefore makes it take an absurd
     * decision — bounded to +/-1 s, which is still enormous.
     * Now computed on a VALID header, and on that alone.
     * Known and uncorrected: the field is a u32 of microseconds, it wraps every
     * ~71.6 min; the wrap produces ONE absurd gap, which the caller's +/-1 s
     * bound reduces to a single wrong measurement. */
    {
        struct timespec dts;
        clock_gettime(CLOCK_MONOTONIC, &dts);
        const int64_t now_us = (int64_t)dts.tv_sec * 1000000 + dts.tv_nsec / 1000;
        g_delay_offset_us = now_us - (int64_t)wh.frame_id;
        g_delay_valid = 1;
    }

    /* ===================================================================
     * G4 FIX 2026-06-02 — OFF-BY-ONE: max_chunks (bytes 4-5) = the INDEX of the
     * LAST chunk (0-based), NOT a count. The server sends idx 0..max (= max+1
     * chunks). The old `chunk_idx >= max_chunks` guard SILENTLY dropped the last
     * chunk (idx==max, flag10=0 = the tail of the bottom slice) on EVERY frame
     * -> a bottom slice one chunk short (~3 MB rows) -> a CABAC error at the
     * bottom that propagates across the GOP through the P-frames -> an
     * unreadable bottom half under motion.
     *
     * PROOF (capture 2026-06-02 webrtc.log): `[DIAG] REJECT idx==max` on EVERY
     * subchan (idx=11/max=11, idx=4/max=4, idx=6/max=6, ...), always with
     * flag10=0. Offline decoding confirmed it: the bottom slice decodes rows
     * 34->64 then misses the last ~3 rows = exactly 1 chunk.
     *
     * Fix: the real count = max_chunks + 1. SHADOW_MAX_COUNT_PLUS1=0 to return
     * to the old behaviour (count semantics, V0).
     * =================================================================== */
    {
        /* === G21 2026-08-22 — G20 WAS A REGRESSION: `max` IS THE LAST INDEX,
         * NOT A COUNT (back to G4) ===
         *
         * G20 had concluded that `max` was a count (chunk_idx 0..max-1) from a
         * measurement taken via the [H1.V7 dbg] log. That measurement was
         * FALSIFIED: the log sits AFTER the `chunk_idx >= max_chunks` guard
         * (rejection around line 1273), so the idx==max chunk had already been
         * dropped and never appeared in the log -> the illusion that `max` is
         * never reached.
         *
         * DIRECT PROOF (raw wire dump /tmp/wire.bin, 2026-08-22): a picture with
         * `max_declared=120` carries chunks idx 0..120 = 121 chunks. Idx 120 (a
         * short tail, ~489 B, flag10=0) is even sent TWICE (redundancy of the
         * bottom slice's tail). Offline reconstruction taking ALL received idx =
         * 0 ffmpeg errors; the G20 client truncated every picture by its last
         * chunk (the tail of the BOTTOM slice) -> 500-600 CABAC errors at the
         * bottom of the picture, propagating across the GOP.
         * Measured A/B: G20 = 626 errors, G4 (below) = 0 errors.
         *
         * So the field is the LAST INDEX: real count = max + 1. We restore G4's
         * +1. SHADOW_MAX_COUNT_PLUS1=0 returns to G20's behaviour (buggy:
         * truncated bottom tail). */
        static int g_max_plus1 = -1;
        if (g_max_plus1 < 0) {
            const char *e = getenv("SHADOW_MAX_COUNT_PLUS1");
            g_max_plus1 = e ? atoi(e) : 1;   /* G21: default 1 (= max is the last index) */
        }
        /* wh.chunk_count already carries the +1. The toggle at 0 restores the
         * count semantics (G20, buggy: truncated bottom tail). */
        max_chunks = (g_max_plus1 && wh.chunk_count <= VID_MAX_CHUNKS)
                   ? wh.chunk_count : wh.max_field;
    }

    /* DIAG: after the +1, this log must NEVER fire again (= confirms the fix).
     * If it still fires, the server is sending 2+ extra chunks -> investigate. */
    if (chunk_idx >= max_chunks && max_chunks > 0 && max_chunks <= VID_MAX_CHUNKS) {
        static int g_rej_dbg = 0;
        if (g_rej_dbg < 30) {
            clog("[DIAG] REJECT chunk idx=%u >= max=%u (flag10=%u byte0=%02x sub=%u)",
                 chunk_idx, max_chunks, flag10, pkt[0], subchan);
            g_rej_dbg++;
        }
    }
    /* `subchan` is a uint8_t: the VID_MAX_SUBCHANS (256) bound is structurally
     * unreachable, no point testing it. */
    if (max_chunks == 0
        || max_chunks > VID_MAX_CHUNKS || chunk_idx >= max_chunks) return;

    vid_diag_chunk(pkt, n, subchan, chunk_idx, max_chunks, flag10);

    /* H1 V5 — REFACTOR: track every chunk (data+parity) in the slot_seen[]
     * bitmap, decrypt+store only the data ones. Lets recv_count reach max_chunks
     * and the G.1 fast path fire even when the LAST chunk is parity.
     *
     * 0-indexed wire with count semantics, confirmed empirically:
     *   - subchan=2 max=4 -> 4 chunks total (1 data + 3 parity) -> V0 correct
     *   - chunk_idx in [0, max_chunks - 1]
     *   - last chunk = chunk_idx == max_chunks - 1 */

    bool is_data = (flag10 == 0x01);
    int ct_len_full = (int)n - 11;     /* everything after the 11 B SUFP header */
    if (ct_len_full <= 0) return;

    /* F23 2026-05-23 10h00: CONCAT-THEN-DECRYPT MODE, per the RE doc
     * ENCRYPTION_AND_FRAMING.md.
     *
     * The original Ghidra RE doc (2026-05-02) says clearly:
     *   "[ SUFP chunk header | encrypted_chunk_payload ]   <- per-datagram
     *      v  reassembly side, accumulates chunks for frame_id
     *    [ encrypted_full_payload = cipher_text || nonce_12 || tag_16 ]   <- per-frame
     *      v  CryptoCipherOpenSsl::Decrypt
     *    [ plain payload ]"
     *
     * So: decrypt ONLY after complete reassembly. byte10 = an "is_last" bit, NOT
     * a data/parity flag. Our per-chunk-decrypt code is wrong for multi-chunk
     * frames.
     *
     * Implementation: store ALL chunks RAW (= no per-chunk decrypt). At flush
     * time: concatenate every chunk, take the last 12 B = nonce, the 16 B before
     * = tag, decrypt the rest with chacha20-poly1305.
     *
     * Enable with SHADOW_REASSEMBLE_DECRYPT=1.
     * Default 0 = legacy behaviour (= per-chunk decrypt). */

    /* Case A: single-chunk data -> emit directly (= bypass reassembly). */
    if (max_chunks == 1) {
        /* REASM-1 2026-09-11 - this IS a picture opening, and it now does what
         * the multi-chunk opening does, in the same order: flush the incomplete
         * predecessor (the G43 trigger), then wipe what a previous lap left in
         * this subchannel's buffer - which nothing else would ever reach, since
         * this path never uses the buffer. Before the flag and decryption
         * checks, like the multi-chunk trigger. See vid_flush_prev_incomplete(). */
        vid_flush_prev_incomplete(ctx, (int)subchan, true);
        if (vid_flush_prev_on() && vid_reasm_incomplete(&g_vid_reasm[subchan])) {
            vid_count_unemitted(ctx, &g_vid_reasm[subchan]);
            vid_reasm_clear(&g_vid_reasm[subchan]);
        }
        g_pic_ord++;   /* REASM-1: an opening, for the age guard */
        if (!is_data) { ctx->stats->parity_skip++; return; }
        int ct_len = ct_len_full - 28;
        if (ct_len <= 0 || ct_len > 2048) return;
        uint8_t plain[2048];
        memcpy(plain, pkt + 11, ct_len);
        const uint8_t *nonce = pkt + n - 28;
        const uint8_t *tag   = pkt + n - 16;
        if (!shadow_cipher_decrypt_unsafe(ctx->cipher, plain, ct_len, nonce, tag)) {
            ctx->stats->decrypt_fail++;
            return;
        }
        ctx->stats->decrypt_ok++;
        /* K19 — DECLARE the emission, as the flush does. Without this call this
         * picture does not exist for the gap detector, and that is what
         * manufactured key frames in a loop. */
        vid_note_emitted_sub(ctx, (int)subchan);
        emit_legacy(ctx, plain, ct_len);
        return;
    }

    /* Case B: multi-chunk -> state machine + slot_seen tracking. */
    vid_reasm_t *r = &g_vid_reasm[subchan];

    /* === G24 — REORDER BUFFER path (default). We activate the picture on its
     * subchan, the storage block below files the chunk (dedup per slot, any
     * order), then vid_drain_ordered emits in sequence order. All the old
     * control machinery (expect_sof / SoF / g_pending / G9c / backwards
     * rejection / max mismatch) is bypassed: it emitted in completion order and
     * dropped pictures with a late chunk too early. */
    const int64_t vp_now = vid_now_ms();
    const int vp_reorder = vid_reorder_enabled();

    /* === G28 2026-08-22 — CLEAN PER-SUBCHAN REASSEMBLY, FOR BOTH MODES ===
     * The old machinery (SoF / g_pending / pending-wait / G9c / backwards
     * rejection / mismatch) was a stack of patches which, with measurement to
     * back it, MISROUTED the tail (idx=max) of ~1 % of pictures: the tail
     * arrives after the start of the next picture, g_pending filed it elsewhere,
     * and the picture stayed cut short of its tail until its subchan was reused
     * (256 pictures later) -> abandon -> artefact, even though the wire HAD
     * delivered the tail (wire: 0 pictures missing only their tail). But the
     * PER-SUBCHAN buffers solve the problem natively: picture S's late tail
     * falls back into g_vid_reasm[S] (its own buffer) and completes it. So we
     * replace the whole machinery with this clean logic (identical to the
     * reorder path):
     *   - SoF (idx==0)          -> a fresh clean buffer (wipes the stale state);
     *   - non-SoF on an emitted/empty buffer (expect_sof) -> straggler/duplicate -> ignore;
     *   - non-SoF with a different max -> a chunk from another picture -> ignore;
     *   - otherwise             -> a legitimate chunk (even reordered) -> store.
     * Emission still depends on the mode: reorder = ordered drain; default =
     * emission at completion (below). SHADOW_LEGACY_REASM=1 restores the old
     * machinery (debug). */
    if (vp_reorder && g_emit_next_sub < 0) { g_emit_next_sub = subchan; g_head_since_ms = vp_now; }

    /* Per-subchannel reassembly (G28): a SoF opens a fresh picture; a chunk with
     * no picture to join, or with an inconsistent size, is ignored. */
    if (chunk_idx == 0) {
        /* G43 (trigger): the arrival of THIS picture's SoF means the server is
         * done with the PREVIOUS one (subchan-1). If that one stayed incomplete
         * (tail lost), we flush it NOW, in order, before starting the new one —
         * vid_reasm_flush will emit it truncated (see G43 tail_loss) instead of
         * letting it die when the subchan is reused (256 pictures later) = the
         * frame_num hole that caused the fog. With no loss, the previous one has
         * already been emitted (active=false) -> the flush is a no-op.
         * REASM-1 2026-09-11: the block moved into vid_flush_prev_incomplete(),
         * shared with the single-chunk path, which used to skip it; it gained an
         * age guard on the way. */
        vid_flush_prev_incomplete(ctx, (int)subchan, false);
        /* REASM-1: a picture still open in THIS buffer was never emitted - the
         * stream moved past it, typically a lap ago. It used to be wiped here in
         * silence; it is now counted (counters only) before the wipe. */
        if (vid_flush_prev_on() && vid_reasm_incomplete(r))
            vid_count_unemitted(ctx, r);
        vid_reasm_clear(r);
        r->active = true;
        r->max_chunks = max_chunks;
        r->open_pic = ++g_pic_ord;   /* REASM-1: the age guard's clock */
        r->highest_idx = 0;
        r->first_seen_ms = vp_now;
        r->first_seen_us = latency_enabled() ? latency_now_us() : 0;  /* L5 */
        r->expect_sof = false;
    } else if (!r->active || r->expect_sof) {
        /* === V11 2026-08-28 — A CATCH-ALL COUNTER MEASURES NOTHING ===
         * `orph` sat at ~4,000 per 90 s session (one per picture) and nobody
         * could say whether that was bad: its two sub-counters, declared and
         * displayed for months, were incremented NOWHERE. So one read
         * `orph=4113(dup=0 lost=0)` and concluded — wrongly — that the orphans
         * came from elsewhere. Measurement: the server resends the last chunk of
         * every picture ~3 ms after the original, which makes ONE harmless
         * orphan per picture; the REST is data from pictures whose opening
         * (idx 0) was lost, and those pictures appear in NO loss counter, since
         * this `return` precedes all accounting. See vid_wire_orphan_is_dup() for
         * the criterion and its measurement. SHADOW_ORPHAN_CLASSIFY=0 restores
         * the previous display (single counter, sub-counters at zero) without
         * changing anything else: no chunk is kept or dropped differently. */
        ctx->stats->chunks_orphan++;
        if (vid_orphan_classify()) {
            if (vid_wire_orphan_is_dup(max_chunks, r->emitted_max,
                                       r->emitted_complete,
                                       g_flush_ord - r->emitted_ord))
                ctx->stats->chunks_orphan_dup++;
            else
                ctx->stats->chunks_orphan_lost++;
        }
        return;
    } else if (r->max_chunks != max_chunks) {
        /* A third cause, unrelated to the other two: a chunk on the SAME
         * subchannel, on a still-open picture, with a different count. Measured
         * over 159,461 chunks of the official client: 34 times, i.e. 0.7 % of the
         * orphans, and always on a buffer more than 2 s old (the subchannel
         * reused 256 pictures later). The rejection is correct; confusing it with
         * the other two was not. */
        ctx->stats->chunks_orphan++;
        if (vid_orphan_classify()) ctx->stats->chunks_orphan_stale++;
        return;
    }


    /* === Track slot occupation (data + parity) === */
    if (chunk_idx >= VID_MAX_CHUNKS) return;
    if (!r->slot_seen[chunk_idx]) {
        r->slot_seen[chunk_idx] = true;
        r->recv_count++;
        if (chunk_idx > r->highest_idx) r->highest_idx = chunk_idx;
    } else {
        /* === V10b 2026-08-28 — THE ONLY DIRECT PROOF OF A RETRANSMISSION ===
         * A chunk arriving for a slot ALREADY filled on the picture in progress
         * can only be a network duplicate or an answer to our `rG`. So it is the
         * only counter that answers "does the server reply to our retransmission
         * requests?" without assuming the answer. It did not exist: we measured
         * the loss, never the repair. */
        ctx->stats->chunks_redundant++;
    }
    /* Filing the chunk — see vid_store_chunk(). */
    vid_store_chunk(ctx, r, pkt, n, chunk_idx, max_chunks, is_data, ct_len_full);

    /* === Completion checks ===
     *
     * F9 2026-05-22 22h45: REMOVED the premature flush on got_last_chunk.
     * Before: if chunk_idx == max-1 arrived (in any order) -> flush, even with
     * recv_count < max_chunks. Bug: UDP reordering can make the last chunk
     * arrive BEFORE the middle ones -> flush too early -> the middle chunks are
     * dropped (slot cleared) -> a broken picture even though the chunks do
     * eventually arrive.
     *
     * Now: we flush ONLY when recv_count == max_chunks (= genuinely complete).
     * To handle frames that never complete (= real UDP loss), a timeout-based
     * flush is triggered by a new SoF arriving on the same subchan OR by a
     * periodic sweep (F10, to be implemented if needed).
     *
     * Set SHADOW_FAST_FLUSH=1 to return to the legacy behaviour (= flush on
     * got_last_chunk). */
    bool got_last_chunk = (chunk_idx == max_chunks - 1);
    if (got_last_chunk) ctx->stats->got_last_chunk++;
    /* V10: the last chunk is here and some are still missing — the hole is
     * CERTAIN, and the server is still on this picture. It is the only moment
     * when an `rG` (which carries no picture identifier) designates what we
     * think it designates. See vid_nack_enqueue(). */
    {
        static int g_nack_at_gap = -1;
        if (g_nack_at_gap < 0) {
            const char *e = getenv("SHADOW_NACK_AT_GAP");
            g_nack_at_gap = e ? atoi(e) : 1;
        }
        if (g_nack_at_gap && got_last_chunk && r->recv_count < r->max_chunks)
            vid_nack_enqueue(ctx, r);
    }
    static int g_fast_flush = -1;
    if (g_fast_flush < 0) {
        const char *e = getenv("SHADOW_FAST_FLUSH");
        g_fast_flush = e ? atoi(e) : 0;
    }
    if (vp_reorder) {
        /* G24: the emission decision is taken by the ordered drain, which emits
         * in subchan order and holds the head for up to ~300 ms. */
        vid_drain_ordered(ctx, vp_now, 0);  /* never time out while processing */
    } else if (r->recv_count >= r->max_chunks) {
        vid_reasm_flush(ctx, r);
    } else if (g_fast_flush && got_last_chunk) {
        ctx->stats->incomplete_at_flush++;
        vid_reasm_flush(ctx, r);
    }
}

