/* ctrl_audio_dtls - DTLS 1.2 client over UDP.
 *
 * WARNING 2026-08-21: `:base+12` is NOT the audio channel. The capture
 * `captures_inputport_20260821_135207/` proves this port carries the INPUT
 * channel (KB.md §3.21) - the SSL object fed by this socket carried 1833 moves
 * of 144 B, 19 clicks of 152 B and 1212 server replies of 104 B. That is why
 * this channel logged `idle Ns (no audio packets)` in every session.
 * The DTLS plumbing itself is still correct and now serves the input channel
 * through `ctrl_dtls_connect_raw`; the audio history below is kept for the
 * record, but its premise has been refuted.
 *
 * I2 2026-05-18 phase 1: found through capture V16, UDP_SENDMSG on :15012.
 * First bytes `16 fe ff` (DTLS ClientHello) then, once the handshake is done,
 * 141 B UDP records (= Opus payloads, 48 kHz stereo).
 *
 * Architecture:
 *   - bind the UDP socket on :base+12
 *   - wolfSSL DTLS 1.2 client + handshake (~4 round trips)
 *   - receive loop: decrypt the Opus frame -> on_audio callback
 *
 * Reuses audio.c::audio_decoder_feed() for the Opus decode and audout/ALSA
 * playback. (Since §3.37 this channel is known to be INPUT, not audio - see
 * SHADOW_AUDIO_DTLS; the file keeps a name whose premise was refuted.)
 *
 * Phase 1 delivered: handshake validation + RX byte counter, no decode.
 * Phase 2 (TODO): Opus parsing + wiring into audio_decoder.
 */

#pragma once

#include <stdbool.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctrl_audio_dtls ctrl_audio_dtls_t;

/* Plaintext Opus frame callback (after the DTLS decrypt). Wire it to
 * audio_decoder through a wrapper. NULL = log only. */
typedef void (*ctrl_audio_dtls_cb)(const uint8_t *payload, size_t len,
                                     uint32_t rtp_ts, void *user);

/* Open the UDP + DTLS connection on :base+12. Blocks for the handshake
 * (~1 s max), then spawns the RX thread. Returns 0 on success. */
int ctrl_audio_dtls_open(ctrl_audio_dtls_t **out,
                          const char *vm_host, uint16_t base_port,
                          ctrl_audio_dtls_cb cb, void *user);

/* Close, join the RX thread, release the resources. */
void ctrl_audio_dtls_close(ctrl_audio_dtls_t *c);

typedef struct {
    bool     handshake_ok;
    uint32_t bytes_recv;
    uint32_t frames_recv;
    uint32_t decrypt_fail;
} ctrl_audio_dtls_stats_t;

void ctrl_audio_dtls_get_stats(const ctrl_audio_dtls_t *c, ctrl_audio_dtls_stats_t *out);

#ifdef __cplusplus
}
#endif

/* S11 - shared DTLS plumbing (see ctrl_audio_dtls.c). Used by the input
 * channel, which is really DTLS/UDP on :base+12 (KB.md §3.21).
 * Returns the socket, the CTX and the session; the caller owns them. */
int ctrl_dtls_connect_raw(const char *vm_host, uint16_t port,
                           int *out_sock, WOLFSSL_CTX **out_ctx, WOLFSSL **out_ssl);
