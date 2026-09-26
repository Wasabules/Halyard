/* log_mask.h - how a secret may appear in the log (SEC2, 2026-09-11).
 *
 * The log does not stay on the device: logsink.txt mirrors every line to the
 * development machine over the network, and the file itself comes off the SD
 * card. Until this date the session's chacha20 key, the authentication hash
 * (the UDP register identifier) and the nonce were written IN FULL at INFO
 * level - F36 wanted them for offline crypto tests - and so were the SPICE
 * secret and, in the headless smoke test, the streaming token, which the
 * journal's `token=` redaction missed because of the spaces around its `=`.
 * Whoever held one such log could decrypt the captured traffic of that session.
 *
 * The rule: the START and the END only. Two logs of one session can still be
 * matched, two sessions told apart; the secret cannot be used. At most 2 bytes
 * (4 hexadecimal digits) or 4 characters at EACH end, and never more than a
 * quarter of the secret in total:
 *
 *     32-byte key       ->  "1b0c...d82a (32 o)"
 *     12-byte value     ->  "1b...2a (12 o)"
 *     fewer than 8      ->  "... (6 o)"            nothing shown
 *     412-char token    ->  "eyJh...Xk2Q (412 car.)"
 *
 * Pure: no allocation, no I/O. The output is always NUL-terminated and never
 * written past `cap`; the returned pointer is `out`, so a call can sit directly
 * in a log argument list. Pinned by tests/test_log_mask.c. */
#ifndef SHADOW_LOG_MASK_H
#define SHADOW_LOG_MASK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Units shown at EACH end of an `n`-unit secret: n / 8, capped. Both ends
 * together are therefore never more than a quarter of it. */
static inline size_t log_mask_keep(size_t n, size_t cap_each)
{
    const size_t k = n / 8;
    return k < cap_each ? k : cap_each;
}

/* A binary secret: "hhhh...hhhh (N o)". */
static inline const char *log_mask_bytes(const uint8_t *b, size_t n,
                                         char *out, size_t cap)
{
    if (!out || cap == 0) return "";
    if (!b)     { snprintf(out, cap, "(absent)"); return out; }
    if (n == 0) { snprintf(out, cap, "(vide)");   return out; }
    const size_t k = log_mask_keep(n, 2);
    char head[8] = "", tail[8] = "";
    for (size_t i = 0; i < k; i++) {
        snprintf(head + 2 * i, sizeof head - 2 * i, "%02x", (unsigned)b[i]);
        snprintf(tail + 2 * i, sizeof tail - 2 * i, "%02x", (unsigned)b[n - k + i]);
    }
    snprintf(out, cap, "%s...%s (%u o)", head, tail, (unsigned)n);
    return out;
}

/* A text secret (token, password): "abcd...wxyz (N car.)". */
static inline const char *log_mask_text(const char *s, char *out, size_t cap)
{
    if (!out || cap == 0) return "";
    if (!s) { snprintf(out, cap, "(absent)"); return out; }
    const size_t n = strlen(s);
    if (n == 0) { snprintf(out, cap, "(vide)"); return out; }
    const size_t k = log_mask_keep(n, 4);
    snprintf(out, cap, "%.*s...%.*s (%u car.)", (int)k, s, (int)k, s + n - k,
             (unsigned)n);
    return out;
}

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_LOG_MASK_H */
