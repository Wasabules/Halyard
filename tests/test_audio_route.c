/* test_audio_route.c - which :base+30 plaintexts are audio, and when a session
 * may still become FLAC (streaming/audio_route.h).
 *
 * Campaigns DEC-1 and AUD-DEDUP-1 (2026-09-11): a FLAC session after an Opus
 * session of the same process decoded nothing, and so did every FLAC session
 * from the 8th of a process on; every Opus session after the first counted one
 * false decode error. The chain is told in audio_route.h.
 *
 * Every counter-case is checked on both rules: the header's must get it right,
 * and the rule as it stood (`old_*`, transcribed below) must get it wrong -
 * which is what proves the check tells the two apart.
 */
#include "../core/protocol/audio_route.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* ---- The rules before DEC-1, verbatim ---------------------------------------
 * ctrl_session.c, single-packet path, SHADOW_CURSOR_ON_30 unset:
 *     is_cursor = g_cur30 && (ptype != 0x12)    -> false for every type,
 * so every plaintext went to on_audio. media/audio.c (K14):
 *     codec_detected == 0 && len >= 7
 *         && payload[5] == 0xFF && (payload[6] & 0xFC) == 0xF8            */
static bool old_is_audio(const uint8_t *p, int len)
{
    (void)p;
    return len >= 1;
}
static bool old_flac_promote(int codec, const uint8_t *p, size_t len)
{
    return codec == 0 && len >= 7 && p[5] == 0xFF && (p[6] & 0xFC) == 0xF8;
}

/* ---- Live bytes ------------------------------------------------------------ */
/* `[AUD2] trame audio #1 len=271` of an Opus session (the 20 bytes logged). */
static const uint8_t desc_opus[271] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x10,
    0x80, 0xbb, 0x02, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00 };
/* `[AUD2] trame audio #1 len=13` of a FLAC session. */
static const uint8_t desc_flac[13] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x10,
    0x80, 0xbb, 0x02 };
/* `[AUD2] trame audio #3 len=8` of an Opus session: a silence frame. */
static const uint8_t opus_silence[8] = {
    0x12, 0x01, 0x00, 0x00, 0x00, 0xf4, 0xff, 0xfe };
/* `[AUD2] trame audio #3 len=21` of a FLAC session: a silence frame. */
static const uint8_t flac_silence[21] = {
    0x12, 0x01, 0x00, 0x00, 0x00, 0xff, 0xf8, 0x7a, 0x18, 0x00,
    0x01, 0xdf, 0xa6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x07 };
/* A raw RTP Opus payload of the legacy DC path whose bytes 5-6 happen to read
 * like the FLAC sync - 8 of 294 000 real packets did (DEC-1 bench). */
static const uint8_t raw_rtp[12] = {
    0xfc, 0xff, 0xfe, 0x31, 0x07, 0xff, 0xf9, 0x42, 0x10, 0x00, 0x5a, 0x33 };

static void which_is_audio(void)
{
    CHECK(!aud_plain_is_audio(desc_opus, (int)sizeof desc_opus),
          "COUNTER-CASE: the 271-byte descriptor of an Opus session is not audio");
    CHECK(old_is_audio(desc_opus, (int)sizeof desc_opus),
          "... and the old single-packet rule sent it to the decoder");
    CHECK(!aud_plain_is_audio(desc_flac, (int)sizeof desc_flac),
          "COUNTER-CASE: the 13-byte descriptor of a FLAC session is not audio");
    CHECK(old_is_audio(desc_flac, (int)sizeof desc_flac),
          "... and the old rule sent that one too");
    CHECK(aud_plain_is_audio(opus_silence, (int)sizeof opus_silence),
          "an Opus silence frame (12 seq f4 ff fe) is audio");
    CHECK(aud_plain_is_audio(flac_silence, (int)sizeof flac_silence),
          "a FLAC silence frame (12 seq ff f8 ...) is audio");
    CHECK(!aud_plain_is_audio(opus_silence, 0), "an empty plaintext is not audio");
}

static void which_is_flac(void)
{
    uint8_t f22[22];
    memcpy(f22, flac_silence, sizeof flac_silence);
    f22[21] = 0x00;
    f22[6]  = 0xf9;   /* the variable-blocksize sync */

    CHECK(!aud_flac_sync(opus_silence, sizeof opus_silence),
          "an Opus frame (12 seq f4 ...) is not FLAC");
    CHECK(aud_flac_sync(flac_silence, sizeof flac_silence),
          "a 21-byte FLAC silence frame (12 seq ff f8) is FLAC");
    CHECK(aud_flac_sync(f22, sizeof f22),
          "a 22-byte one with the variable-blocksize sync ff f9 is FLAC too");
    CHECK(!aud_flac_sync(flac_silence, 6), "six bytes cannot carry the sync");
    CHECK(!aud_flac_sync(desc_opus, sizeof desc_opus)
          && !aud_flac_sync(desc_flac, sizeof desc_flac),
          "a stream descriptor is never FLAC");
    CHECK(!aud_flac_sync(raw_rtp, sizeof raw_rtp),
          "COUNTER-CASE: a raw RTP packet with ff f9 at byte 5 but no 0x12 prefix is never FLAC");
    CHECK(old_flac_promote(0, raw_rtp, sizeof raw_rtp),
          "... where the old test, which ignored the type byte, called it FLAC");
}

static void promotion(void)
{
    CHECK(aud_flac_promote(0, true, flac_silence, sizeof flac_silence)
          && aud_flac_promote(0, false, flac_silence, sizeof flac_silence),
          "unknown -> FLAC on a FLAC frame, with the toggle on or off");
    CHECK(aud_flac_promote(1, true, flac_silence, sizeof flac_silence),
          "COUNTER-CASE: a session classified Opus is promoted by a FLAC frame");
    CHECK(!old_flac_promote(1, flac_silence, sizeof flac_silence),
          "... which the old rule refused: that is the silent FLAC session");
    CHECK(!aud_flac_promote(1, false, flac_silence, sizeof flac_silence),
          "SHADOW_FLAC_LATE_DETECT=0 restores the old rule: no promotion out of Opus");
    CHECK(!aud_flac_promote(2, true, opus_silence, sizeof opus_silence)
          && !aud_flac_promote(2, true, flac_silence, sizeof flac_silence),
          "never out of FLAC, and never promoted twice");
    CHECK(!aud_flac_promote(0, true, desc_flac, sizeof desc_flac)
          && !aud_flac_promote(1, true, raw_rtp, sizeof raw_rtp),
          "neither a descriptor nor a raw RTP packet promotes");
}

/* ---- The two routes, replayed on audio.c's classification -------------------
 * Reduced to what decides the codec. The Opus offset probe is process-wide and
 * here already fixed - after one Opus session (route 1), or after eight
 * descriptors from eight FLAC sessions (route 2) - so a packet that does not
 * promote to FLAC and reaches the Opus path while the codec is unknown marks the
 * session Opus: audio.c does it before validating the frame. The stream is the
 * descriptor twice, then `frames` FLAC frames. Returns the FLAC frames decoded. */
typedef struct {
    bool filter;      /* SHADOW_AUDIO_TYPE_FILTER */
    bool late;        /* SHADOW_FLAC_LATE_DETECT */
    bool old_rules;   /* the rules before DEC-1 (the two toggles then do nothing) */
} rules_t;

static int flac_session(rules_t r, const uint8_t *desc, int desc_len, int frames)
{
    int codec = 0, decoded = 0;
    for (int i = 0; i < 2 + frames; i++) {
        const uint8_t *p = i < 2 ? desc : flac_silence;
        const int len = i < 2 ? desc_len : (int)sizeof flac_silence;
        const bool audio = r.old_rules ? old_is_audio(p, len)
                                       : (!r.filter || aud_plain_is_audio(p, len));
        if (!audio) continue;                          /* dropped at ingress */
        const bool promote = r.old_rules
                           ? old_flac_promote(codec, p, (size_t)len)
                           : aud_flac_promote(codec, r.late, p, (size_t)len);
        if (promote) codec = 2;
        if (codec == 2) {
            if (aud_flac_sync(p, (size_t)len)) decoded++;
            continue;
        }
        if (codec == 0) codec = 1;
    }
    return decoded;
}

static void routes(void)
{
    const rules_t old_rules   = { false, false, true };
    const rules_t dec1        = { true,  true,  false };
    const rules_t filter_only = { true,  false, false };
    const rules_t late_only   = { false, true,  false };

    CHECK(flac_session(old_rules, desc_flac, (int)sizeof desc_flac, 200) == 0,
          "COUNTER-CASE: old rules, a FLAC session once the probe is fixed: 0 of 200 frames");
    CHECK(flac_session(dec1, desc_flac, (int)sizeof desc_flac, 200) == 200,
          "DEC-1: 200 of 200");
    CHECK(flac_session(filter_only, desc_flac, (int)sizeof desc_flac, 200) == 200,
          "the ingress filter alone is enough (SHADOW_FLAC_LATE_DETECT=0)");
    CHECK(flac_session(late_only, desc_flac, (int)sizeof desc_flac, 200) == 200,
          "the promotion alone is enough (SHADOW_AUDIO_TYPE_FILTER=0)");
    CHECK(flac_session(old_rules, desc_opus, (int)sizeof desc_opus, 200) == 0
          && flac_session(dec1, desc_opus, (int)sizeof desc_opus, 200) == 200,
          "same with the 271-byte descriptor, should a FLAC stream ever open with it");
}

int main(void)
{
    printf("== which :base+30 plaintexts are audio, and when FLAC is recognised ==\n");
    which_is_audio();
    which_is_flac();
    promotion();
    routes();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
