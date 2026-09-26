/* test_log_mask.c - how a secret may appear in the log (SEC2, 2026-09-11),
 * core/services/log_mask.h.
 *
 * The session's chacha20 key, the authentication hash and the SPICE secret
 * were written in full into a log that is mirrored over the network. The
 * COUNTER-CASE checks fail on any "mask" that shows too much: no run of the
 * secret's middle may reach the output, and its two ends together never exceed
 * a quarter of it - for every length from 1 to 64. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/services/log_mask.h"

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                 \
    if (!(cond)) { printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, what);   \
                   failures++; }                                              \
} while (0)

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", (unsigned)b[i]);
    out[2 * n] = 0;
}

/* The characters the output reveals: what precedes "..." plus what sits
 * between "..." and " (". */
static size_t revealed(const char *out)
{
    const char *d = strstr(out, "...");
    if (!d) return strlen(out);
    const char *p = strstr(d, " (");
    return (size_t)(d - out) + (p ? (size_t)(p - (d + 3)) : strlen(d + 3));
}

/* COUNTER-CASE helper: does any `w`-character window of `full`, taken outside
 * its first and last `keep` characters, appear in the output? */
static int leaks_middle(const char *full, size_t keep, size_t w, const char *out)
{
    const size_t n = strlen(full);
    if (n < 2 * keep + w) return 0;
    for (size_t i = keep; i + w <= n - keep; i++) {
        char win[16];
        memcpy(win, full + i, w);
        win[w] = 0;
        if (strstr(out, win)) return 1;
    }
    return 0;
}

int main(void)
{
    printf("== log_mask: how a secret may appear in the log (SEC2) ==\n");
    char out[64], exp_key[64], full[2 * 64 + 1];

    /* The session key: 32 bytes. */
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x1b + 37 * i);
    log_mask_bytes(key, 32, out, sizeof out);
    snprintf(exp_key, sizeof exp_key, "%02x%02x...%02x%02x (32 o)",
             key[0], key[1], key[30], key[31]);
    CHECK(strcmp(out, exp_key) == 0, "32-byte key: the first and last 2 bytes, then the length");
    CHECK(revealed(out) == 8, "32-byte key: 8 hex digits shown, no more");
    to_hex(key, 32, full);
    CHECK(!leaks_middle(full, 4, 6, out), "COUNTER-CASE: no run of the key's middle in the output");

    /* The authentication hash: 20 bytes. */
    uint8_t hash[20];
    for (int i = 0; i < 20; i++) hash[i] = (uint8_t)(0xa5 ^ (13 * i));
    log_mask_bytes(hash, 20, out, sizeof out);
    char exp[64];
    snprintf(exp, sizeof exp, "%02x%02x...%02x%02x (20 o)", hash[0], hash[1], hash[18], hash[19]);
    CHECK(strcmp(out, exp) == 0, "20-byte hash: the first and last 2 bytes");
    to_hex(hash, 20, full);
    CHECK(!leaks_middle(full, 4, 6, out), "COUNTER-CASE: no run of the hash's middle in the output");

    /* Shorter values show less, then nothing. */
    uint8_t v[12];
    for (int i = 0; i < 12; i++) v[i] = (uint8_t)(0x40 + 9 * i);
    log_mask_bytes(v, 12, out, sizeof out);
    snprintf(exp, sizeof exp, "%02x...%02x (12 o)", v[0], v[11]);
    CHECK(strcmp(out, exp) == 0, "12 bytes: one byte at each end");
    log_mask_bytes(v, 7, out, sizeof out);
    CHECK(strcmp(out, "... (7 o)") == 0, "under 8 bytes: nothing shown but the length");
    log_mask_bytes(v, 0, out, sizeof out);
    CHECK(strcmp(out, "(vide)") == 0, "an empty value says so");
    log_mask_bytes(NULL, 32, out, sizeof out);
    CHECK(strcmp(out, "(absent)") == 0, "a missing value says so");

    /* The quarter rule, and the middle, for every length. */
    {
        int ok_quarter = 1, ok_middle = 1;
        for (size_t n = 1; n <= 64; n++) {
            uint8_t buf[64];
            for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(0x31 + 11 * i);
            log_mask_bytes(buf, n, out, sizeof out);
            const size_t shown_bytes = revealed(out) / 2;
            if (shown_bytes * 4 > n) ok_quarter = 0;
            to_hex(buf, n, full);
            if (leaks_middle(full, 2 * log_mask_keep(n, 2), 6, out)) ok_middle = 0;
        }
        CHECK(ok_quarter, "COUNTER-CASE: lengths 1-64 never show more than a quarter");
        CHECK(ok_middle, "COUNTER-CASE: lengths 1-64 never show a run of the middle");
    }

    /* Text secrets: tokens, passwords. */
    {
        static const char alpha[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        char tok[413];
        for (int i = 0; i < 412; i++) tok[i] = alpha[(i * 7 + i / 64) % 64];
        tok[412] = 0;
        log_mask_text(tok, out, sizeof out);
        snprintf(exp, sizeof exp, "%.4s...%.4s (412 car.)", tok, tok + 408);
        CHECK(strcmp(out, exp) == 0, "412-char token: the first and last 4 characters");
        CHECK(!leaks_middle(tok, 4, 6, out), "COUNTER-CASE: no run of the token's middle in the output");
        log_mask_text("abcdefghij", out, sizeof out);
        CHECK(strcmp(out, "a...j (10 car.)") == 0, "10 characters: one at each end");
        log_mask_text("secret7", out, sizeof out);
        CHECK(strcmp(out, "... (7 car.)") == 0, "under 8 characters: nothing shown but the length");
        log_mask_text("", out, sizeof out);
        CHECK(strcmp(out, "(vide)") == 0, "an empty text says so");
        log_mask_text(NULL, out, sizeof out);
        CHECK(strcmp(out, "(absent)") == 0, "a missing text says so");
    }

    /* A short buffer: NUL-terminated at `cap`, nothing written past it. */
    {
        char small[10];
        memset(small, 'Z', sizeof small);
        /* volatile: the truncation is the point here, and a constant cap lets
         * gcc 16 warn about it (-Wformat-truncation) at compile time. */
        volatile size_t small_cap = 6;
        log_mask_bytes(key, 32, small, small_cap);
        CHECK(small[5] == 0 && small[6] == 'Z' && small[9] == 'Z',
              "a short buffer: terminated at cap, nothing written past it");
        CHECK(strncmp(small, exp_key, 5) == 0, "a short buffer keeps the start of the MASKED form");
        small[0] = 'Q';
        const char *r = log_mask_bytes(key, 32, small, 0);
        CHECK(r[0] == 0 && small[0] == 'Q', "cap 0: nothing written");
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
