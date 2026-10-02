#include "encryption.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/chacha20_poly1305.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/wc_port.h>   /* SEC3: wolfCrypt_Init */

#include "../common/log.h"

/* S81 - the log category is DECLARED here, never inferred from the message
 * text. `elog` stays at INFO so no existing call goes silent. `edbg` exists
 * for the high-volume lines, which move over to it one at a time. */
#define elog(...) JOURNAL_INFO_(JOURNAL_CAT_SESSION, __VA_ARGS__)
#define edbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SESSION, __VA_ARGS__)
struct shadow_cipher {
    uint8_t  key_tx[SHADOW_KEY_LEN];
    uint8_t  key_rx[SHADOW_KEY_LEN];
    uint8_t  tx_nonce[SHADOW_NONCE_LEN];   /* monotone counter, init random au 1er encrypt */
    bool     tx_nonce_initialized;
    uint8_t  rx_last_nonce[SHADOW_NONCE_LEN]; /* replay protection */
    bool     rx_seen;
    pthread_mutex_t mtx;
};

/* Increment little-endian 12-byte counter by 1. */
static void le_inc(uint8_t *n, int len) {
    for (int i = 0; i < len; i++) {
        if (++n[i] != 0) return;
    }
}

/* Compare 2 little-endian counters. Returns >0 if a>b, <0 if a<b, 0 if eq. */
static int le_cmp(const uint8_t *a, const uint8_t *b, int len) {
    for (int i = len - 1; i >= 0; i--) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

shadow_cipher *shadow_cipher_create(const uint8_t key_tx[SHADOW_KEY_LEN],
                                     const uint8_t key_rx[SHADOW_KEY_LEN]) {
    if (!key_tx || !key_rx) return NULL;
    shadow_cipher *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    memcpy(c->key_tx, key_tx, SHADOW_KEY_LEN);
    memcpy(c->key_rx, key_rx, SHADOW_KEY_LEN);
    pthread_mutex_init(&c->mtx, NULL);
    return c;
}

void shadow_cipher_destroy(shadow_cipher *c) {
    if (!c) return;
    /* Wipe sensitive material */
    memset(c->key_tx, 0, SHADOW_KEY_LEN);
    memset(c->key_rx, 0, SHADOW_KEY_LEN);
    pthread_mutex_destroy(&c->mtx);
    free(c);
}

/* === SEC3 2026-10-02 - THIS MODULE USED SOMEBODY ELSE'S INITIALISATION =====
 *
 * `wc_InitRng()` below locks a wolfCrypt-global mutex, and that mutex is
 * created by `wolfCrypt_Init()`. This module never called it. It worked anyway,
 * for a reason that is pure luck: the shipped client brings up its TLS control
 * channel before it ever encrypts, `wolfSSL_Init()` calls `wolfCrypt_Init()`,
 * and the mutex happens to exist by the time the cipher is used.
 *
 * Measured the moment that stopped being true. `tests/test_encryption.c` links
 * this file alone and SEGFAULTS on Windows, inside
 * `wc_LockMutex -> RtlEnterCriticalSection`: a CRITICAL_SECTION that was never
 * initialised faults, where a zero-initialised pthread mutex on Linux is
 * usable by accident. The suite had been printing SKIPPED on Windows for want
 * of a wolfSSL build directory, so nothing had ever run this path without TLS
 * in front of it.
 *
 * WHO ELSE WOULD HAVE HIT IT: anything that uses the cipher WITHOUT opening a
 * TLS channel first. A client that only wants the clipboard, or only file
 * transfer, or a tool that decrypts a capture offline - exactly the kind of
 * caller `session_caps.h` was written to make possible.
 *
 * So the module initialises what it uses. `pthread_once` because two threads
 * can reach the first encrypt together, and `wolfCrypt_Init()` is cheap,
 * refcounted and safe to call after `wolfSSL_Init()` has already called it -
 * which is why this does not disturb the shipped path. */
static pthread_once_t g_wc_once = PTHREAD_ONCE_INIT;
static int            g_wc_rc;

static void wc_init_once(void)
{
    g_wc_rc = wolfCrypt_Init();
    if (g_wc_rc != 0)
        elog("[SEC3] wolfCrypt_Init FAILED (%d): no random nonce can be drawn",
             g_wc_rc);
}

int shadow_cipher_encrypt(shadow_cipher *c, uint8_t *buf, int len) {
    if (!c || !buf || len < 0) return -1;

    /* SEC3: before anything that can reach wc_InitRng. */
    pthread_once(&g_wc_once, wc_init_once);
    if (g_wc_rc != 0) return -1;

    pthread_mutex_lock(&c->mtx);
    /* Seed the nonce randomly on the first encrypt (= the BN_rand behaviour
     * of the Linux binary) */
    if (!c->tx_nonce_initialized) {
        WC_RNG rng;
        if (wc_InitRng(&rng) != 0) {
            pthread_mutex_unlock(&c->mtx);
            return -1;
        }
        if (wc_RNG_GenerateBlock(&rng, c->tx_nonce, SHADOW_NONCE_LEN) != 0) {
            wc_FreeRng(&rng);
            pthread_mutex_unlock(&c->mtx);
            return -1;
        }
        wc_FreeRng(&rng);
        c->tx_nonce_initialized = true;
    } else {
        /* Bump the little-endian monotonic counter (= BN_add_word(1) followed
         * by BN_bn2lebinpad) */
        le_inc(c->tx_nonce, SHADOW_NONCE_LEN);
    }

    /* AEAD encrypt (no AAD) - wolfSSL signature:
     * Encrypt(key, iv, aad, aadLen, plain, plainLen, cipher_out, tag_out) */
    uint8_t tag[SHADOW_TAG_LEN];
    int rc = wc_ChaCha20Poly1305_Encrypt(c->key_tx, c->tx_nonce,
                                          NULL, 0,
                                          buf, (uint32_t)len,
                                          buf,         /* in-place */
                                          tag);
    if (rc != 0) {
        elog("shadow_cipher_encrypt: wc_ChaCha20Poly1305_Encrypt rc=%d", rc);
        pthread_mutex_unlock(&c->mtx);
        return -1;
    }

    /* APPEND the nonce, then the tag */
    memcpy(buf + len, c->tx_nonce, SHADOW_NONCE_LEN);
    memcpy(buf + len + SHADOW_NONCE_LEN, tag, SHADOW_TAG_LEN);

    pthread_mutex_unlock(&c->mtx);
    return len + SHADOW_AEAD_OVERHEAD;
}

bool shadow_cipher_decrypt_unsafe(shadow_cipher *c, uint8_t *buf, int ct_len,
                                    const uint8_t *nonce, const uint8_t *tag) {
    if (!c || !buf || !nonce || !tag || ct_len < 0) return false;
    int rc = wc_ChaCha20Poly1305_Decrypt(c->key_rx, nonce, NULL, 0,
                                          buf, (uint32_t)ct_len, tag, buf);
    return rc == 0;
}

bool shadow_cipher_decrypt_aad_unsafe(shadow_cipher *c, uint8_t *buf, int ct_len,
                                        const uint8_t *nonce, const uint8_t *tag,
                                        const uint8_t *aad, int aad_len) {
    if (!c || !buf || !nonce || !tag || ct_len < 0) return false;
    int rc = wc_ChaCha20Poly1305_Decrypt(c->key_rx, nonce, aad, (uint32_t)aad_len,
                                          buf, (uint32_t)ct_len, tag, buf);
    return rc == 0;
}

bool shadow_cipher_decrypt(shadow_cipher *c, uint8_t *buf, int ct_len,
                            const uint8_t *nonce, const uint8_t *tag) {
    if (!c || !buf || !nonce || !tag || ct_len < 0) return false;

    pthread_mutex_lock(&c->mtx);
    /* Replay protection: the incoming nonce must be strictly > the last seen */
    if (c->rx_seen) {
        if (le_cmp(nonce, c->rx_last_nonce, SHADOW_NONCE_LEN) <= 0) {
            elog("shadow_cipher_decrypt: invalid nonce (replay or out-of-order)");
            pthread_mutex_unlock(&c->mtx);
            return false;
        }
    }

    int rc = wc_ChaCha20Poly1305_Decrypt(c->key_rx, nonce,
                                          NULL, 0,
                                          buf, (uint32_t)ct_len,
                                          tag,
                                          buf);  /* in-place */
    if (rc != 0) {
        /* MAC mismatch, or any other failure */
        pthread_mutex_unlock(&c->mtx);
        return false;
    }

    /* Update last seen nonce */
    memcpy(c->rx_last_nonce, nonce, SHADOW_NONCE_LEN);
    c->rx_seen = true;

    pthread_mutex_unlock(&c->mtx);
    return true;
}

bool shadow_cipher_decrypt_wire(shadow_cipher *c, uint8_t *buf, int total_len) {
    if (total_len < SHADOW_AEAD_OVERHEAD) return false;
    int ct_len = total_len - SHADOW_AEAD_OVERHEAD;
    const uint8_t *nonce = buf + ct_len;
    const uint8_t *tag   = buf + ct_len + SHADOW_NONCE_LEN;
    return shadow_cipher_decrypt(c, buf, ct_len, nonce, tag);
}
