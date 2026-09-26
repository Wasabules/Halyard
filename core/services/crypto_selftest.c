/* crypto_selftest - does this libcrypto COMPUTE CORRECTLY, and how fast?
 *
 * === WHY THIS FILE EXISTS =============================================
 *
 * Measured on console 2026-09-13: a TLS handshake costs 402 to 1846 ms on this
 * machine, twelve times per bootstrap. The cause is established -- the vitasdk
 * libcrypto is built as generic C, `nm` does not even find `bn_mul_mont` in
 * it. The obvious fix is to rebuild it with the ARM assembly, and it was
 * attempted: the library builds, the symbols are there, the client links...
 * and EVERY TLS connection fails on console.
 *
 * The problem was not the failure, it was the LOOP: each attempt cost a
 * deployment and a session, and left the console with no network in between.
 * You cannot hunt a wrong computation with an instrument that takes two
 * minutes to say "no" and breaks the device while it does.
 *
 * This bench answers both questions with no network, no session, and nothing
 * broken:
 *   - CORRECT? a modexp with a known answer. That is exactly the operation
 *     `bn_mul_mont` dominates, so assembly that lies is caught right here.
 *   - HOW FAST? the same computation, timed, plus a P-256 point
 *     multiplication. Those two numbers PREDICT what a handshake costs, so
 *     they compare two libcryptos without opening a connection.
 *
 * `SHADOW_CRYPTO_SELFTEST=1` in env.txt triggers it at startup. It is
 * triggered BY FILE rather than on by default, like autotest.txt: a bench that
 * runs by itself on a user's machine is time stolen from every launch.
 */

#include "crypto_selftest.h"
#include "journal.h"

/* === WHICH LIBRARY IT BENCHES, AND WHY THAT IS NOT A PLATFORM TEST =====
 *
 * It measures the library the REST path actually uses - libcurl's TLS stack:
 *   - desktop: the system's OpenSSL;
 *   - PS Vita: Mbed TLS since 2026-09-26. It was OpenSSL 1.0.2, the vitasdk's,
 *     which the GPL does not allow in a redistributed binary; libcurl is now
 *     rebuilt on Mbed TLS (tools/build-libs.sh vita curl), and CMake says so by
 *     defining SHADOW_REST_TLS_MBEDTLS. The same vectors run on both arms, so
 *     the Vita's before (OpenSSL: modexp 5.9 ms, P-256 12.9 ms) and after are
 *     directly comparable;
 *   - Switch: none. Its libcurl talks to the console's own TLS service, and
 *     there is no library in the binary to interrogate;
 *   - Windows: none either - it links no libcrypto of its own (MSYS2's curl
 *     brings its TLS in a DLL), and including <openssl/bn.h> there left
 *     BN_* unresolved at link time.
 *
 * The condition names the DEPENDENCY, and CMake - which knows what it links -
 * is what declares it: SHADOW_REST_TLS_OPENSSL or SHADOW_REST_TLS_MBEDTLS.
 * A target that declares neither gets the silent stub. */
#if defined(SHADOW_REST_TLS_MBEDTLS)
#  define SHADOW_BENCH_LIB 2        /* Mbed TLS */
#elif defined(SHADOW_REST_TLS_OPENSSL)
#  define SHADOW_BENCH_LIB 1        /* OpenSSL */
#else
#  define SHADOW_BENCH_LIB 0        /* nothing linked to bench */
#endif

#if SHADOW_BENCH_LIB != 0

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if SHADOW_BENCH_LIB == 1
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#else
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/version.h>
#endif

#define clog(...) JOURNAL_INFO_(JOURNAL_CAT_NETWORK, __VA_ARGS__)

/* Vector generated on the host: 2^65537 mod p, p a 1024-bit prime. The
 * expected value is computed SOMEWHERE OTHER than the library under test -- a
 * test that asks an implementation to confirm its own output tests nothing. */
static const char *MOD_HEX =
    "c4f8d7e3a1b25c9f0e6d4a8b3c7159e2f0a6d5c8b4937e1a2f5c806d3b9e475a"
    "1c8f2e6b09d3745a8c1f2e5b6d09437c8a1e2f5b6d094a3c8f1e2b5d609473a8"
    "c1f5e2b6d0943a7c81f2e5b6d0943c7a81f2e5b6d0943ca781f2e5b6d0943ca8"
    "71f2e5b6d0943ca87f12e5b6d0943ca871fe25b6d0943ca871f2e5b6d1f";
static const char *EXPECTED_HEX =
    "8a0b652e4d09e25f1dfa3f7becb9051d09c82667484c9b0f39fe274d8bdd5be2"
    "6ca44f303cceb493b84af206aa5f1a8f911b04b0d4656abad74f4ef973557174"
    "8788d241a2831d518eeee38c96ba99d7429ce1990aee4d47852b11ea94611740"
    "822de766d2c4375e1b38542738fa3aee721a3083134bae36f2187c40337";

/* === RSA-4096, BECAUSE THAT IS WHAT THE CHAIN ASKS FOR =================
 *
 * Timestamped handshake trace: 680 ms elapse between the Certificate message
 * and the Server key exchange, two messages RECEIVED in the SAME flight --
 * so processing, not network. The chain the Shadow endpoints present is Let's
 * Encrypt, and its ISRG Root X1 root is a 4096-bit RSA key. Verifying the
 * intermediate's signature against that root is a 4096-bit modexp.
 *
 * The 1024-bit vector measures 5.9 ms. This one says what the real size costs
 * -- and if the figure lands near 680 ms, the cause is named without having to
 * instrument OpenSSL. */
static const char *MOD4096_HEX =
    "941db47e0d173cd58ce397cb671929950171af825f111ddca8362ae229d382f7"
    "5b40b8b61dfc04963f664f9bd84c54682d230dd6867e05c3e4159c04cc41286a"
    "ad715e71649d43a3347509d0c7070010c87a5b4214c52dd6a4437b510feed90e"
    "1c526fa288d1ba25d081fc72d2827d3f314f694e6eb86d161e934775a4615053"
    "be8b1fb524fdd022a384f6c5511bde88eeb72a475a25b6fe5b33de08bf8d1c7b"
    "3b0286b8d16f735c0520d17e5ab02f4c588e105ed45573b496d29662014419cd"
    "b3a562d57faed202e4b040522f7918780806f194c9c157ee1267e8703e747ec7"
    "dc612d84a2601396480ee99b97370571e5f6d2653ceaa79ffde37a361af26d2e"
    "ca0f30687bbc4c3fcdf274f75e0edf87efdcc1fa31632fa427d9053a8cc5d5da"
    "3f1ab136d224c5ff34aca7a5641c52014d8cbe678c2698f960bb0d42df447dcd"
    "1df7a3a90a400566672f1a5802d68a0768cb4eb47a81be7b8307a4476c6567f6"
    "9115b64864d1bfcb88f5aa198a16cae56504ff85679e4d5fde590bc3b21b8583"
    "fb750c6d7de289469e4f2046d0e10aec80a354e803e7131f523ec106352227ac"
    "2d4742d953188542885d75fec2106682765eeab69249b95a0b0425e87453d97a"
    "fca1105f4419828be5097a70550da58bb1d6a1ff12dbe54b35a4891f8b61a2dd"
    "5e715f60b9918322ff7a018ecc5650304098906e50c76a0448ef6621f3480479"
    ;
static const char *EXP4096_HEX =
    "61134a744a3c051077d1db6421c338c87974b6cbe0e7b3ece54730c58de01bc8"
    "620bec4ff412b8ea792a8c6b97c9dc43bf0ac69f2ae2c3e8654d42f079c02d34"
    "2ddd16341a5a7f127299d7feab63213246a878294511dd19e92b74475ca05c52"
    "1f4d502dc0f254a6a2369a3e6f8f0d357882a80e3399595662a0145e98894dd0"
    "edeb717e52ea0c3467585aca7d849c7e4c96d422def438c053ff66fcb47e3d82"
    "9695886237804bb074a57b0c426a7086b063518bf69d441b97f184ab8ae992bd"
    "a948cfcffa965b37eecfc41507a44ec0dcfac7d6a7df1ed2ca308795d798ea83"
    "de9f2152cc45a32a6df1f45332ff2a80086796cf288ffa52c7e61e43b1e148ca"
    "e5efd6180ce82ae916b8a0e25a1d53ba8deae5fd8d8d6bd3e9762c277b59f69c"
    "ed803227e94468be413d6e5faa0ea6c2b0d16a7d6120e1a3403a4cd188cc6cbd"
    "feeececd1c07804f97b619756d5307b2bc2cf923814e5e406ef3f796fa5698cd"
    "1785c2bfa672c52117b7f5b232fb531634aaeae015eb2c83604976c38b8f21b8"
    "d827b50975ae864ab5731eaea5d646d61c18d6f8738e0f084ab49fb0396870a6"
    "36d275d5a6fb4faff20bd38fc2b7e729aab7f113fc529209347686dd4e48d90d"
    "2475c9bd94be4fd1e8bd5801104d158e8002fc8ad83e2a186fac7f39bd3f9c2b"
    "5c9ec6a078c35f2e1b8a16de4dd6d3c6dc37f7d37798916a10c293458ab166f8"
    ;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#if SHADOW_BENCH_LIB == 1

int shadow_crypto_selftest(void)
{
    const char *e = getenv("SHADOW_CRYPTO_SELFTEST");
    if (!e || atoi(e) == 0) return 0;

    clog("[CRYPTO] bench: does the library compute correctly, and how fast? (%s)",
         OpenSSL_version(OPENSSL_VERSION));

    int bad = 0;
    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *m = NULL, *b = NULL, *x = NULL, *r = NULL, *want = NULL;
    if (!ctx) { clog("[CRYPTO] BN_CTX_new FAIL"); return 1; }
    BN_hex2bn(&m, MOD_HEX);
    BN_hex2bn(&want, EXPECTED_HEX);
    b = BN_new(); x = BN_new(); r = BN_new();
    if (!m || !want || !b || !x || !r) { clog("[CRYPTO] allocation FAIL"); return 1; }
    BN_set_word(b, 2);
    BN_set_word(x, 65537);

    /* CORRECTNESS first. Timing a wrong answer teaches nothing, and the
     * reverse order is how a speed-up that lies gets published. */
    if (!BN_mod_exp(r, b, x, m, ctx)) {
        clog("[CRYPTO] modexp: the call FAILED"); bad++;
    } else if (BN_cmp(r, want) != 0) {
        char *got = BN_bn2hex(r);
        clog("[CRYPTO] modexp: WRONG RESULT - the library lies");
        clog("[CRYPTO]   got      %.32s...", got ? got : "?");
        clog("[CRYPTO]   expected %.32s...", EXPECTED_HEX);
        if (got) OPENSSL_free(got);
        bad++;
    } else {
        clog("[CRYPTO] modexp 1024 bits: CORRECT");
    }

    /* HOW FAST. Ten rounds: a single modexp is lost in scheduling noise on a
     * console, and it is the average that compares across builds. */
    if (!bad) {
        const double t0 = now_ms();
        for (int i = 0; i < 10; i++) BN_mod_exp(r, b, x, m, ctx);
        const double dt = (now_ms() - t0) / 10.0;
        clog("[CRYPTO] modexp 1024 bits: %.1f ms per operation", dt);
    }

    /* P-256, because it is what the handshake actually does: the negotiated
     * suite is ECDHE-ECDSA, not RSA. */
    EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    if (!g) {
        clog("[CRYPTO] P-256 not available in this build"); bad++;
    } else {
        EC_POINT *p1 = EC_POINT_new(g);
        BIGNUM *k = BN_new();
        BN_set_word(k, 0x9E3779B9u);
        const double t0 = now_ms();
        int ok = 1;
        for (int i = 0; i < 10; i++)
            if (!EC_POINT_mul(g, p1, k, NULL, NULL, ctx)) { ok = 0; break; }
        const double dt = (now_ms() - t0) / 10.0;
        if (!ok) { clog("[CRYPTO] EC_POINT_mul: FAILED"); bad++; }
        else if (EC_POINT_is_on_curve(g, p1, ctx) != 1) {
            clog("[CRYPTO] EC_POINT_mul: the point is NOT on the curve - wrong"); bad++;
        } else {
            clog("[CRYPTO] P-256 point mul: %.1f ms per operation (on the curve)", dt);
        }
        EC_POINT_free(p1); BN_free(k); EC_GROUP_free(g);
    }

    /* The same computation at the size of the chain's root. */
    {
        BIGNUM *m4 = NULL, *w4 = NULL, *r4 = BN_new();
        BN_hex2bn(&m4, MOD4096_HEX);
        BN_hex2bn(&w4, EXP4096_HEX);
        if (m4 && w4 && r4) {
            const double t0 = now_ms();
            const int ok = BN_mod_exp(r4, b, x, m4, ctx);
            const double dt = now_ms() - t0;
            if (!ok)                        { clog("[CRYPTO] modexp 4096: FAILED"); bad++; }
            else if (BN_cmp(r4, w4) != 0)   { clog("[CRYPTO] modexp 4096: WRONG"); bad++; }
            else clog("[CRYPTO] modexp 4096 bits (ISRG Root X1's size): %.0f ms", dt);
        }
        BN_free(m4); BN_free(w4); BN_free(r4);
    }

    /* === THE GENERATOR, AND WHY IT IS HERE ============================
     *
     * This bench first served to show that asymmetric crypto does NOT explain
     * what the handshake costs: 5.8 ms for the modexp, 12.9 ms for the point
     * multiplication, so ~50 ms for everything an ECDHE-ECDSA computes, plus
     * two round trips at ~30 ms. A handshake should cost ~110 ms; it costs
     * 1500. There are 1400 ms missing that are not computation.
     *
     * The generator is the next suspect, and the Vita fork of OpenSSL carries
     * a commit named "Fix PRNG generation": if it reseeds on every connection
     * from a slow source, that shows up in no compute profile at all and lands
     * entirely inside APPCONNECT. So two things are measured separately -- the
     * FIRST call, which pays for the seeding, and the ones after it. */
    {
        unsigned char buf[32];
        double t0 = now_ms();
        const int ok1 = RAND_bytes(buf, (int)sizeof buf);
        const double t_first = now_ms() - t0;
        t0 = now_ms();
        int okn = 1;
        for (int i = 0; i < 20; i++) if (!RAND_bytes(buf, (int)sizeof buf)) { okn = 0; break; }
        const double t_next = (now_ms() - t0) / 20.0;
        if (!ok1 || !okn) { clog("[CRYPTO] RAND_bytes: FAILED"); bad++; }
        else clog("[CRYPTO] RAND_bytes 32 B: %.1f ms on the first call, %.2f ms after",
                  t_first, t_next);
    }

    BN_free(m); BN_free(b); BN_free(x); BN_free(r); BN_free(want);
    BN_CTX_free(ctx);
    clog("[CRYPTO] bench done: %d failure(s)", bad);
    return bad;
}

#else   /* SHADOW_BENCH_LIB == 2: Mbed TLS */

int shadow_crypto_selftest(void)
{
    const char *e = getenv("SHADOW_CRYPTO_SELFTEST");
    if (!e || atoi(e) == 0) return 0;

    clog("[CRYPTO] bench: does the library compute correctly, and how fast? "
         "(Mbed TLS %s)", MBEDTLS_VERSION_STRING);

    int bad = 0, rc, modexp_ok = 0;

    /* THE GENERATOR FIRST, because the point multiplication below needs one
     * (Mbed TLS blinds it), and because its seeding is exactly the cost the
     * OpenSSL arm's first RAND_bytes measured. A seed that fails here is the
     * wolfSSL /dev/urandom defect again: every TLS connection would fail. */
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    double t0 = now_ms();
    rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                               (const unsigned char *)"halyard-bench", 13);
    const double t_seed = now_ms() - t0;
    const int have_rng = (rc == 0);
    if (!have_rng) {
        clog("[CRYPTO] ctr_drbg_seed FAILED (-0x%04x): no entropy - every TLS "
             "connection would fail", (unsigned)-rc);
        bad++;
    } else {
        unsigned char buf[32];
        int okn = 1;
        t0 = now_ms();
        for (int i = 0; i < 20; i++)
            if (mbedtls_ctr_drbg_random(&drbg, buf, sizeof buf) != 0) { okn = 0; break; }
        const double t_next = (now_ms() - t0) / 20.0;
        if (!okn) { clog("[CRYPTO] ctr_drbg_random: FAILED"); bad++; }
        else clog("[CRYPTO] random 32 B: %.1f ms to seed, %.2f ms per call after",
                  t_seed, t_next);
    }

    mbedtls_mpi m, b, x, r, want;
    mbedtls_mpi_init(&m); mbedtls_mpi_init(&b); mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&want);
    if (mbedtls_mpi_read_string(&m, 16, MOD_HEX) != 0 ||
        mbedtls_mpi_read_string(&want, 16, EXPECTED_HEX) != 0 ||
        mbedtls_mpi_lset(&b, 2) != 0 || mbedtls_mpi_lset(&x, 65537) != 0) {
        clog("[CRYPTO] allocation FAIL");
        bad++;
        goto out;
    }

    /* CORRECTNESS first - same vector, same rule as the OpenSSL arm. */
    if ((rc = mbedtls_mpi_exp_mod(&r, &b, &x, &m, NULL)) != 0) {
        clog("[CRYPTO] modexp: the call FAILED (-0x%04x)", (unsigned)-rc); bad++;
    } else if (mbedtls_mpi_cmp_mpi(&r, &want) != 0) {
        char got[300]; size_t olen = 0;
        if (mbedtls_mpi_write_string(&r, 16, got, sizeof got, &olen) != 0) got[0] = 0;
        clog("[CRYPTO] modexp: WRONG RESULT - the library lies");
        clog("[CRYPTO]   got      %.32s...", got[0] ? got : "?");
        clog("[CRYPTO]   expected %.32s...", EXPECTED_HEX);
        bad++;
    } else {
        clog("[CRYPTO] modexp 1024 bits: CORRECT");
        modexp_ok = 1;
    }

    /* HOW FAST. No cached R^2 (the last argument is NULL), so each round pays
     * what BN_mod_exp pays: the figures compare across the two libraries. */
    if (modexp_ok) {
        t0 = now_ms();
        for (int i = 0; i < 10; i++) mbedtls_mpi_exp_mod(&r, &b, &x, &m, NULL);
        const double dt = (now_ms() - t0) / 10.0;
        clog("[CRYPTO] modexp 1024 bits: %.1f ms per operation", dt);
    }

    /* P-256: the negotiated suite is ECDHE-ECDSA. */
    if (have_rng) {
        mbedtls_ecp_group g;
        mbedtls_ecp_point p1;
        mbedtls_mpi k;
        mbedtls_ecp_group_init(&g); mbedtls_ecp_point_init(&p1); mbedtls_mpi_init(&k);
        /* 0x9E3779B9 does not fit mbedtls_mpi_sint (32 bits here). */
        if (mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
            mbedtls_mpi_read_string(&k, 16, "9E3779B9") != 0) {
            clog("[CRYPTO] P-256 not available in this build"); bad++;
        } else {
            int ok = 1;
            t0 = now_ms();
            for (int i = 0; i < 10; i++)
                if (mbedtls_ecp_mul(&g, &p1, &k, &g.G,
                                    mbedtls_ctr_drbg_random, &drbg) != 0) { ok = 0; break; }
            const double dt = (now_ms() - t0) / 10.0;
            if (!ok) { clog("[CRYPTO] ecp_mul: FAILED"); bad++; }
            else if (mbedtls_ecp_check_pubkey(&g, &p1) != 0) {
                clog("[CRYPTO] ecp_mul: the point is NOT on the curve - wrong"); bad++;
            } else {
                clog("[CRYPTO] P-256 point mul: %.1f ms per operation (on the curve)", dt);
            }
        }
        mbedtls_ecp_group_free(&g); mbedtls_ecp_point_free(&p1); mbedtls_mpi_free(&k);
    }

    /* The same computation at the size of the chain's root. */
    {
        mbedtls_mpi m4, w4, r4;
        mbedtls_mpi_init(&m4); mbedtls_mpi_init(&w4); mbedtls_mpi_init(&r4);
        if (mbedtls_mpi_read_string(&m4, 16, MOD4096_HEX) == 0 &&
            mbedtls_mpi_read_string(&w4, 16, EXP4096_HEX) == 0) {
            t0 = now_ms();
            rc = mbedtls_mpi_exp_mod(&r4, &b, &x, &m4, NULL);
            const double dt = now_ms() - t0;
            if (rc != 0)                              { clog("[CRYPTO] modexp 4096: FAILED"); bad++; }
            else if (mbedtls_mpi_cmp_mpi(&r4, &w4))   { clog("[CRYPTO] modexp 4096: WRONG"); bad++; }
            else clog("[CRYPTO] modexp 4096 bits (ISRG Root X1's size): %.0f ms", dt);
        }
        mbedtls_mpi_free(&m4); mbedtls_mpi_free(&w4); mbedtls_mpi_free(&r4);
    }

out:
    mbedtls_mpi_free(&m); mbedtls_mpi_free(&b); mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&want);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    clog("[CRYPTO] bench done: %d failure(s)", bad);
    return bad;
}

#endif  /* which library */

#else   /* SHADOW_BENCH_LIB == 0: no library to bench on this target */

int shadow_crypto_selftest(void)
{
    /* Silent: a bench that is absent is not a failure, and one line per
     * startup on the console that cannot run it would be noise. Returning 0
     * means "nothing to report", exactly as when the toggle is not set. */
    return 0;
}

#endif  /* SHADOW_BENCH_LIB */
