/* test_encryption.c - chacha20-poly1305, the heart of the Shadow wire.
 *
 * Everything arriving from the server goes through here. A parameter mistake -
 * key and nonce swapped, AAD passed when it must not be, Tx/Rx direction crossed
 * - does NOT show in use: it decrypts to mush, and the decoder gets blamed.
 * Hence the reference vector.
 *
 * The reference is RFC 8439 §2.8.2 (AEAD_CHACHA20_POLY1305): key, nonce, AAD,
 * plaintext, ciphertext and tag are all given byte by byte. If our wrapper
 * passes that vector, the key, the nonce and the AAD do reach the slots they
 * must.
 *
 * This file needs the compiled wolfSSL LIBRARY (unlike the other suites): the
 * real encryption is precisely what is being checked.
 */
#include <stdio.h>
#include <string.h>
#include <wolfssl/options.h>
#include "../core/protocol/encryption.h"

/* S81 - the journal now has a severity and a category; the modules call
 * `journal_write` through their alias. The stub must therefore provide both of
 * the module's functions, and `journal_enabled` must return FALSE: that is what
 * makes the test write nothing at all, including when the module under test logs
 * in a per-packet loop. */
void journal_uncategorised(const char *fmt, ...) { (void)fmt; }
void journal_write(int sev, int cat, const char *fmt, ...)
{ (void)sev; (void)cat; (void)fmt; }
int journal_enabled(int sev, int cat) { (void)sev; (void)cat; return 0; }

static int total = 0, failed = 0;
static void check(int cond, const char *what)
{
    total++;
    if (!cond) { failed++; printf("  FAIL  %s\n", what); }
}

/* ─── The RFC 8439 §2.8.2 vector ─────────────────────────────────────────── */
static const uint8_t RFC_KEY[32] = {
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f };
static const uint8_t RFC_NONCE[12] = {
    0x07,0x00,0x00,0x00,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47 };
static const uint8_t RFC_AAD[12] = {
    0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7 };
static const char RFC_CLAIR[] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one "
    "tip for the future, sunscreen would be it.";
static const uint8_t RFC_CHIFFRE[114] = {
    0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,0x7b,0x86,0xaf,0xbc,0x53,0xef,0x7e,0xc2,
    0xa4,0xad,0xed,0x51,0x29,0x6e,0x08,0xfe,0xa9,0xe2,0xb5,0xa7,0x36,0xee,0x62,0xd6,
    0x3d,0xbe,0xa4,0x5e,0x8c,0xa9,0x67,0x12,0x82,0xfa,0xfb,0x69,0xda,0x92,0x72,0x8b,
    0x1a,0x71,0xde,0x0a,0x9e,0x06,0x0b,0x29,0x05,0xd6,0xa5,0xb6,0x7e,0xcd,0x3b,0x36,
    0x92,0xdd,0xbd,0x7f,0x2d,0x77,0x8b,0x8c,0x98,0x03,0xae,0xe3,0x28,0x09,0x1b,0x58,
    0xfa,0xb3,0x24,0xe4,0xfa,0xd6,0x75,0x94,0x55,0x85,0x80,0x8b,0x48,0x31,0xd7,0xbc,
    0x3f,0xf4,0xde,0xf0,0x8e,0x4b,0x7a,0x9d,0xe5,0x76,0xd2,0x65,0x86,0xce,0xc6,0x4b,
    0x61,0x16 };
static const uint8_t RFC_TAG[16] = {
    0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91 };

static void test_vecteur_rfc(void)
{
    check(strlen(RFC_CLAIR) == 114, "the RFC plaintext really is 114 bytes");

    uint8_t other[32]; memset(other, 0x11, sizeof other);
    shadow_cipher *c = shadow_cipher_create(other, RFC_KEY);   /* clef RFC en RECEPTION */
    check(c != NULL, "cipher created with distinct Tx and Rx keys");

    uint8_t buf[114];
    memcpy(buf, RFC_CHIFFRE, sizeof buf);
    check(shadow_cipher_decrypt_aad_unsafe(c, buf, (int)sizeof buf,
                                           RFC_NONCE, RFC_TAG, RFC_AAD, (int)sizeof RFC_AAD),
          "RFC 8439 §2.8.2 vector: a valid tag");
    check(memcmp(buf, RFC_CLAIR, 114) == 0,
          "RFC 8439 §2.8.2 vector: plaintext recovered to the byte");

    /* One bit changed in the TAG must invalidate. */
    uint8_t tag_faux[16]; memcpy(tag_faux, RFC_TAG, 16); tag_faux[0] ^= 0x01;
    memcpy(buf, RFC_CHIFFRE, sizeof buf);
    check(!shadow_cipher_decrypt_aad_unsafe(c, buf, (int)sizeof buf,
                                            RFC_NONCE, tag_faux, RFC_AAD, (int)sizeof RFC_AAD),
          "one bit changed in the tag invalidates");

    /* One bit changed in the CIPHERTEXT too. */
    memcpy(buf, RFC_CHIFFRE, sizeof buf); buf[50] ^= 0x80;
    check(!shadow_cipher_decrypt_aad_unsafe(c, buf, (int)sizeof buf,
                                            RFC_NONCE, RFC_TAG, RFC_AAD, (int)sizeof RFC_AAD),
          "one bit changed in the ciphertext invalidates");

    /* One bit changed in the AAD too - that is the whole point of the AAD. */
    uint8_t aad_faux[12]; memcpy(aad_faux, RFC_AAD, 12); aad_faux[3] ^= 0x01;
    memcpy(buf, RFC_CHIFFRE, sizeof buf);
    check(!shadow_cipher_decrypt_aad_unsafe(c, buf, (int)sizeof buf,
                                            RFC_NONCE, RFC_TAG, aad_faux, (int)sizeof aad_faux),
          "one bit changed in the AAD invalidates");

    /* Nonce different : invalide. */
    uint8_t nonce_faux[12]; memcpy(nonce_faux, RFC_NONCE, 12); nonce_faux[11] ^= 0x01;
    memcpy(buf, RFC_CHIFFRE, sizeof buf);
    check(!shadow_cipher_decrypt_aad_unsafe(c, buf, (int)sizeof buf,
                                            nonce_faux, RFC_TAG, RFC_AAD, (int)sizeof RFC_AAD),
          "a different nonce invalidates");

    /* WITHOUT the AAD while the vector has one: must fail. This is the
     * guarantee that the AAD is taken into account and not silently ignored -
     * the Shadow wire does not use one, and confusing the two variants would
     * make 100 % of decryptions fail with nothing to say why. */
    memcpy(buf, RFC_CHIFFRE, sizeof buf);
    check(!shadow_cipher_decrypt_unsafe(c, buf, (int)sizeof buf, RFC_NONCE, RFC_TAG),
          "the AAD-less variant refuses a message that carried one");

    shadow_cipher_destroy(c);
}

static void test_wire_format(void)
{
    uint8_t k[32]; for (int i = 0; i < 32; i++) k[i] = (uint8_t)(i * 3 + 7);
    shadow_cipher *c = shadow_cipher_create(k, k);

    const char *msg = "halyard";
    const int   len = (int)strlen(msg);
    uint8_t buf[64]; memset(buf, 0, sizeof buf);
    memcpy(buf, msg, len);

    int total_len = shadow_cipher_encrypt(c, buf, len);
    check(total_len == len + 28, "encrypt returns len + 28 (nonce 12 + tag 16)");
    check(memcmp(buf, msg, len) != 0, "the plaintext no longer appears at the head");

    /* The announced format is [ciphertext][nonce 12][tag 16]: we check it by
     * decrypting with the nonce and the tag read AT THE ANNOUNCED OFFSETS. */
    shadow_cipher *d = shadow_cipher_create(k, k);
    check(shadow_cipher_decrypt_unsafe(d, buf, len, buf + len, buf + len + 12),
          "nonce a l'offset len, tag a l'offset len+12");
    check(memcmp(buf, msg, len) == 0, "round trip: the plaintext is recovered");
    shadow_cipher_destroy(d);

    /* decrypt_wire must do the same decomposition on its own. */
    memset(buf, 0, sizeof buf); memcpy(buf, msg, len);
    total_len = shadow_cipher_encrypt(c, buf, len);
    shadow_cipher *e = shadow_cipher_create(k, k);
    check(shadow_cipher_decrypt_wire(e, buf, total_len), "decrypt_wire accepts the message");
    check(memcmp(buf, msg, len) == 0, "decrypt_wire recovers the plaintext");
    check(!shadow_cipher_decrypt_wire(e, buf, 27), "a message shorter than 28 is refused");
    shadow_cipher_destroy(e);

    shadow_cipher_destroy(c);
}

static void test_nonce_monotone(void)
{
    uint8_t k[32]; memset(k, 0x5A, sizeof k);
    shadow_cipher *c = shadow_cipher_create(k, k);

    uint8_t a[40] = {0}, b[40] = {0};
    shadow_cipher_encrypt(c, a, 4);
    shadow_cipher_encrypt(c, b, 4);

    /* The second message's nonce must equal the first's + 1, little-endian. A
     * nonce reused with the same key breaks ChaCha20-Poly1305 outright: this is
     * not a comfort detail. */
    const uint8_t *n1 = a + 4, *n2 = b + 4;
    uint8_t expected[12]; memcpy(expected, n1, 12);
    for (int i = 0; i < 12; i++) { if (++expected[i] != 0) break; }
    check(memcmp(n2, expected, 12) == 0, "the nonce increments by 1, little-endian");
    check(memcmp(n1, n2, 12) != 0, "two messages never share a nonce");

    shadow_cipher_destroy(c);
}

static void test_replay_and_keys(void)
{
    uint8_t k[32]; memset(k, 0x33, sizeof k);

    /* Trois messages emis, donc trois nonces croissants. */
    shadow_cipher *tx = shadow_cipher_create(k, k);
    uint8_t m1[40] = {0}, m2[40] = {0};
    memcpy(m1, "aaaa", 4); memcpy(m2, "bbbb", 4);
    int l1 = shadow_cipher_encrypt(tx, m1, 4);
    int l2 = shadow_cipher_encrypt(tx, m2, 4);
    check(l1 == 32 && l2 == 32, "deux messages chiffres");

    /* The variant with replay protection requires strictly increasing
     * nonces. */
    shadow_cipher *rx = shadow_cipher_create(k, k);
    uint8_t c2[40]; memcpy(c2, m2, sizeof c2);
    check(shadow_cipher_decrypt_wire(rx, c2, l2), "the 2nd message is accepted");
    uint8_t c1[40]; memcpy(c1, m1, sizeof c1);
    check(!shadow_cipher_decrypt_wire(rx, c1, l1),
          "the 1st message, arriving after, is refused (a smaller nonce)");
    memcpy(c2, m2, sizeof c2);
    check(!shadow_cipher_decrypt_wire(rx, c2, l2), "the same message replayed is refused");
    shadow_cipher_destroy(rx);

    /* The `unsafe` variant accepts disorder: that is what UDP needs, where the
     * frames arrive in any order. */
    shadow_cipher *ru = shadow_cipher_create(k, k);
    memcpy(c2, m2, sizeof c2);
    check(shadow_cipher_decrypt_unsafe(ru, c2, 4, c2 + 4, c2 + 16), "unsafe : 2e message");
    memcpy(c1, m1, sizeof c1);
    check(shadow_cipher_decrypt_unsafe(ru, c1, 4, c1 + 4, c1 + 16),
          "unsafe: the 1st message accepted after the 2nd (UDP disorder)");
    shadow_cipher_destroy(ru);

    /* The Tx and Rx keys really are distinct: encrypting with one and
     * decrypting with the other must fail. */
    uint8_t k2[32]; memset(k2, 0x44, sizeof k2);
    shadow_cipher *croise = shadow_cipher_create(k, k2);   /* Rx != Tx */
    uint8_t m3[40] = {0}; memcpy(m3, "cccc", 4);
    int l3 = shadow_cipher_encrypt(croise, m3, 4);
    check(l3 == 32, "a message encrypted with the Tx key");
    check(!shadow_cipher_decrypt_wire(croise, m3, l3),
          "decrypting with the Rx key fails: the two directions really are separate");
    shadow_cipher_destroy(croise);

    shadow_cipher_destroy(tx);
}

static void test_degenerate_inputs(void)
{
    uint8_t k[32] = {0};
    check(shadow_cipher_create(NULL, k) == NULL, "clef Tx nulle refusee");
    check(shadow_cipher_create(k, NULL) == NULL, "clef Rx nulle refusee");
    shadow_cipher_destroy(NULL);            /* must not crash */

    shadow_cipher *c = shadow_cipher_create(k, k);
    uint8_t buf[64] = {0};
    check(shadow_cipher_encrypt(NULL, buf, 4) == -1, "encrypt with no cipher is refused");
    check(shadow_cipher_encrypt(c, NULL, 4) == -1, "encrypt with no buffer is refused");
    check(shadow_cipher_encrypt(c, buf, -1) == -1, "encrypt with a negative length is refused");
    check(!shadow_cipher_decrypt_unsafe(c, buf, -1, buf, buf), "decrypt with a negative length is refused");
    check(!shadow_cipher_decrypt_unsafe(c, buf, 4, NULL, buf), "decrypt with no nonce is refused");
    check(!shadow_cipher_decrypt_unsafe(c, buf, 4, buf, NULL), "decrypt with no tag is refused");
    check(!shadow_cipher_decrypt_wire(c, buf, 0), "decrypt_wire of an empty message is refused");

    /* An empty but authenticated message: 0 bytes of plaintext, 28 of envelope. */
    uint8_t empty[32] = {0};
    int n = shadow_cipher_encrypt(c, empty, 0);
    check(n == 28, "an empty message is 28 bytes on the wire");
    shadow_cipher *d = shadow_cipher_create(k, k);
    check(shadow_cipher_decrypt_wire(d, empty, 28), "and it authenticates correctly");
    shadow_cipher_destroy(d);

    shadow_cipher_destroy(c);
}

int main(void)
{
    printf("== chacha20-poly1305 (the RFC 8439 vector) ==\n");
    test_vecteur_rfc();
    test_wire_format();
    test_nonce_monotone();
    test_replay_and_keys();
    test_degenerate_inputs();
    printf("%d checks, %d failure(s)\n", total, failed);
    if (!failed) printf("OK\n");
    return failed ? 1 : 0;
}
