/* vid_reasm — reassembly of the video pictures received on :base+10.
 *
 * Extracted from ctrl_session.c on 2026-08-25: ~1900 lines in the middle of a
 * 4263-line file, where most of the picture fixes were concentrated (G4, G21,
 * G28, G36, G40, G43...). A dependency analysis showed that this block calls
 * NOTHING from the rest of the session: the extraction is therefore
 * behaviour-identical, and the file becomes readable again.
 *
 * Contents: SUFP header, chacha20 decryption, per-subchannel chunk tracking,
 * Annex-B emission to the decoder.
 */
#pragma once

#include "ctrl_session_int.h"

#include <stdbool.h>
#include <stdint.h>

/* Entry point: one datagram received on :base+10. */
/* Resets the module's whole state (slices, sequencing, counters). Call at the
 * start of EVERY session — see the comment in the .c. */
void vid_reasm_reset_session(void);

void on_video_packet(const uint8_t *pkt, size_t n, void *user);

/* K15j — emits a video payload that is ALREADY reassembled, as it arrives on a
 * TCP STFP channel: `VideoFrame` header then Annex-B, with no SUFP header and no
 * application-level encryption. Shares the whole emission path with UDP —
 * deduplication, header stripping, access-unit aggregation, hand-off to the
 * decoder. `user` is the session context. */
void vid_reasm_emit_payload(void *user, uint8_t *payload, int len);

/* Ordered drain (reorder buffer, opt-in via SHADOW_REORDER_BUFFER). */
void vid_drain_ordered(session_ctx_t *ctx, int64_t now_ms, int socket_drained);

/* Monotonic clock in ms, shared with the session loop. */
int64_t vid_now_ms(void);

/* State observed or driven by the session loop. */
extern volatile int      g_idr_needed;      /* a key frame is being requested */
extern volatile uint32_t g_last_frame_id;
extern int               g_emit_next_sub;
extern int               g_emit_last_sub;
extern int64_t           g_head_since_ms;
extern int64_t           g_delay_offset_us; /* last clock offset observed */
extern int               g_delay_valid;
extern uint32_t          g_vid_short_pkts;  /* V9: short packets received on :base+10 */
extern uint32_t          g_vid_total_pkts;
