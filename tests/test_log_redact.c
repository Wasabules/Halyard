/* test_log_redact.c - the journal's last line of defence, and its counter-cases.
 *
 * === WHY THIS SUITE EXISTS =============================================
 *
 * `redact()` had NO test. The function whose entire job is to catch the secrets
 * the call sites missed was itself unverified - and the audit of 2026-09-13
 * found five leaks in `smoke_test.c` that it had been failing to catch for
 * months. A safety net nobody has ever dropped anything into is a decoration.
 *
 * The counter-cases here matter as much as the positive ones, and possibly
 * more: a redactor that eats protocol wire hex would blind the video debugging
 * this whole project depends on. Every `[KEEP]` check below is a line the code
 * really logs, which must come out untouched.
 */
#include "../core/services/log_redact.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
        checks++;                                                             \
        if (!(cond)) { failures++;                                            \
            printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); }      \
    } while (0)

/* Redact a copy, so the literal stays readable in the assertion. */
static const char *red(char *dst, size_t cap, const char *in)
{
    snprintf(dst, cap, "%s", in);
    log_redact(dst);
    return dst;
}
#define RED(in) red(buf, sizeof buf, (in))

static void jwt(void)
{
    char buf[512];
    printf("-- JWT: recognised by shape, whatever the label\n");

    /* A three-segment token. The header is `{"alg":...` in base64, hence eyJ. */
    const char *t = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9."
                    "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4ifQ."
                    "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
    char line[512];
    snprintf(line, sizeof line, "auth: got token %s for the session", t);
    RED(line);
    CHECK(strstr(buf, "eyJzdWIiOiIxMjM0") == NULL, "the JWT body must be gone");
    CHECK(strstr(buf, "[JWT-REDACTED]") != NULL, "and replaced by a marker");
    CHECK(strstr(buf, "for the session") != NULL, "the rest of the line survives");

    /* The label is irrelevant - that is the whole point of a shape rule. */
    snprintf(line, sizeof line, "smoke: REGISTER OK tok=%s expiry=99", t);
    RED(line);
    CHECK(strstr(buf, "SflKxwRJ") == NULL,
          "COUNTER-CASE: `tok=` is not in any marker list the old code had");

    snprintf(line, sizeof line, "GET /x\\r\\nAuthorization: Bearer %s\\r\\n", t);
    RED(line);
    CHECK(strstr(buf, "eyJhbGciOiJSUzI1NiI") == NULL, "a Bearer header is caught too");

    /* [KEEP] `eyJ` that is not a token must survive. */
    CHECK(strstr(RED("video: codec eyJ not a token here"), "eyJ") != NULL,
          "KEEP: three letters that merely start like one are left alone");
}

static void pem(void)
{
    char buf[512];
    printf("-- PEM: a private key body, never its label alone\n");

    RED("ssh: reply carries -----BEGIN OPENSSH PRIVATE KEY-----\n"
        "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAAB\n"
        "-----END OPENSSH PRIVATE KEY-----\n");
    CHECK(strstr(buf, "b3BlbnNzaC1rZXk") == NULL,
          "the key body must be gone - this is the SEC1 defect exactly");
    CHECK(strstr(buf, "[PRIVATE-KEY-REDACTED]") != NULL, "and named as removed");

    /* [KEEP] a CERTIFICATE is not a secret; masking it would hide a diagnosis. */
    CHECK(strstr(RED("tls: chain -----BEGIN CERTIFICATE----- MIIB..."),
                 "BEGIN CERTIFICATE") != NULL,
          "KEEP: a certificate is public and must stay readable");
}

static void long_hex(void)
{
    char buf[512];
    printf("-- contiguous hex: a key is a key whatever precedes it\n");

    /* 64 hex characters = a 32-byte ChaCha20 key. */
    RED("crypto: session key dcee1122334455667788990011223344"
        "55667788990011223344556677889900dac1 installed");
    CHECK(strstr(buf, "5566778899001122334455") == NULL, "the middle must be gone");
    CHECK(strstr(buf, "...") != NULL, "first and last kept, as log_mask.h does");
    CHECK(strstr(buf, "installed") != NULL, "the sentence survives");

    /* 40 hex = the device-id, and the auth hash written without spaces. */
    RED("clients: device-id 804f1122334455667788990011223344556605271 sent");
    CHECK(strstr(buf, "8899001122334455") == NULL, "a 40-hex identifier too");

    /* Already masked at the source: must pass through untouched. */
    const char *m = "auth_hash=1b0c...d82a (20 o)";
    CHECK(strcmp(RED(m), m) == 0,
          "COUNTER-CASE: a value already masked is not masked twice");
}

static void keeps(void)
{
    char buf[512];
    printf("-- KEEP: the protocol wire this project debugs with\n");

    /* Every one of these is a line the code really writes. If the redactor
     * eats them, the video and wire diagnosis goes dark - which would be a
     * worse failure than the one being defended against. */
    const char *wire[] = {
        "M32: register packet 41 01 00 14 00 <auth hash, 20 B>",
        "  cap 0000: 08 05 18 01 20 01 28 e7 36 12 8b 03 00 00 00 01",
        "vst: FRAME byte0=0x02 flag=0x01 frame_id=8213 nal_len=1241",
        "[G43] flush prev incomplete: recv=11 max=11 contig=10",
        "video: SPS 67 64 00 34 ac 2b 40 3c 01 13 f2 e0 22",
        "sufp: hdr 0a 00 0b 00 15 20 00 00 chunk=10/11",
        "[L5] video/decode n=1841 avg=3.5 p50=3.4 p90=4.1 p99=8.8 worst=21.0",
        "net: 192.168.1.17:5000 rtt=12ms loss=0.56%",
    };
    for (size_t i = 0; i < sizeof wire / sizeof *wire; i++) {
        char what[160];
        snprintf(what, sizeof what, "KEEP untouched: %.90s", wire[i]);
        CHECK(strcmp(RED(wire[i]), wire[i]) == 0, what);
    }

    /* A UUID is 32 hex characters, but its dashes break every run to 12 or
     * fewer - it must survive, because a client id is how you follow a session
     * across the log. */
    const char *uuid = "clients: id 5ca1ab1e-0000-4000-8000-00000000c0de-main";
    CHECK(strcmp(RED(uuid), uuid) == 0, "KEEP: a dashed UUID is not a key");
}

static void invariants(void)
{
    char buf[512];
    printf("-- the invariant: never grow, never write past the end\n");

    /* The buffer is the journal's, fixed. A replacement longer than what it
     * replaces would be an overflow in the one function whose job is safety. */
    for (size_t n = 8; n <= 200; n += 7) {
        char in[512], guard[512];
        size_t o = (size_t)snprintf(in, sizeof in, "k token=");
        for (size_t i = 0; i < n && o < sizeof in - 8; i++) in[o++] = 'a';
        in[o] = 0;
        snprintf(guard, sizeof guard, "%s", in);
        const size_t before = strlen(guard);
        log_redact(guard);
        CHECK(strlen(guard) <= before, "a redacted line is never longer");
    }

    CHECK(strcmp(RED(""), "") == 0, "an empty line is left empty");
    log_redact(NULL);                       /* must not crash */
    CHECK(1, "a null line does not crash");

    /* Two secrets on one line: both go. */
    RED("a token=abcdefghijklmnop and tok=qrstuvwxyz012345 end");
    CHECK(strstr(buf, "abcdefghij") == NULL && strstr(buf, "qrstuvwxyz") == NULL,
          "several secrets on one line are all redacted");
    CHECK(strstr(buf, "end") != NULL, "and the line still ends where it did");
}

int main(void)
{
    printf("== the journal's last line of defence ==\n");
    jwt();
    pem();
    long_hex();
    keeps();
    invariants();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
