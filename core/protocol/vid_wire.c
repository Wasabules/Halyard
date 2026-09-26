/* vid_wire.c — see vid_wire.h. Pure functions, zero dependencies. */
#include "vid_wire.h"

bool vid_wire_parse_hdr(const uint8_t *pkt, size_t n, vid_wire_hdr_t *out)
{
    if (!pkt || !out) return false;

    /* === V11 2026-08-28 — THE BOUND DEPENDS ON THE FLAG, AND IT WAS EATING
     * PICTURE TAILS ===
     *
     * The old bound demanded `n > 11 + 28` of EVERY packet, assuming each one
     * carries a nonce and a seal. That is false: only an AEAD chunk (b10 & 1)
     * carries them. A CONTINUATION chunk (b10 & 1 == 0) is plaintext H.264 NAL
     * with no overhead at all — its legitimate minimum is ONE useful byte, hence
     * n > 11.
     *
     * The comment that justified the old bound claimed "the wire has never shown
     * a continuation that short". The wire shows them constantly: across four
     * plaintext captures of the OFFICIAL client, 98 packets of 12 to 39 bytes
     * arrive on :base+10, and all 98 carry `chunk_idx == max_field` (the LAST
     * chunk) with `b10 == 0`. No exception. Full example
     * (captures_states_20260821_031020, t=...619.205):
     *
     *   23 88 38 00 38 00 9d e0 4f a4 00 | be 1d 29 e6 dc 44 4b 7f 80
     *   ^ver/type ^sub ^idx=56 ^max=56  ^flag=0   9 bytes of tail
     *
     * And the server re-sends that tail ~3 ms later, identically: BOTH copies
     * were dropped. The loss was therefore CERTAIN, not probabilistic — hence a
     * truncated picture every time the tail is short, followed by a key-frame
     * request (G26).
     *
     * Measured cost of the old bound: 2.0 to 2.7 % of pictures at 5-13 Mbit/s
     * (`frames_miss1`), and on the 5-6 Mbit/s sessions `chunks_missing` equalled
     * `frames_miss1` EXACTLY (51/51, 39/39, 53/53, 66/66...) — in other words
     * 100 % of our measured "UDP loss" was this guard. Consequence: ~1.1 key
     * frames requested per second (idr_t=46 over 41 s) against 0.06/s on a
     * session without short tails (g56_type_3: lost=0, idr_t=2 over 35 s).
     *
     * The tail's length is a division remainder: it depends on the content, not
     * on the network. Its measured distribution gives 0.06 % to 2.6 % of tails
     * under 29 bytes depending on the scene.
     *
     * Revert toggle: SHADOW_SHORT_TAIL=0, on the caller's side (vid_reasm.c),
     * which restores the single bound n > 39 BEFORE this call. This module stays
     * pure (no getenv) — that is its contract, see vid_wire.h. */
    if (n <= VID_WIRE_HDR_LEN) return false;          /* bare header = beacon */
    if ((pkt[10] & 1) != 0 &&
        n <= VID_WIRE_HDR_LEN + VID_WIRE_AEAD_OVERHEAD) return false;

    out->subchan   = pkt[1];
    out->chunk_idx = (uint16_t)pkt[2] | ((uint16_t)pkt[3] << 8);
    out->max_field = (uint16_t)pkt[4] | ((uint16_t)pkt[5] << 8);
    out->frame_id  = (uint32_t)pkt[6]        | ((uint32_t)pkt[7] << 8)
                   | ((uint32_t)pkt[8] << 16) | ((uint32_t)pkt[9] << 24);
    out->flags     = pkt[10];
    out->is_aead   = (pkt[10] & 1) != 0;

    /* G4/G21: the field is the last index, so the count is field + 1. True ALSO
     * for field == 0: a single-chunk picture. The old code made the +1
     * conditional on `field > 0`, leaving a count of 0 that the emission loop
     * never recognises as complete (it requires count > 0). */
    out->chunk_count = (uint16_t)(out->max_field + 1);
    return true;
}

bool vid_wire_first_mb_is_zero(uint8_t byte_after_nal_hdr)
{
    return (byte_after_nal_hdr & 0x80) != 0;
}

/* === K15 2026-08-27 — HEVC DOES NOT HAVE THE SAME NAL HEADER ===
 *
 * H.264: ONE-byte header. `nal_type = b0 & 0x1F`; picture slices are types 1
 * (non-IDR) and 5 (IDR); `first_mb_in_slice` is the first element of the body,
 * hence the top bit of the NEXT byte.
 *
 * HEVC: TWO-byte header. `nal_type = (b0 >> 1) & 0x3F`; picture slices are ALL
 * types from 0 to 31 (against two values in H.264);
 * `first_slice_segment_in_pic_flag` is the first bit of the body, hence the top
 * bit of the SECOND byte after the header.
 *
 * Confusing the two does not produce an error but a picture that never gets
 * split: an HEVC header byte read the H.264 way gives an arbitrary type, almost
 * never 1 or 5, so `starts_picture` would always return false and no access unit
 * would ever be closed. */
bool vid_wire_starts_picture_hevc(const uint8_t *annexb, size_t len)
{
    if (!annexb) return false;
    for (size_t i = 0; i + 5 < len; i++) {
        size_t sc = 0;
        if (annexb[i] == 0 && annexb[i+1] == 0 &&
            annexb[i+2] == 0 && annexb[i+3] == 1) sc = 4;
        else if (annexb[i] == 0 && annexb[i+1] == 0 && annexb[i+2] == 1) sc = 3;
        if (!sc) continue;
        const uint8_t nt = (uint8_t)((annexb[i + sc] >> 1) & 0x3F);
        if (nt <= 31)                        /* first VCL slice encountered */
            return vid_wire_first_mb_is_zero(annexb[i + sc + 2]);
        i += sc - 1;
    }
    return false;                            /* no VCL slice */
}

bool vid_wire_starts_picture_codec(const uint8_t *annexb, size_t len, int hevc)
{
    return hevc ? vid_wire_starts_picture_hevc(annexb, len)
                : vid_wire_starts_picture(annexb, len);
}

/* See vid_wire.h. The nibble order is the whole point: 0x12 is "H.265, version
 * 2". Read the other way round - the reading this repo held until RE-3 - it is
 * "codec 2, version 1", and every H.265 picture would get the H.264 grammar. */
int vid_wire_vf_is_hevc(uint8_t vf_byte0)
{
    return (vf_byte0 >> 4) == 1;
}

bool vid_wire_starts_picture(const uint8_t *annexb, size_t len)
{
    if (!annexb) return false;
    for (size_t i = 0; i + 4 < len; i++) {
        size_t sc = 0;
        if (annexb[i] == 0 && annexb[i+1] == 0 &&
            annexb[i+2] == 0 && annexb[i+3] == 1) sc = 4;
        else if (annexb[i] == 0 && annexb[i+1] == 0 && annexb[i+2] == 1) sc = 3;
        if (!sc) continue;
        uint8_t nt = annexb[i + sc] & 0x1f;
        if (nt == 1 || nt == 5)              /* first VCL slice encountered */
            return vid_wire_first_mb_is_zero(annexb[i + sc + 1]);
        i += sc - 1;
    }
    return false;                            /* no VCL slice */
}

/* See the comment block in vid_wire.h. Pure: the caller supplies the
 * subchannel's state, this function only reads it. */
bool vid_wire_orphan_is_dup(uint16_t chunk_count, uint16_t emitted_count,
                            bool emitted_complete, uint32_t frames_since_emit)
{
    if (!emitted_complete) return false;   /* picture emitted truncated: unknown */
    if (emitted_count == 0) return false;  /* nothing was ever emitted here */
    if (chunk_count != emitted_count) return false;      /* another picture */
    return frames_since_emit <= VID_WIRE_ORPHAN_WINDOW;  /* else: subchannel reused */
}

bool vid_wire_is_tail_loss(unsigned recv_count, unsigned chunk_count,
                           int last_contiguous, bool has_idr)
{
    if (has_idr) return false;               /* G43: never on an IDR */
    if (chunk_count == 0) return false;
    if (recv_count >= chunk_count) return false;   /* picture complete */
    if (last_contiguous + 1 != (int)recv_count) return false;  /* hole in the middle */
    return recv_count + 3 >= chunk_count;    /* only the tail is missing */
}
