/* log_redact.h - the journal's LAST line of defence, by SHAPE and not by label.
 *
 * === WHY THIS EXISTS SEPARATELY FROM log_mask.h ========================
 *
 * `log_mask.h` is the FIRST line: a call site that knows it holds a key asks
 * for it to be shown as `1b0c...d82a (32 o)`. That is the right place, because
 * only the call site knows what the value means.
 *
 * This is what catches the call sites that did not. And they exist: the audit
 * of 2026-09-13 found five in `smoke_test.c` alone - the whole 20-byte auth
 * hash, eight contiguous key bytes twice, a token prefix, and an 80-byte dump
 * of the very reply that carries the session key. All of them at INFO, so in
 * the log file AND in the network mirror.
 *
 * === WHY MARKERS WERE NOT ENOUGH, WHICH IS THE POINT ===================
 *
 * The previous version keyed off four literal markers: `token=`, `Bearer `,
 * `chacha20_key=`, `auth_hash=`. Every one of the five leaks above escaped it,
 * because `smoke_test.c` writes `tok=` and `Auth hash 20B extracted:` and its
 * own spaced hex. A marker list is a list of the mistakes you already know
 * about; a secret does not announce itself with a label.
 *
 * So the rules here are about SHAPE. A JWT looks like a JWT wherever it is
 * printed. A PEM body looks like a PEM body. A 64-character hex run is a key no
 * matter what precedes it.
 *
 * === WHAT IS DELIBERATELY *NOT* MASKED, AND WHY IT MATTERS =============
 *
 * SPACED hex is left alone: `41 01 00 14 00`, `b0=0x02 flag=0x01`, chunk
 * headers, NAL prefixes. That is the protocol-dump idiom of this project -
 * nineteen `"%02x "` call sites - and blanket-masking it would blind exactly
 * the video and wire debugging the whole repository is built on. A defence that
 * destroys the diagnosis is not a defence, it is a different failure.
 *
 * CONTIGUOUS hex is the opposite: only four call sites emit it, and no protocol
 * constant here is 32 hex characters long in one run. A run that long is a key,
 * a hash, or a device id.
 *
 * The consequence, stated plainly rather than hidden: a secret printed as
 * SPACED hex still gets through. `smoke_test.c`'s auth hash was exactly that,
 * and only the source-side fix caught it. This file narrows the gap; it does
 * not close it, and nothing here replaces masking at the call site.
 *
 * === THE INVARIANT ======================================================
 *
 * Every replacement happens IN PLACE and NEVER GROWS the line. The journal
 * hands over a fixed buffer; a redactor that needed more room than the secret
 * occupied would be a buffer overflow in the one function whose job is safety.
 * When a replacement would not fit, the text is left untouched - which is why
 * every threshold below is comfortably longer than its label.
 */
#pragma once

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

/* A contiguous hex run of at least this many characters is a secret. 32 = 16
 * bytes; the longest contiguous protocol constant logged here is a 32-character
 * UUID, which is why UUIDs are excluded by the dash test below. */
#define LOG_REDACT_HEX_MIN 32

/* --------------------------------------------------------------- helpers */

static inline bool lr_b64ch(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '+' || c == '/';
}

/* Overwrite [beg,end) with `label`, then close the gap. Never grows. */
static inline char *lr_replace(char *beg, char *end, const char *label)
{
    const size_t ll = strlen(label), ls = (size_t)(end - beg);
    if (ll > ls) return end;                 /* would grow: leave it alone */
    memcpy(beg, label, ll);
    memmove(beg + ll, end, strlen(end) + 1);
    return beg + ll;
}

/* ------------------------------------------------------------------ JWT */

/* `eyJ` + base64url + '.' + base64url + '.' - the shape of a JSON Web Token,
 * wherever it appears and whatever label precedes it. `eyJ` is `{"` in base64,
 * so a token is recognisable without knowing the field it sits in. */
static inline void lr_jwt(char *buf)
{
    char *p = buf;
    while ((p = strstr(p, "eyJ")) != NULL) {
        char *q = p;
        int dots = 0;
        size_t seg = 0;
        while (*q && (lr_b64ch(*q) || *q == '.' || *q == '=')) {
            if (*q == '.') { if (seg < 8) break; dots++; seg = 0; }
            else seg++;
            q++;
        }
        if (dots >= 2 && (size_t)(q - p) >= 24) p = lr_replace(p, q, "[JWT-REDACTED]");
        else p += 3;
    }
}

/* ------------------------------------------------------------------ PEM */

/* Everything between a PEM BEGIN line and its END, private keys only. A public
 * key or a certificate in a log is not a secret and stays readable. */
static inline void lr_pem(char *buf)
{
    char *p = buf;
    while ((p = strstr(p, "BEGIN ")) != NULL) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char *key = strstr(p, "PRIVATE KEY");
        if (!key || key > eol) { p += 6; continue; }
        char *end = strstr(key, "END ");
        char *stop = end ? end : p + strlen(p);
        if (end) { while (*stop && *stop != '\n') stop++; }
        p = lr_replace(p, stop, "[PRIVATE-KEY-REDACTED]");
    }
}

/* ----------------------------------------------------- long contiguous hex */

/* A run of >= LOG_REDACT_HEX_MIN hex characters, keeping four at each end so
 * two sessions can still be told apart. A UUID is excluded: its dashes break
 * the run into pieces well under the threshold, and 32 hex characters that ARE
 * a UUID arrive dashless only from our own `%02x` formatter, which no longer
 * prints one. */
static inline void lr_long_hex(char *buf)
{
    char *p = buf;
    while (*p) {
        if (!isxdigit((unsigned char)*p)) { p++; continue; }
        char *beg = p;
        while (isxdigit((unsigned char)*p)) p++;
        const size_t n = (size_t)(p - beg);
        if (n < LOG_REDACT_HEX_MIN) continue;
        /* `1b0c...d82a` - the same shape log_mask.h produces at the source, so
         * an already-masked value passes through unchanged. */
        memcpy(beg + 4, "...", 3);
        memmove(beg + 7, p - 4, strlen(p - 4) + 1);
        p = beg + 11;
    }
}

/* ------------------------------------------------------------- by marker */

/* The label-based rules stay, for what shape cannot see: a token that happens
 * to be short, a password made of words. The list is case-insensitive on the
 * marker and covers the variants that actually occur in this code base -
 * including `tok=`, which is what let a live token through for months. */
static inline char *lr_after(char *start, const char *marker, const char *ends,
                             const char *label)
{
    const size_t lm = strlen(marker);
    for (char *p = start; *p; p++) {
        if (strncasecmp(p, marker, lm) != 0) continue;
        char *beg = p + lm, *end = beg;
        while (*end && !strchr(ends, *end)) end++;
        if ((size_t)(end - beg) < 8) return end;   /* too short to be one */
        return lr_replace(beg, end, label);
    }
    return NULL;
}

/* --------------------------------------------------------------- the pass */

static inline void log_redact(char *buf)
{
    if (!buf) return;
    static const char END_URL[] = " \t\r\n\"'&#,)";
    static const char END_HDR[] = " \t\r\n\"'";

    /* Shape first: it catches what no marker list anticipated. */
    lr_pem(buf);
    lr_jwt(buf);

    /* Then the labels, for the values shape cannot recognise. */
    static const char *const MARKERS[] = {
        "token=", "tok=", "streamingtoken=", "streaming_token=",
        "password=", "passwd=", "secret=", "api_key=", "apikey=",
        "client_secret=", "authorization: ", NULL
    };
    for (int i = 0; MARKERS[i]; i++) {
        char *p = buf;
        while ((p = lr_after(p, MARKERS[i], END_URL, "[REDACTED]")) != NULL)
            ;
    }
    char *p = buf;
    while ((p = lr_after(p, "Bearer ", END_HDR, "[JWT-REDACTED]")) != NULL)
        ;

    /* Long contiguous hex last, so it also covers whatever the rules above
     * left behind. */
    lr_long_hex(buf);
}
