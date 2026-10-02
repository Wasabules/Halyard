// SUFP — Shadow UDP Framed Protocol.
//
// Fragmentation/reassembly layer of the CURSOR channel (`:base+30`), as a
// FALLBACK: single-datagram packets are handled directly by
// ctrl_session.c::on_cursor_packet, and only the multi-chunk ones land here.
//
// VIDEO NO LONGER USES THIS MODULE. Since 2026-05-09 it parses its header inline
// in vid_reasm.c; the video reassembler that stayed allocated here was removed
// on 2026-08-25 (3.07 MiB per session for nothing).
//
// Wire format (chunk type 3) — 11 bytes:
//   [byte0]   version:4 | pkt_type:4         (HIGH nibble = version)
//   [byte1]   logical subchannel
//   [2..3]    chunk_idx (uint16 LE)          **0-based** (N58)
//   [4..5]    max          (uint16 LE)       **LAST INDEX, not a count**: the
//                                            server sends 0..max, i.e. max+1
//                                            chunks (G4/G21, then S32 for this
//                                            channel)
//   [6..9]    frame_id (uint32 LE)           NOT a counter: the sender's
//                                           timestamp in MICROSECONDS (V9
//                                           2026-08-28, and SRV1-AB measured
//                                           it running across sessions). It is
//                                           echoed back in gE field 3, where
//                                           the server computes now_us - it.
//   [byte10]  flags                          bit 0 = SELF-CONTAINED chunk,
//                                            encrypted `[ct][nonce 12][tag 16]`.
//                                            bit 0 clear = plaintext
//                                            continuation, to be concatenated
//                                            as-is. It is NOT a "last chunk"
//                                            marker, and these are not parity:
//                                            there is no FEC in this protocol
//                                            (F31, KB §3.15).
//   [11..]    payload
//
// Reassembly: a window of SUFP_WINDOW_SIZE concurrent pictures. A picture is
// complete once it has received its max+1 chunks; they are concatenated in
// ascending index order. A full window evicts the oldest.
//
// BEWARE THE OLD DESCRIPTIONS. This header long described the pre-2026-05-09
// video model — 1-based index, `max` read as a total, bit 0 read as "last
// chunk", FEC parity. All four were wrong, and each cost an RE campaign.
// Corrected on 2026-08-25.
//
// Tests: tests/test_sufp.c (33 checks).
//
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUFP_HDR_TYPE3      11    /* modern chunks */
#define SUFP_HDR_LEGACY     10    /* type 1/2 */
#define SUFP_PKT_TYPE_DATA1   1
#define SUFP_PKT_TYPE_DATA2   2
#define SUFP_PKT_TYPE_DATA3   3
#define SUFP_PKT_TYPE_PING  0xf

#define SUFP_WINDOW_SIZE     256  /* concurrent frames — wide, for 1080p multi-slice */
#define SUFP_MAX_CHUNKS_PER_FRAME 256  /* 1080p high-profile keyframes can have 200+ chunks */
#define SUFP_MAX_FRAME_BYTES (2 * 1024 * 1024)  /* 2 MiB — keyframes 1080p high profile */

typedef struct sufp_reasm sufp_reasm;

/* Called when a complete frame has been reassembled.
 * `buf` is valid until the callback returns (reused afterwards).
 * `subchan` = byte 1 of the chunk header.
 */
typedef void (*sufp_frame_cb)(uint8_t subchan, uint32_t frame_id,
                                const uint8_t *buf, size_t len, void *user);

/* Creates a reassembler. cb may be NULL (frames are then dropped silently). */
sufp_reasm *sufp_create(sufp_frame_cb cb, void *user);
void sufp_destroy(sufp_reasm *r);

/* Feed one raw UDP packet as received. Returns:
 *   1 = a frame was completed (the callback was called)
 *   0 = chunk inserted, still waiting
 *  -1 = invalid packet / dropped
 *   2 = ping packet (no frame; the caller may handle the RTT)
 */
int sufp_feed(sufp_reasm *r, const uint8_t *buf, size_t len);

/* Builds a type 3 chunk ready to send over UDP.
 *   out_buf   : size >= SUFP_HDR_TYPE3 + payload_len
 *   payload, payload_len : the data (typically already encrypted)
 * Returns the number of bytes written into out_buf, or -1 on error.
 */
int sufp_build_chunk_v3(uint8_t *out_buf, size_t out_cap,
                          uint8_t version, uint8_t subchan,
                          uint16_t chunk_idx, uint16_t max_chunks,
                          uint32_t frame_id, uint8_t flags,
                          const uint8_t *payload, size_t payload_len);

/* Builds a ping packet (type 0xf). Used to measure the RTT.
 * Layout: [0xf0][subchan][seq_uint32_le][timestamp_us_uint32_le] = 10 bytes.
 */
int sufp_build_ping(uint8_t *out_buf, size_t out_cap,
                     uint8_t subchan, uint32_t seq, uint32_t timestamp_us);

#ifdef __cplusplus
}
#endif
