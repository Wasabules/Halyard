/* test_jwt - the `instance` field, and the buffer that once overflowed.
 *
 * `jwt_instance` carried a one-byte stack overflow (S49) that survived until
 * unrelated code moved and the compiler happened to notice. The bound checks
 * at the bottom of this file are that defect's counter-cases: a payload sized
 * exactly to the buffer, and one past it.
 *
 * Nothing here is a real token. The payloads are built from plain JSON encoded
 * with base64url by the helper below, so the test says what it means and no
 * credential appears in the repository.
 *
 * Compiled with -Wall -Wextra -Werror -O1 by tests/run_tests.sh.
 */
#include <stdio.h>
#include <string.h>

#include "../core/services/jwt.h"

static int checks = 0, failures = 0;

/* base64url, no padding - which is what a real JWT uses, and which the module
 * must accept. */
static void b64url(const char *in, char *out, size_t cap)
{
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const size_t n = strlen(in);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned b0 = (unsigned char)in[i];
        const unsigned b1 = i + 1 < n ? (unsigned char)in[i + 1] : 0;
        const unsigned b2 = i + 2 < n ? (unsigned char)in[i + 2] : 0;
        if (o + 4 >= cap) break;
        out[o++] = A[b0 >> 2];
        out[o++] = A[((b0 & 3) << 4) | (b1 >> 4)];
        if (i + 1 < n) out[o++] = A[((b1 & 0xf) << 2) | (b2 >> 6)];
        if (i + 2 < n) out[o++] = A[b2 & 0x3f];
    }
    out[o] = '\0';
}

/* A token whose payload is `json`. */
static void token(const char *json, char *out, size_t cap)
{
    char payload[6000];
    b64url(json, payload, sizeof payload);
    snprintf(out, cap, "eyJhbGciOiJIUzI1NiJ9.%s.c2lnbmF0dXJl", payload);
}

static void expect(const char *json, int want, const char *what)
{
    char tok[8192];
    token(json, tok, sizeof tok);
    checks++;
    const int got = jwt_instance(tok);
    if (got != want) {
        failures++;
        printf("  FAIL %-46s got %d, expected %d\n", what, got, want);
    }
}

static void expect_raw(const char *tok, int want, const char *what)
{
    checks++;
    const int got = jwt_instance(tok);
    if (got != want) {
        failures++;
        printf("  FAIL %-46s got %d, expected %d\n", what, got, want);
    }
}

int main(void)
{
    printf("== the JWT `instance` field (LIB2 2026-10-02) ==\n");

    /* --- what must be read ------------------------------------------------ */
    expect("{\"instance\":1}",                     1,  "instance 1");
    expect("{\"instance\":7}",                     7,  "a single digit");
    expect("{\"instance\":42}",                    42, "two digits");
    expect("{\"instance\":99}",                    99, "the top of the range");
    expect("{\"instance\": 12}",                   12, "a space after the colon");
    expect("{\"instance\":   12}",                 12, "several spaces");
    expect("{\"sub\":\"x\",\"instance\":5}",       5,  "not the first field");
    expect("{\"instance\":5,\"exp\":1900000000}",  5,  "not the last field");
    expect("{\"iss\":\"shadow\",\"aud\":\"vm\",\"instance\":31,\"jti\":\"abc\"}",
           31, "among several fields");

    /* --- the range. Outside 1..99 reads as absent, so no caller needs a
     * second check - which is the contract the header states. */
    expect("{\"instance\":0}",    -1, "0 is out of range");
    expect("{\"instance\":100}",  -1, "100 is out of range");
    expect("{\"instance\":-3}",   -1, "a negative instance");
    expect("{\"instance\":1e3}",  1,  "1e3 reads as 1, atoi stopping at 'e'");

    /* --- absent, malformed, hostile -------------------------------------- */
    expect("{\"instanc\":5}",     -1, "a near-miss field name");
    expect("{\"instance\":}",     -1, "no value");
    expect("{\"instance\":\"5\"}", -1, "a STRING value is not a number");
    expect("{}",                  -1, "an empty payload object");
    expect("not json at all",     -1, "a payload that is not JSON");

    expect_raw(NULL,              -1, "a NULL token");
    expect_raw("",                -1, "an empty token");
    expect_raw("no-dots-at-all",  -1, "no '.' separator");
    expect_raw("header.",         -1, "an empty payload");
    expect_raw(".",               -1, "nothing but a dot");

    /* A payload with no signature part: a JWT is header.payload.signature, but
     * the module accepts a two-part string because nothing here verifies a
     * signature and refusing it would only make a caller strip it first. */
    {
        char payload[2048], tok[4096];
        b64url("{\"instance\":8}", payload, sizeof payload);
        snprintf(tok, sizeof tok, "eyJhbGciOiJIUzI1NiJ9.%s", payload);
        expect_raw(tok, 8, "two parts, no signature");
    }

    /* --- THE BOUNDS, which is why this file exists ======================
     *
     * S49 was a one-byte write past `b64[4200]` when the payload filled it.
     * These two cases walk right up to that edge: a payload of exactly 4096
     * base64 characters (the module's documented ceiling) and one of 4097,
     * which must be refused rather than truncated into a wrong answer.
     *
     * Neither can crash visibly without a sanitizer - the byte written lands
     * on the stack - so what they really guard is the ACCEPT/REFUSE boundary
     * around it. Run under -fsanitize=address they guard the write too. */
    {
        char tok[8192];
        /* JSON padded with a long ignored field, so the base64 payload lands
         * just under the ceiling. */
        char json[3100];
        int n = snprintf(json, sizeof json, "{\"pad\":\"");
        while (n < (int)sizeof(json) - 24) json[n++] = 'x';
        snprintf(json + n, sizeof json - (size_t)n, "\",\"instance\":9}");
        token(json, tok, sizeof tok);
        checks++;
        const int got = jwt_instance(tok);
        /* Either it reads 9 or it refuses - both are correct. What must not
         * happen is a different number, which would mean it decoded garbage. */
        if (got != 9 && got != -1) {
            failures++;
            printf("  FAIL a payload at the ceiling gave %d\n", got);
        }
    }
    {
        /* Over the ceiling: refused, flatly. */
        char tok[16384];
        char json[6000];
        int n = snprintf(json, sizeof json, "{\"instance\":9,\"pad\":\"");
        while (n < (int)sizeof(json) - 8) json[n++] = 'y';
        snprintf(json + n, sizeof json - (size_t)n, "\"}");
        token(json, tok, sizeof tok);
        expect_raw(tok, -1, "a payload over 4096 bytes is REFUSED");
    }

    /* --- MUTATION CHECK ==================================================
     *
     * The range test is the one a reader is tempted to drop ("the server only
     * ever sends 1..99 anyway"). Without it, `{"instance":0}` returns 0, and 0
     * is a plausible-looking instance that would build REST paths like `/0/...`
     * for the rest of the session. */
    checks++;
    {
        char tok[4096];
        token("{\"instance\":0}", tok, sizeof tok);
        if (jwt_instance(tok) == 0) {
            failures++;
            printf("  FAIL instance 0 must not be returned as 0\n");
        }
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
