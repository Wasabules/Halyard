/* test_vid_wire.c — offline verification of the video wire's semantics.
 *
 * Every rule is tested WITH its counter-case: the precise input that broke the
 * picture in production. A test failing here names the RE campaign whose result
 * it protects, so a future session knows what it has just undone.
 *
 * Build: see tests/run_tests.sh — no console, no VM, no network.
 */
#include <stdio.h>
#include <string.h>
#include "../core/protocol/vid_wire.h"

static int total = 0, failed = 0;

static void check(bool cond, const char *campaign, const char *what)
{
    total++;
    if (!cond) { failed++; printf("  FAIL  [%s] %s\n", campaign, what); }
}

/* Builds a SUFP v3 header followed by enough payload to be valid. */
static size_t hdr(uint8_t *b, uint8_t sub, uint16_t idx, uint16_t max,
                  uint32_t fid, uint8_t flags)
{
    memset(b, 0xAA, 128);
    b[0] = 0x30; b[1] = sub;
    b[2] = idx & 0xff;  b[3] = idx >> 8;
    b[4] = max & 0xff;  b[5] = max >> 8;
    b[6] = fid & 0xff;  b[7] = (fid >> 8) & 0xff;
    b[8] = (fid >> 16) & 0xff; b[9] = (fid >> 24) & 0xff;
    b[10] = flags;
    return 128;
}

static void test_hdr(void)
{
    uint8_t b[128]; vid_wire_hdr_t h;

    /* --- G4/G21: `max` is the LAST INDEX, the count is max+1. --- */
    size_t n = hdr(b, 3, 0, 120, 0xDEADBEEF, 1);
    check(vid_wire_parse_hdr(b, n, &h), "G4", "valid header accepted");
    check(h.chunk_count == 121, "G4", "max=120 gives 121 chunks, not 120");
    check(h.max_field == 120, "G4", "raw field preserved");
    check(h.subchan == 3 && h.chunk_idx == 0, "G4", "subchannel and index");
    check(h.frame_id == 0xDEADBEEF, "G4", "frame_id little-endian");
    check(h.is_aead, "G4", "flag 1 = self-contained encrypted chunk");

    /* The chunk at index max EXISTS: it is the tail of the bottom slice, the one
     * G20 dropped on every picture. Rejecting it reintroduces 626 errors. */
    n = hdr(b, 3, 120, 120, 1, 0);
    check(vid_wire_parse_hdr(b, n, &h), "G21", "chunk idx==max accepted");
    check(h.chunk_idx < h.chunk_count, "G21", "idx==max falls INSIDE the count");
    check(!h.is_aead, "G21", "flag 0 = plaintext continuation");

    /* Single-chunk picture: field 0, hence one chunk, not zero. The old code made
     * the +1 conditional on `field > 0` and left a count of 0. */
    n = hdr(b, 0, 0, 0, 7, 1);
    check(vid_wire_parse_hdr(b, n, &h), "G4", "single-chunk picture accepted");
    check(h.chunk_count == 1, "G4", "max=0 gives 1 chunk, not 0");

    /* Packets too short to carry a payload. `b` here carries the AEAD flag (last
     * call to hdr(..., 1)): it is for that case, and only that case, that the
     * bound is 11 + 28. */
    check(!vid_wire_parse_hdr(b, VID_WIRE_HDR_LEN, &h), "-", "header alone rejected");
    check(!vid_wire_parse_hdr(b, 39, &h), "-", "AEAD: header + 28 rejected (bound)");
    check(vid_wire_parse_hdr(b, 40, &h), "-", "AEAD: one more byte accepted");
    check(!vid_wire_parse_hdr(NULL, 128, &h), "-", "null pointer rejected");

    /* === V11 — THE COUNTER-CASE: A PICTURE'S SHORT TAIL ===
     *
     * Bytes taken AS THEY ARE from the OFFICIAL client's wire
     * (captures/captures_states_20260821_031020, UDP_RECVMSG peer=...:14010,
     * t=1787274619.205365710, len=20):
     *
     *   23 88 38 00 38 00 9d e0 4f a4 00 | be 1d 29 e6 dc 44 4b 7f 80
     *
     * subchannel 0x88, idx 56, max 56 (= the LAST chunk), flag 0 (= plaintext
     * continuation, neither nonce nor seal), 9 bytes of tail. The server resends
     * it identically ~5 ms later: both copies fell under the old single bound
     * `n > 39`, so the loss was CERTAIN. Measured cost: 2.0 to 2.7 % of pictures
     * cut off at their tail at 5-13 Mbit/s, and on the 5-6 Mbit/s sessions
     * `chunks_missing` equalled `frames_miss1` EXACTLY — all of our "UDP loss"
     * was this guard.
     *
     * Across 4 captures of the official client, 98 packets of 12 to 39 bytes
     * arrive on the video port and all 98 have `chunk_idx == max_field` and
     * `b10 == 0`. Not one of them is anything but a tail.
     *
     * The mistake not to make while relaxing the bound: the 11-byte BEACON (bare
     * header, `b0 = 0x33`, idx = max = 0) must stay rejected — it is what
     * poisoned the round-trip clock before V9. */
    const uint8_t tail[] = {
        0x23, 0x88, 0x38, 0x00, 0x38, 0x00, 0x9d, 0xe0, 0x4f, 0xa4, 0x00,
        0xbe, 0x1d, 0x29, 0xe6, 0xdc, 0x44, 0x4b, 0x7f, 0x80
    };
    check(vid_wire_parse_hdr(tail, sizeof tail, &h), "V11",
          "9-byte tail accepted (20 B on the official wire)");
    check(!h.is_aead, "V11", "tail: flag 0 = plaintext continuation");
    check(h.chunk_idx == 56 && h.max_field == 56, "V11",
          "tail: idx == max = last chunk of the picture");
    check(h.chunk_idx == h.chunk_count - 1, "V11",
          "tail: it really is the last index of the count (G4/G21)");
    check(h.subchan == 0x88, "V11", "tail: subchannel read back");

    /* The absolute minimum of a continuation: ONE useful byte. */
    uint8_t shortp[128];         /* hdr() writes 128 bytes: no fewer */
    hdr(shortp, 9, 5, 5, 42, 0);
    check(vid_wire_parse_hdr(shortp, VID_WIRE_HDR_LEN + 1, &h), "V11",
          "single-byte continuation accepted");
    check(!vid_wire_parse_hdr(shortp, VID_WIRE_HDR_LEN, &h), "V11",
          "bare header WITHOUT payload still rejected (V9 beacon)");

    /* And the AEAD bound stays whole: a 39-byte encrypted chunk cannot carry
     * nonce(12) + seal(16) + a single byte of ciphertext. */
    hdr(shortp, 9, 5, 5, 42, 1);
    check(!vid_wire_parse_hdr(shortp, 39, &h), "V11",
          "short AEAD still rejected: the bound was NOT relaxed for it");
}

/* === V11 — CLASSIFYING AN ORPHAN ===
 * The inputs are TAKEN from the official client's captures; each one is a case
 * where the naive rule "the last index = a duplicate" is wrong, and wrong in the
 * dangerous direction: it labels "harmless" data that we really are losing. */
static void test_orphan_kind(void)
{
    /* The NORMAL case, ~1 per picture: the server resends the tail ~3 ms after
     * the original. The picture has just been emitted, complete, on this
     * subchannel. */
    check(vid_wire_orphan_is_dup(24, 24, true, 0), "V11",
          "tail resent right after a complete picture = duplicate");
    check(vid_wire_orphan_is_dup(24, 24, true, VID_WIRE_ORPHAN_WINDOW), "V11",
          "still a duplicate at the edge of the window");

    /* COUNTER-CASE 1 — scenario_manette_20260826, subchannel 71: the picture's
     * opening (idx 0) was lost, so NO picture was ever emitted on this
     * subchannel. All 135 of its chunks land as orphans, including index 134.
     * `chunk_idx == count-1` declares it a harmless duplicate; in truth it is the
     * tail of the bottom slice, and we lose it. Measured: 206 chunks in that case
     * across 4 captures. */
    check(!vid_wire_orphan_is_dup(135, 0, false, 0), "V11",
          "picture head lost: the last index is NOT a duplicate");

    /* COUNTER-CASE 2 — captures_input_20260602, subchannel 53: the last picture
     * emitted here did have the same count (10), but 158 pictures earlier — the
     * subchannel was reused. Without the recency window, 280 genuinely lost
     * chunks passed for duplicates on that single capture, because 17,379 chunks
     * out of 19,259 there carry `count == 10`. */
    check(!vid_wire_orphan_is_dup(10, 10, true, 158), "V11",
          "subchannel reused 158 pictures later: not a duplicate");

    /* A picture emitted TRUNCATED allows no conclusion: not all of its indices
     * were seen, so the chunk may be the one that was missing. */
    check(!vid_wire_orphan_is_dup(24, 24, false, 0), "V11",
          "picture emitted truncated: we do not conclude duplicate");

    /* A different count = another picture, whatever happens. */
    check(!vid_wire_orphan_is_dup(25, 24, true, 0), "V11",
          "different count = another picture");
}

static void test_starts_picture(void)
{
    /* TOP slice: type 1, first_mb==0 (top bit set). */
    const uint8_t top[]    = {0,0,0,1, 0x41, 0x9A, 0x00, 0x11, 0x22};
    /* BOTTOM slice: first_mb != 0 (top bit clear). */
    const uint8_t bottom[] = {0,0,0,1, 0x41, 0x3C, 0x00, 0x11, 0x22};
    check(vid_wire_starts_picture(top, sizeof top), "G40", "first_mb=0 starts a picture");
    check(!vid_wire_starts_picture(bottom, sizeof bottom), "G40", "first_mb!=0 starts nothing");

    /* SPS + PPS before the slice: they must be walked through, not mistaken for
     * a VCL slice. */
    const uint8_t with_ps[] = {0,0,0,1, 0x67, 0x42, 0x00, 0x1f,
                               0,0,0,1, 0x68, 0xCE, 0x3C, 0x80,
                               0,0,0,1, 0x65, 0x88, 0x00, 0x11, 0x22};
    check(vid_wire_starts_picture(with_ps, sizeof with_ps), "G40", "SPS/PPS walked through to the IDR");

    /* It is the FIRST VCL slice that decides: a bottom-then-top emission does
     * not start a picture, otherwise we would cut mid-picture. */
    const uint8_t bottom_then_top[] = {0,0,0,1, 0x41, 0x3C, 0x00, 0x11,
                                       0,0,0,1, 0x41, 0x9A, 0x00, 0x11, 0x22};
    check(!vid_wire_starts_picture(bottom_then_top, sizeof bottom_then_top),
          "G40", "only the first VCL slice decides");

    /* 3-byte start code, allowed by the spec. */
    const uint8_t sc3[] = {0,0,1, 0x41, 0x9A, 0x00, 0x11, 0x22};
    check(vid_wire_starts_picture(sc3, sizeof sc3), "G40", "3-byte start code");

    /* No VCL slice: must assert nothing. */
    const uint8_t sps_only[] = {0,0,0,1, 0x67, 0x42, 0x00, 0x1f, 0x00, 0x11};
    check(!vid_wire_starts_picture(sps_only, sizeof sps_only), "G40", "SPS alone does not start a picture");
    check(!vid_wire_starts_picture(NULL, 0), "-", "null stream rejected");
}

static void test_tail_loss(void)
{
    /* Nominal G43 case: 118 received out of 121, all contiguous from 0, no IDR.
     * Emitting truncated keeps the top slice as a valid reference. */
    check(vid_wire_is_tail_loss(118, 121, 117, false), "G43", "missing tail = emittable truncated");

    /* An incomplete IDR is NEVER emitted: a corrupted key frame poisons every
     * reference that follows it. */
    check(!vid_wire_is_tail_loss(118, 121, 117, true), "G43", "incomplete IDR never truncated");

    /* Hole in the MIDDLE: last_contiguous+1 != recv_count. Emitting would glue
     * two non-adjacent chunks together. */
    check(!vid_wire_is_tail_loss(118, 121, 50, false), "G43", "hole in the middle = not a tail loss");

    /* Too many chunks missing: this is no longer a tail, it is a missing
     * picture. */
    check(!vid_wire_is_tail_loss(100, 121, 99, false), "G43", "21 missing = too many to truncate");
    check(vid_wire_is_tail_loss(118, 121, 117, false), "G43", "3 missing = the accepted limit");
    check(!vid_wire_is_tail_loss(117, 121, 116, false), "G43", "4 missing = refused");

    /* Complete picture: nothing to truncate. */
    check(!vid_wire_is_tail_loss(121, 121, 120, false), "G43", "a complete picture is not a loss");
    check(!vid_wire_is_tail_loss(0, 0, -1, false), "G43", "a null count triggers nothing");
}

/* === CFG3 — THE GRAMMAR IS THE PICTURE'S, NOT THE PROCESS'S ===
 *
 * Byte 0 of every VideoFrame header carries the picture's codec in its HIGH
 * nibble and the format version in its low one (RE-3, read out of the official
 * client's parser, which picks its NAL grammar from exactly that nibble). The
 * reassembly used to take its grammar from SHADOW_CODEC instead, read ONCE per
 * process: after an H.265 session, an H.264 session was parsed with the HEVC
 * rules - on the real dump, 0 picture starts, 0 IDR and 0 SPS against
 * 1928 / 8 / 4. G40, F11 and G43 went silently off and K16d raised a false
 * alarm. A grammar frozen on the wrong codec is a function that ignores this
 * byte: the checks below fail for it whichever codec it froze on. */
static void test_vf_codec(void)
{
    check(vid_wire_vf_is_hevc(0x02) == 0, "CFG3", "0x02 = H.264 version 2: H.264 grammar");
    check(vid_wire_vf_is_hevc(0x12) == 1, "CFG3",
          "0x12 = H.265 version 2: HEVC grammar (the HIGH nibble is the codec, RE-3)");
    check(vid_wire_vf_is_hevc(0x22) == 0, "CFG3", "0x22 = AV1 version 2: not HEVC");

    /* THE COUNTER-CASE: the same H.264 IDR bytes open a picture under the grammar
     * of 0x02, and open nothing under the grammar of 0x12. The second reading
     * applied to the first stream is exactly the frozen flag: no access unit is
     * ever split on structure again. */
    const uint8_t h264_idr[] = { 0,0,0,1, 0x65, 0x80 };
    check(vid_wire_starts_picture_codec(h264_idr, sizeof h264_idr,
                                        vid_wire_vf_is_hevc(0x02)),
          "CFG3", "COUNTER-CASE: an H.264 IDR in a 0x02 picture opens a picture");
    check(!vid_wire_starts_picture_codec(h264_idr, sizeof h264_idr,
                                         vid_wire_vf_is_hevc(0x12)),
          "CFG3", "COUNTER-CASE: the same bytes under 0x12's grammar open nothing");

    /* The same freeze in the other direction: an H.265 session after an H.264
     * one. First slice of a TRAIL_R picture, as in the K15 block. */
    const uint8_t hevc_first[] = { 0,0,0,1, 0x02, 0x01, 0x80, 0x00 };
    check(vid_wire_starts_picture_codec(hevc_first, sizeof hevc_first,
                                        vid_wire_vf_is_hevc(0x12)),
          "CFG3", "an H.265 first slice in a 0x12 picture opens a picture");
    check(!vid_wire_starts_picture_codec(hevc_first, sizeof hevc_first,
                                         vid_wire_vf_is_hevc(0x02)),
          "CFG3", "... and opens nothing under 0x02's grammar");
}

int main(void)
{
    printf("== semantics of the Shadow video wire ==\n");
    test_hdr();
    test_orphan_kind();
    test_starts_picture();
    test_tail_loss();
    /* === K15 — HEVC, AND WHY CONFUSING IT RAISES NO ERROR ===
     *
     * HEVC's NAL header is TWO bytes: the type lives on six bits starting at
     * bit 1 (`(b0 >> 1) & 0x3F`), and ALL types from 0 to 31 are picture slices
     * — against exactly two values (1 and 5) in H.264. The first-slice flag is
     * therefore in the SECOND byte after the header, not the first.
     *
     * THE CENTRAL COUNTER-CASE: reading an HEVC stream the H.264 way raises NO
     * error. The computed type is simply arbitrary, almost never 1 or 5, so
     * `starts_picture` always returns false — no access unit is ever closed and
     * the picture never comes out. A silence, not a diagnosis. */
    {
        /* TRAIL_R (type 1): b0 = 1<<1 = 0x02, b1 = 0x01 (layer 0, tid 1). Then
         * the body, whose top bit carries
         * first_slice_segment_in_pic_flag. */
        const uint8_t first[] = { 0,0,0,1, 0x02, 0x01, 0x80, 0x00 };
        const uint8_t next[]  = { 0,0,0,1, 0x02, 0x01, 0x00, 0x00 };
        check(vid_wire_starts_picture_hevc(first, sizeof first),
               "K15", "HEVC: first slice of a picture recognised");
        check(!vid_wire_starts_picture_hevc(next, sizeof next),
               "K15", "HEVC: a LATER slice of the same picture, not a start");

        /* IDR_W_RADL = type 19 -> b0 = 19<<1 = 0x26 */
        const uint8_t idr[] = { 0,0,0,1, 0x26, 0x01, 0x80, 0x00 };
        check(vid_wire_starts_picture_hevc(idr, sizeof idr),
               "K15", "HEVC: an IDR is a picture slice like any other (type 19)");

        /* VPS = 32, SPS = 33, PPS = 34: beyond 31 these are NOT slices. A
         * parameter set followed by a slice must return the SLICE's verdict, not
         * stop at the parameter set. */
        const uint8_t vps_only[] = { 0,0,0,1, 0x40, 0x01, 0x80, 0x00 };
        check(!vid_wire_starts_picture_hevc(vps_only, sizeof vps_only),
               "K15", "HEVC: a lone VPS (type 32) is not a picture slice");
        const uint8_t vps_then_slice[] = {
            0,0,0,1, 0x40, 0x01, 0x00, 0x00,      /* VPS */
            0,0,0,1, 0x02, 0x01, 0x80, 0x00,      /* slice, first of the picture */
        };
        check(vid_wire_starts_picture_hevc(vps_then_slice, sizeof vps_then_slice),
               "K15", "HEVC: parameter sets are skipped, the first slice decides");

        /* COUNTER-CASE: the same HEVC stream read by the H.264 parser. */
        check(!vid_wire_starts_picture(first, sizeof first),
               "K15", "COUNTER-CASE: the H.264 parser sees NOTHING in HEVC — it does "
              "not get it wrong, it stays silent, and the picture never comes out");

        /* And the reverse: H.264 read as HEVC must not be accepted by accident.
         * `0x65` (H.264 IDR) gives an HEVC type of 50: outside the slice range. */
        const uint8_t h264_idr[] = { 0,0,0,1, 0x65, 0x80, 0x00, 0x00 };
        check(vid_wire_starts_picture(h264_idr, sizeof h264_idr),
               "K15", "control: the H.264 parser does recognise its own IDR");
        check(!vid_wire_starts_picture_hevc(h264_idr, sizeof h264_idr),
               "K15", "COUNTER-CASE: H.264 read as HEVC is not accepted by accident");

        /* The explicit dispatch returns the same as the chosen variant. */
        check(vid_wire_starts_picture_codec(first, sizeof first, 1)
                  == vid_wire_starts_picture_hevc(first, sizeof first),
               "K15", "the HEVC dispatch returns the HEVC variant");
        check(vid_wire_starts_picture_codec(h264_idr, sizeof h264_idr, 0)
                  == vid_wire_starts_picture(h264_idr, sizeof h264_idr),
               "K15", "the H.264 dispatch returns the H.264 variant");

        /* Bounds: a buffer that is too short must not be read past. The HEVC
         * variant reads TWO bytes more than the H.264 one, so its bound differs. */
        const uint8_t tooshort[] = { 0,0,0,1, 0x02 };
        check(!vid_wire_starts_picture_hevc(tooshort, sizeof tooshort),
               "K15", "HEVC: buffer too short, no out-of-bounds read");
        check(!vid_wire_starts_picture_hevc(NULL, 16),  "K15", "HEVC: null pointer");
    }

    test_vf_codec();   /* CFG3 - after K15, whose two grammars it chooses between */

    printf("%d checks, %d failure(s)\n", total, failed);
    if (!failed) printf("OK\n");
    return failed ? 1 : 0;
}
