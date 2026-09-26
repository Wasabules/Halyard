// Shadow streaming encryption - chacha20-poly1305 wrapper.
//
// Implements the Shadow wire format `[ciphertext_N | nonce_12B | tag_16B]`
// (28 B of overhead, no AAD), confirmed by Ghidra RE
// (see recon/ENCRYPTION_AND_FRAMING.md section 1).
//
// The API is symmetric with two distinct keys (Tx + Rx), mirroring the
// CryptoCipherOpenSsl layout of the Linux binary. Every Encrypt bumps a
// monotonic little-endian nonce (seeded randomly on the first encrypt).
// Decrypt enforces replay protection: nonces must be strictly increasing.
//
// Backend: wolfSSL `wc_ChaCha20Poly1305_Encrypt/Decrypt` (already
// cross-compiled for Switch by M7.1).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHADOW_KEY_LEN     32   /* ChaCha20-Poly1305 AEAD key */
#define SHADOW_NONCE_LEN   12   /* 96-bit IV */
#define SHADOW_TAG_LEN     16   /* Poly1305 MAC */
#define SHADOW_AEAD_OVERHEAD (SHADOW_NONCE_LEN + SHADOW_TAG_LEN)  /* 28 */

typedef struct shadow_cipher shadow_cipher;

/* Create a cipher with distinct Tx and Rx keys (= the CryptoCipher layout of
 * the Linux binary: key @+0x30 for Encrypt, key @+0x60 for Decrypt).
 * When the same key serves both directions, pass key_tx == key_rx.
 * Returns NULL on error (allocation, wolfSSL init).
 */
shadow_cipher *shadow_cipher_create(const uint8_t key_tx[SHADOW_KEY_LEN],
                                     const uint8_t key_rx[SHADOW_KEY_LEN]);

void shadow_cipher_destroy(shadow_cipher *c);

/* Encrypt in-place. `buf` must have at least `len + SHADOW_AEAD_OVERHEAD`
 * bytes of capacity. The layout written out is:
 *   buf[0..len-1]                       = ciphertext
 *   buf[len..len+11]                    = nonce
 *   buf[len+12..len+27]                 = tag
 * Returns the total number of bytes written (= len + 28), or -1 on error.
 */
int shadow_cipher_encrypt(shadow_cipher *c, uint8_t *buf, int len);

/* Decrypt in-place. The caller supplies nonce and tag separately (typically
 * pulled out of the wire format: nonce at `buf[ct_len..ct_len+11]`, tag at
 * `buf[ct_len+12..ct_len+27]`).
 * Returns true when decrypt + auth + replay check all pass.
 */
bool shadow_cipher_decrypt(shadow_cipher *c, uint8_t *buf, int ct_len,
                            const uint8_t *nonce, const uint8_t *tag);

/* Convenience: decrypt a whole wire message (ciphertext + nonce + tag).
 * Splits the buffer apart and calls shadow_cipher_decrypt. The resulting
 * plaintext is `total_len - 28` bytes. Returns true on success.
 */
bool shadow_cipher_decrypt_wire(shadow_cipher *c, uint8_t *buf, int total_len);

/* Variant without replay protection - for UDP streams where out-of-order
 * frames are legitimate (chacha20 video on :13010). The caller owns the
 * replay check, using the SUFP sequence number at the SUFP layer. */
bool shadow_cipher_decrypt_unsafe(shadow_cipher *c, uint8_t *buf, int ct_len,
                                    const uint8_t *nonce, const uint8_t *tag);

/* Variant with AAD, written for the Shadow byte10==0 chunks back when we
 * guessed they were FEC parity, or a bottom slice sealed with the SUFP header
 * as AAD. That guess was never confirmed, and F31 later refuted the premise
 * outright: there is no FEC in this protocol, and those chunks are plaintext
 * continuations. No production caller remains; only tests/test_encryption.c
 * exercises it. */
bool shadow_cipher_decrypt_aad_unsafe(shadow_cipher *c, uint8_t *buf, int ct_len,
                                        const uint8_t *nonce, const uint8_t *tag,
                                        const uint8_t *aad, int aad_len);

#ifdef __cplusplus
}
#endif
