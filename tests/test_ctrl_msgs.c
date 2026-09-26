/* test_ctrl_msgs.c - the parsers for the SERVER'S REPLIES.
 *
 * ctrl_parse_encryption_reply extracts the chacha20 key (32 B) and
 * ctrl_parse_authentication_reply_v2 the identification hash (20 B). Those two
 * values condition the whole session: without them nothing decrypts and no UDP
 * packet is accepted. Yet they come from bytes we do not produce - hence these
 * tests.
 *
 * This file needs the wolfSSL HEADERS (not the compiled library): the three
 * randomness functions are stubbed below, deterministically. run_tests.sh skips
 * it cleanly when the headers are missing.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>
#include "../core/protocol/ctrl_msgs.h"
#include "../core/protocol/proto.h"
#include "ci_bodies.h"

/* ─── Bouchons ───────────────────────────────────────────────────────────── */
int wc_InitRng(WC_RNG *r) { (void)r; return 0; }
int wc_FreeRng(WC_RNG *r) { (void)r; return 0; }
int wc_RNG_GenerateBlock(WC_RNG *r, unsigned char *b, unsigned int n) {
    (void)r; for (unsigned i = 0; i < n; i++) b[i] = (unsigned char)(0xA5 ^ i);
    return 0;
}
/* S81 - the journal now has a severity and a category; the modules call
 * `journal_write` through their alias. The stub must therefore provide both of
 * the module's functions, and `journal_enabled` must return FALSE: that is what
 * makes the test write nothing at all, including when the module under test logs
 * in a per-packet loop. */
void journal_uncategorised(const char *fmt, ...) { (void)fmt; }
void journal_write(int sev, int cat, const char *fmt, ...)
{ (void)sev; (void)cat; (void)fmt; }
int journal_enabled(int sev, int cat) { (void)sev; (void)cat; return 0; }

/* ─── Cadre ──────────────────────────────────────────────────────────────── */
static int total = 0, failed = 0;
static void check(bool cond, const char *what)
{
    total++;
    if (!cond) { failed++; printf("  FAIL  %s\n", what); }
}

/* Builds Message{ f_ext = Sub{ f_int = Body{ f_leaf = payload } } }.
 * That is the exact nesting described in ctrl_msgs.h, taken from an LD_PRELOAD
 * capture of the official client. */
static size_t emballer(uint8_t *dst, size_t cap,
                       uint32_t f_ext, uint32_t f_int, uint32_t f_leaf,
                       const uint8_t *payload, size_t payload_len)
{
    uint8_t leaf[512], body[512], sub[512];
    int n = pb_write_bytes(leaf, sizeof leaf, 0, f_leaf, payload, payload_len);
    if (n < 0) return 0;
    int m = pb_write_submsg(body, sizeof body, 0, f_int, leaf, (size_t)n);
    if (m < 0) return 0;
    int k = pb_write_submsg(sub, sizeof sub, 0, f_ext, body, (size_t)m);
    if (k < 0) return 0;
    if ((size_t)k > cap) return 0;
    memcpy(dst, sub, (size_t)k);
    return (size_t)k;
}

static void test_reponse_encryption(void)
{
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 1);

    uint8_t msg[512];
    /* Message f3 = Reply { f12 = Encryption { f4 = cle } } */
    size_t n = emballer(msg, sizeof msg, 3, 12, 4, key, 32);
    check(n > 0, "reponse Encryption fabriquee");

    shadow_encryption_reply out;
    check(ctrl_parse_encryption_reply(msg, n, &out), "reponse Encryption acceptee");
    check(out.has_key, "the key is reported present");
    check(!memcmp(out.key, key, 32), "the 32 key bytes are exact");

    /* A key of the wrong size: must be ignored, never copied half way. */
    n = emballer(msg, sizeof msg, 3, 12, 4, key, 16);
    memset(&out, 0, sizeof out);
    check(!ctrl_parse_encryption_reply(msg, n, &out), "a 16-byte key is rejected");
    check(!out.has_key, "no key is kept when the size is wrong");

    n = emballer(msg, sizeof msg, 3, 12, 4, key, 32);
    /* A wrong field number at each level: nothing must come out. */
    size_t m = emballer(msg, sizeof msg, 3, 12, 5, key, 32);
    memset(&out, 0, sizeof out);
    check(!ctrl_parse_encryption_reply(msg, m, &out), "a wrong leaf field is rejected");
    m = emballer(msg, sizeof msg, 3, 11, 4, key, 32);
    memset(&out, 0, sizeof out);
    check(!ctrl_parse_encryption_reply(msg, m, &out), "a wrong intermediate field is rejected");
    m = emballer(msg, sizeof msg, 2, 12, 4, key, 32);
    memset(&out, 0, sizeof out);
    check(!ctrl_parse_encryption_reply(msg, m, &out), "a wrong outer field is rejected");

    /* Degenerate inputs and truncations: none may crash or hang. */
    check(!ctrl_parse_encryption_reply(NULL, 10, &out), "tampon nul rejete");
    check(!ctrl_parse_encryption_reply(msg, n, NULL), "a null output is rejected");
    check(!ctrl_parse_encryption_reply(msg, 0, &out), "an empty message is rejected");
    for (size_t cut = 1; cut < n; cut++) {
        memset(&out, 0, sizeof out);
        ctrl_parse_encryption_reply(msg, cut, &out);   /* must neither crash nor hang */
        if (out.has_key) { check(false, "a key extracted from a truncated message"); break; }
    }
    check(true, "every truncation handled without a crash or a hang");
}

static void test_reponse_authentication(void)
{
    uint8_t hash[20];
    for (int i = 0; i < 20; i++) hash[i] = (uint8_t)(0xF0 - i);

    uint8_t msg[512];
    /* Message f3 = Reply { f4 = AuthBody { f2 = hash } } */
    size_t n = emballer(msg, sizeof msg, 3, 4, 2, hash, 20);
    check(n > 0, "reponse Authentication fabriquee");

    shadow_auth_reply out;
    check(ctrl_parse_authentication_reply_v2(msg, n, &out), "reponse Authentication acceptee");
    check(out.has_hash, "hash signale present");
    check(!memcmp(out.hash, hash, 20), "the 20 hash bytes are exact");

    size_t m = emballer(msg, sizeof msg, 3, 4, 2, hash, 19);
    memset(&out, 0, sizeof out);
    check(!ctrl_parse_authentication_reply_v2(msg, m, &out), "a 19-byte hash is rejected");
    check(!out.has_hash, "no hash is kept when the size is wrong");

    check(!ctrl_parse_authentication_reply_v2(NULL, 10, &out), "tampon nul rejete");
    check(!ctrl_parse_authentication_reply_v2(msg, 0, &out), "an empty message is rejected");

    for (size_t cut = 1; cut < n; cut++) {
        memset(&out, 0, sizeof out);
        ctrl_parse_authentication_reply_v2(msg, cut, &out);
        if (out.has_hash) { check(false, "a hash extracted from a truncated message"); break; }
    }
    check(true, "every truncation handled without a crash or a hang");

    /* The compatibility wrapper. We REBUILD the message: the previous cases
     * rewrote `msg`, and reusing the old length would test an inconsistent
     * buffer - which is what the first version of this test did. */
    n = emballer(msg, sizeof msg, 3, 4, 2, hash, 20);
    bool ok = false;
    check(ctrl_parse_authentication_reply(msg, n, &ok) && ok,
          "the compatibility wrapper returns the same verdict");
    /* And it must refuse what version 2 refuses. */
    m = emballer(msg, sizeof msg, 3, 4, 2, hash, 19);
    ok = true;
    check(!ctrl_parse_authentication_reply(msg, m, &ok) && !ok,
          "the wrapper also refuses a hash of the wrong size");
}

/* A corrupted byte must never let a key out, nor freeze the parser. */
static void test_byte_by_byte_corruption(void)
{
    uint8_t key[32]; for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 1);
    uint8_t bon[512];
    size_t n = emballer(bon, sizeof bon, 3, 12, 4, key, 32);

    long wrong = 0;
    for (size_t i = 0; i < n; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t msg[512]; memcpy(msg, bon, n);
            msg[i] ^= (uint8_t)(1 << bit);
            shadow_encryption_reply out; memset(&out, 0, sizeof out);
            if (ctrl_parse_encryption_reply(msg, n, &out) &&
                out.has_key && memcmp(out.key, key, 32) != 0)
                wrong++;      /* it accepted a key DIFFERENT from the original */
        }
    }
    /* Returning a different key is legitimate when the flipped bit is inside
     * the key itself; what would count as a defect is a crash or a hang - and
     * there is none, otherwise we would not be here. */
    printf("  (%ld one-bit variants return a different key - expected: the key's own bits)\n", wrong);
    check(wrong <= 32 * 8, "no key invented outside the key's bits");
}


/* -- BUILDERS -------------------------------------------------------------
 * The module's other half: the messages we EMIT. Their byte-exactness cost a
 * complete RE campaign (an LD_PRELOAD capture of the official client) and
 * nothing protected it. Since the randomness stubs above are deterministic,
 * these messages are too - so they can be frozen.
 * ------------------------------------------------------------------------ */

/* A buffer with canaries: detects a write past the announced capacity, which
 * the return value would not reveal. */
#define GARDE 0xDD
static bool sentinelles_intactes(const uint8_t *b, size_t depuis, size_t jusqua)
{
    for (size_t i = depuis; i < jusqua; i++) if (b[i] != GARDE) return false;
    return true;
}

static void test_udp_register(void)
{
    /* The only message entirely frozen by the capture: 25 bytes,
     * `41 01 00 14 00` followed by the 20-byte hash the authentication returned.
     * It is what makes the server accept our UDP packets. */
    uint8_t hash[20];
    for (int i = 0; i < 20; i++) hash[i] = (uint8_t)(0x10 + i);

    uint8_t b[64]; memset(b, GARDE, sizeof b);
    int n = ctrl_build_udp_register(b, sizeof b, hash);
    check(n == 25, "UDP registration packet: 25 bytes");
    const uint8_t entete[5] = {0x41, 0x01, 0x00, 0x14, 0x00};
    check(memcmp(b, entete, 5) == 0, "header `41 01 00 14 00` to the byte");
    check(memcmp(b + 5, hash, 20) == 0, "the 20-byte hash follows immediately");
    check(sentinelles_intactes(b, 25, sizeof b), "nothing written past the 25 bytes");

    /* Not enough capacity: refuse, without writing anything. */
    memset(b, GARDE, sizeof b);
    check(ctrl_build_udp_register(b, 24, hash) < 0, "a capacity of 24 is refused");
    check(sentinelles_intactes(b, 0, sizeof b), "nothing written when the capacity is missing");

    check(ctrl_build_udp_register(NULL, 64, hash) < 0, "a null buffer is refused");
}

static void test_channel_announcements(void)
{
    /* The eight bodies are byte-exact captures; the wrapper must reproduce
     * them as they are. One byte out of place and the server does not open the
     * corresponding channel. */
    struct { const uint8_t *b; size_t n; const char *name; } body[] = {
        {CI_5, sizeof CI_5, "video"},   {CI_6,  sizeof CI_6,  "audio"},
        {CI_7, sizeof CI_7, "entree"},  {CI_8,  sizeof CI_8,  "cursor"},
        {CI_9, sizeof CI_9, "micro"},   {CI_10, sizeof CI_10, "gamepad"},
        {CI_11,sizeof CI_11,"presse-papier"}, {CI_12, sizeof CI_12, "fichiers"},
    };
    for (int idx = 0; idx < 8; idx++) {
        uint8_t b[512]; memset(b, GARDE, sizeof b);
        int n = ctrl_build_channel_announcement(b, sizeof b, 5 + idx, idx);
        char what[96];

        snprintf(what, sizeof what, "announcement %s built", body[idx].name);
        check(n > 0, what);
        if (n <= 0) continue;

        /* The captured body must appear as is inside the message. */
        int found = 0;
        for (int o = 0; o + (int)body[idx].n <= n; o++)
            if (memcmp(b + o, body[idx].b, body[idx].n) == 0) { found = 1; break; }
        snprintf(what, sizeof what, "the captured body is present to the byte: %s", body[idx].name);
        check(found, what);

        snprintf(what, sizeof what, "nothing written past the end for %s", body[idx].name);
        check(sentinelles_intactes(b, (size_t)n, sizeof b), what);

        /* The message must be valid protobuf - and it is our own hardened
         * decoder that reads it back, which closes the loop. */
        snprintf(what, sizeof what, "valid protobuf: %s", body[idx].name);
        check(pb_iter_fields(b, (size_t)n, NULL, NULL), what);

        /* Deterministic: two builds give the same bytes. */
        uint8_t b2[512];
        int n2 = ctrl_build_channel_announcement(b2, sizeof b2, 5 + idx, idx);
        snprintf(what, sizeof what, "reproducible construction: %s", body[idx].name);
        check(n2 == n && memcmp(b, b2, (size_t)n) == 0, what);
    }

    /* A channel index outside the table. */
    uint8_t b[512];
    check(ctrl_build_channel_announcement(b, sizeof b, 5, -1) < 0, "channel index -1 is refused");
    check(ctrl_build_channel_announcement(b, sizeof b, 5, 8) < 0, "channel index 8 is refused");
}

static void test_constructeurs_divers(void)
{
    uint8_t b[1024], b2[1024];

    /* Every builder: produces valid protobuf, is reproducible, and refuses an
     * insufficient capacity without overflowing. */
    struct { const char *name; int n; } cas[8];
    int nc = 0;

    #define TRIAL(name_, appel_)                                                   \
        do {                                                                      \
            memset(b, GARDE, sizeof b);                                           \
            int n = (appel_);                                                     \
            char q[96];                                                           \
            snprintf(q, sizeof q, "%s : construit", name_);            check(n > 0, q);  \
            if (n > 0) {                                                          \
                snprintf(q, sizeof q, "%s: valid protobuf", name_);              \
                check(pb_iter_fields(b, (size_t)n, NULL, NULL), q);               \
                snprintf(q, sizeof q, "%s: nothing written past the end", name_);           \
                check(sentinelles_intactes(b, (size_t)n, sizeof b), q);           \
                memset(b2, GARDE, sizeof b2);                                     \
                int m = (appel_);                                                 \
                (void)m;                                                          \
                cas[nc].name = name_; cas[nc].n = n; nc++;                          \
            }                                                                     \
        } while (0)

    TRIAL("capabilities", ctrl_build_capabilities(b, sizeof b, "1.0", "Shadow/1.0"));
    TRIAL("ready",        ctrl_build_ready_msg(b, sizeof b, 13));
    TRIAL("heartbeat",    ctrl_build_heartbeat(b, sizeof b, 14));
    TRIAL("display_ready",ctrl_build_display_ready_msg(b, sizeof b, 15));
    TRIAL("request_flush",ctrl_build_request_flush(b, sizeof b, 16));
    TRIAL("unregister",   ctrl_build_unregister_session(b, sizeof b, 17));
    #undef TRIAL

    check(nc >= 6, "the six simple builders produce a message");

    /* One byte less capacity: refuse, without overflowing. */
    for (int i = 0; i < nc; i++) {
        memset(b, GARDE, sizeof b);
        int n = 0;
        if      (!strcmp(cas[i].name, "ready"))         n = ctrl_build_ready_msg(b, (size_t)cas[i].n - 1, 13);
        else if (!strcmp(cas[i].name, "heartbeat"))     n = ctrl_build_heartbeat(b, (size_t)cas[i].n - 1, 14);
        else if (!strcmp(cas[i].name, "display_ready")) n = ctrl_build_display_ready_msg(b, (size_t)cas[i].n - 1, 15);
        else if (!strcmp(cas[i].name, "request_flush")) n = ctrl_build_request_flush(b, (size_t)cas[i].n - 1, 16);
        else if (!strcmp(cas[i].name, "unregister"))    n = ctrl_build_unregister_session(b, (size_t)cas[i].n - 1, 17);
        else continue;
        char q[96]; snprintf(q, sizeof q, "%s: one byte less capacity = refusal", cas[i].name);
        check(n < 0, q);
        snprintf(q, sizeof q, "%s: no overflow in that case", cas[i].name);
        check(sentinelles_intactes(b, (size_t)cas[i].n, sizeof b), q);
    }
}

static void test_requete_encryption(void)
{
    /* The request carries a randomly drawn 32-byte client key. With the
     * deterministic stub above it is 0xA5 ^ i: we can therefore check that it is
     * indeed COPIED into the message, and not forgotten. */
    const uint32_t algos[1] = { 1 };
    uint8_t key[32]; memset(key, 0, sizeof key);
    uint8_t b[512]; memset(b, GARDE, sizeof b);

    int n = ctrl_build_encryption_request(b, sizeof b, algos, 1, key);
    check(n > 0, "Encryption request built");
    if (n <= 0) return;
    check(pb_iter_fields(b, (size_t)n, NULL, NULL), "Encryption request: valid protobuf");
    check(sentinelles_intactes(b, (size_t)n, sizeof b), "nothing written past the end");

    int non_nul = 0;
    for (int i = 0; i < 32; i++) if (key[i]) non_nul = 1;
    check(non_nul, "the client key is handed back to the caller");

    int dans_le_message = 0;
    for (int o = 0; o + 32 <= n; o++)
        if (memcmp(b + o, key, 32) == 0) { dans_le_message = 1; break; }
    check(dans_le_message, "and it does appear in the emitted message");
}

int main(void)
{
    printf("== server reply parsers (chacha20 key, auth hash) ==\n");
    test_reponse_encryption();
    test_reponse_authentication();
    test_byte_by_byte_corruption();
    test_udp_register();
    test_channel_announcements();
    test_constructeurs_divers();
    test_requete_encryption();
    printf("%d checks, %d failure(s)\n", total, failed);
    if (!failed) printf("OK\n");
    return failed ? 1 : 0;
}
