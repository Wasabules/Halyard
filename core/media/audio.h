// Audio decode and output.
//
// The native session hands every AudioOut frame (`:base+30`, reassembled) to
// audio_decoder_feed(); the codec is the one the server granted - Opus or FLAC.
// Output is 48 kHz stereo s16: audout on the Switch, SceAudioOut on the Vita,
// ALSA or WASAPI on the desktop.
//
// (Named `audio_feed_rtp` until 2026-09-26, from the WebRTC path that fed it
// RTP Opus; that path is gone and the function never took RTP on this one.)
#pragma once

#include "../protocol/eq.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_decoder audio_decoder;

// Creates the decoder and opens the platform output. NULL on failure.
audio_decoder *audio_decoder_create(void);

// Frees everything. Safe on NULL.
void audio_decoder_destroy(audio_decoder *a);

// Feeds one reassembled AudioOut frame (decrypted, with its `0x12` type
// prefix). It is decoded to stereo s16 PCM and queued to the output. `ts` is
// the frame's server timestamp; the decoder does not use it.
void audio_decoder_feed(audio_decoder *a, const uint8_t *payload, size_t len,
                        uint32_t ts);

typedef struct {
    uint32_t packets_received;
    uint32_t frames_decoded;
    uint32_t decode_errors;
    uint32_t buffers_pushed;
    uint32_t buffers_dropped;   /* output buffer full - regulation, not a failure */
    uint32_t invalid_packets;   /* invalid Opus framing - dropped before decoding */
} audio_stats_t;
void audio_decoder_get_stats(audio_decoder *a, audio_stats_t *out);

/* K18 - the codec ACTUALLY received on `:base+30`: "Opus" or "FLAC". It is
 * NEGOTIATED when the session opens, so showing the requested setting would lie
 * - the panel used to hard-code "Opus 48 kHz" while FLAC was being decoded.
 * Returns "?" until a frame has been classified. */
const char *audio_decoder_codec_name(const audio_decoder *a);

/* EQV2 2026-09-11 - the equaliser's filter STATE starts from rest for each
 * session; its settings are kept. Call it once per session, before
 * audio_decoder_create(). Declared in the part C sees: its caller is the C
 * session glue. See media/audio.c. */
void audio_eq_session_start(void);

#ifdef __cplusplus
}
/* Volume de sortie, en pourcent (0 a 300 ; 100 = niveau d'origine).
 * Above 100 it is a boost: loud passages clip, which is accepted - a remote
 * machine whose mixer is turned down stays inaudible
 * autrement. Utilisable a tout moment, y compris flux en cours. */
void     audio_set_volume(uint32_t pourcent);
uint32_t audio_get_volume(void);

/* === S90 — CORRECTION TONALE ===
 *
 * See `streaming/eq.h` for why biquads (no algorithmic latency
 * algorithmique, contrairement a un filtre a phase lineaire).
 *
 * Global rather than tied to the decoder: it is set outside a session, and it
 * must hold for the next one without having to be set again. Usable at any time,
 * including mid-stream - the coefficients are swapped under a lock. */
void  audio_eq_configure(const eq_band_t *bands, int nb, bool auto_trim);
bool  audio_eq_active(void);
/* The cascade's response at `freq`, in dB. For drawing the curve on screen. */
float audio_eq_reponse_db(float freq);

#endif
