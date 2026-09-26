/* audio_route.h - which `:base+30` plaintexts are audio, and when a session may
 * still be recognised as FLAC.
 *
 * PURE module: no dependency, no I/O, no global state, testable offline
 * (tests/test_audio_route.c). Same intent as audio_dedup.h: two rules that each
 * cost a campaign, written once so that their call sites cannot drift apart.
 *
 * Created 2026-09-11 (campaigns DEC-1 and AUD-DEDUP-1).
 *
 * RULE 1 - ONLY `0x12` IS AUDIO. Every stream on `:base+30` opens with a stream
 * DESCRIPTOR of type `0x02`, sent twice with sequence number 0 (live logs,
 * 2026-09-11):
 *     Opus session: 271 B, `02 00 00 00 00 01 00 01 00 10 80 bb 02 01 01 ...`
 *     FLAC session:  13 B, `02 00 00 00 00 01 00 02 00 10 80 bb 02`
 * Bytes 9-12 are 16 bits, 48000 Hz, 2 channels; byte 7 was 1 in every Opus
 * session and 2 in every FLAC one (the codec, probably - unconfirmed). The two
 * reassembly call sites already tested `== 0x12`; the single-packet one did not,
 * and handed the descriptor to the decoder.
 *
 * RULE 2 - FLAC IS A ONE-WAY PROMOTION. A native FLAC frame starts with the
 * `ff f8..fb` sync right after the `[0x12][seq u32 LE]` prefix. No valid Opus
 * packet starts that way: TOC 0xff with a count byte f8..fb announces 56-59
 * frames of 20 ms. Measured: 0 of 360 000 real Opus packets and 0 of 23 984
 * random ones starting `ff f8..fb` matched, parsed or decoded.
 *
 * COUNTER-CASE (DEC-1, 2026-09-11) - FLAC SILENT AFTER AN OPUS SESSION, AND FROM
 * THE 8TH FLAC SESSION OF A PROCESS ON. The decoder recognised FLAC only while
 * the session's codec was still unclassified, and the descriptor reached it
 * first. The Opus offset probe in media/audio.c is process-wide: once an earlier
 * session had fixed it (an Opus session, or eight descriptors from eight FLAC
 * sessions, each counting as one more "consecutive" frame), the descriptor
 * classified the session as Opus, and every FLAC frame after it was refused as
 * badly framed. Bench: 0 of 200 frames decoded, deterministic over 7
 * repetitions; the panel said "Opus" and no warning fired. Live on 2026-09-11,
 * session 8 of ten FLAC sessions logged `a 5-byte header ... 8 consecutive
 * frames`, then nothing but `packet discarded (invalid framing, len=16,
 * nb=-4)`. Rule 1 keeps the descriptor out; rule 2 recovers a session that
 * anything else classified first. Either one alone fixes both routes.
 *
 * WHY THE SYNC ALSO REQUIRES `0x12`. Raw Opus with no prefix carries Opus
 * data in bytes 5-6. Measured on the libdatachannel path (removed 2026-09-26),
 * which fed this decoder raw RTP Opus: without the gate, 8 of 294 000 real
 * packets matched the sync and 3 of 14 sessions of 200 s were promoted to FLAC
 * for good, then went silent. With it: 0 and 0. The gate stays - it is what
 * makes the sync mean "a FLAC frame" rather than "two bytes that look like one".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A `:base+30` plaintext is audio when its type byte is 0x12. */
static inline bool aud_plain_is_audio(const uint8_t *p, int len)
{
    return len >= 1 && p[0] == 0x12;
}

/* An audio plaintext whose payload, after the 5-byte prefix, starts with the
 * FLAC frame sync. */
static inline bool aud_flac_sync(const uint8_t *p, size_t len)
{
    return len >= 7 && p[0] == 0x12 && p[5] == 0xFF && (p[6] & 0xFC) == 0xF8;
}

/* Must this packet switch the decoder to FLAC? `codec` is
 * struct audio_decoder::codec_detected: 0 unknown, 1 Opus, 2 FLAC. From unknown,
 * always; from Opus, only when `late` (SHADOW_FLAC_LATE_DETECT, on by default);
 * never back out of FLAC, and never twice. */
static inline bool aud_flac_promote(int codec, bool late, const uint8_t *p, size_t len)
{
    return (codec == 0 || (late && codec == 1)) && aud_flac_sync(p, len);
}
