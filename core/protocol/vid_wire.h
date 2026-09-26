/* vid_wire.h — the semantics of the Shadow video wire, as PURE functions.
 *
 * This file isolates the three protocol rules that each cost a full RE
 * campaign:
 *
 *   G4/G21 — the `max` field of the SUFP header is the LAST INDEX, not a count.
 *            Getting it wrong costs the last chunk of every picture (the tail of
 *            the bottom slice): 626 ffmpeg errors against 0.
 *   G40    — an H.264 access unit starts at a `first_mb == 0` slice. Trusting
 *            the timestamp instead merges two pictures that share one.
 *   G43    — a picture missing only its TAIL is better emitted truncated than
 *            dropped: dropping it breaks the reference chain and makes the
 *            decoder drift until the next key frame.
 *
 * They are pure — no state, no I/O, no logging, no getenv — so they can be
 * verified offline by tests/test_vid_wire.c, with no console and no virtual
 * machine. The SHADOW_* toggles stay with the callers: this module says what the
 * protocol MEANS, not what we choose to do about it.
 */
#ifndef VID_WIRE_H
#define VID_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VID_WIRE_HDR_LEN 11
/* nonce (12) + tag (16) of a self-contained chacha20-poly1305 chunk. */
#define VID_WIRE_AEAD_OVERHEAD 28

typedef struct {
    uint8_t  subchan;
    uint16_t chunk_idx;    /* 0-based */
    uint16_t max_field;    /* raw bytes 4-5, as they are on the wire */
    uint16_t chunk_count;  /* = max_field + 1  (G4/G21) */
    uint32_t frame_id;
    uint8_t  flags;
    bool     is_aead;      /* flags & 1: self-contained encrypted chunk */
} vid_wire_hdr_t;

/* Decodes the 11-byte SUFP v3 header. Returns false when the packet is too
 * short to carry a usable chunk. */
bool vid_wire_parse_hdr(const uint8_t *pkt, size_t n, vid_wire_hdr_t *out);

/* first_mb_in_slice is exp-golomb coded: the value 0 is written as `1` on a
 * single bit, so the top bit of the first byte after the NAL header is 1 if and
 * only if first_mb is 0. */
bool vid_wire_first_mb_is_zero(uint8_t byte_after_nal_hdr);

/* true when the Annex-B stream starts a new picture, i.e. when its FIRST VCL
 * slice (type 1 or 5) carries first_mb == 0. Leading non-VCL NALs (SPS, PPS,
 * SEI) are walked through. */
bool vid_wire_starts_picture(const uint8_t *annexb, size_t len);

/* Same question for an HEVC stream. Its NAL header is TWO bytes, the type lives
 * on six bits starting at bit 1, and ALL types from 0 to 31 are picture slices.
 * See the K15 block in vid_wire.c. */
bool vid_wire_starts_picture_hevc(const uint8_t *annexb, size_t len);

/* Dispatch: a non-zero `hevc` selects the HEVC variant. Made explicit rather
 * than guessed, so the caller SAYS which stream it is handling. */
bool vid_wire_starts_picture_codec(const uint8_t *annexb, size_t len, int hevc);

/* CFG-3 — which NAL grammar ONE picture uses, read from byte 0 of its own
 * VideoFrame header: the HIGH nibble is the codec (0 H.264, 1 H.265, 2 AV1) and
 * the low nibble the format version, always 2 (RE-3, [C95+]: read out of the
 * official client's parser @0xbfebb0, which picks its NAL grammar from exactly
 * this nibble). Returns 1 for H.265 and 0 otherwise, ready for the `hevc`
 * argument above. Validating the version and the codec range stays with the
 * caller. */
int vid_wire_vf_is_hevc(uint8_t vf_byte0);

/* === CLASSIFYING AN ORPHAN — see vid_wire.c for the measurement ===
 *
 * A chunk arriving with no picture to join is dropped. A single counter did not
 * say WHICH of the two cases it was, and the two have nothing in common:
 *   - the SERVER re-sends the last chunk of every picture ~3 ms after the
 *     original; it arrives after the flush, hence with no picture to join.
 *     Nothing is lost, the data is already filed and already emitted.
 *   - the OPENING of a picture (idx 0) was lost; no buffer is opened and ALL of
 *     its following chunks land here. That is real data, and the whole picture
 *     vanishes from the loss accounting.
 *
 * The criterion: if the last picture emitted ON THIS SUBCHANNEL had the same
 * chunk count and was COMPLETE, then all of its indices were received, so this
 * chunk must be a second copy. The recency window is indispensable: a subchannel
 * is reused 256 pictures later, and on a stream where the count barely varies
 * (measured: 17,379 chunks out of 19,259 at `count == 10`) equality of counts
 * alone would make a picture with a lost head look like a duplicate.
 *
 * Measured over 4 captures of the official client (13,942 pictures, 4,792
 * orphans, ground truth established by payload comparison):
 *   `chunk_idx == count-1` alone: 8.3 % error, including 206 real losses
 *                                 labelled "harmless duplicate";
 *   this criterion without window: 5.9 % error, 281 real losses hidden;
 *   this criterion with the window: 0.02 % error, ZERO loss hidden.
 * The direction of the error matters more than its rate: a loss read as a
 * duplicate is a defect you stop looking for. */
#define VID_WIRE_ORPHAN_WINDOW 4   /* pictures; 1 and 2 let 24 duplicates through, 4 lets 1 */

bool vid_wire_orphan_is_dup(uint16_t chunk_count, uint16_t emitted_count,
                            bool emitted_complete, uint32_t frames_since_emit);

/* true when an incomplete picture is incomplete ONLY by its tail, hence
 * emittable truncated (G43). Requires: received chunks all contiguous from 0, at
 * most three missing, and no IDR — an incomplete IDR is still dropped, because a
 * corrupted key frame poisons every reference that follows. */
bool vid_wire_is_tail_loss(unsigned recv_count, unsigned chunk_count,
                           int last_contiguous, bool has_idr);

#endif /* VID_WIRE_H */
