/* jwt - the one field this client reads out of a JWT: `instance`.
 *
 * IN `services/` AND NOT `protocol/`: it has no dependency of its own and both
 * sides want it - the REST layer (`launcher.c`, which carried its own `extern`
 * declaration of it) and the session (`ctrl_session.c`, `vid_reasm.c`). A pure
 * utility belongs in the bottom layer, and since `core/common` was dissolved
 * (see `log.h`) that is `services`. `protocol/` was the first attempt, and it
 * made `launcher.c` reach UPWARDS - exactly the kind of edge this round of
 * work removes.
 *
 * === LIB2 2026-10-02 — EXTRACTED FROM A 744-LINE SMOKE TEST ================
 *
 * `jwt_instance()` lived in `core/protocol/smoke_test.c`, and two modules that
 * are not smoke tests called it: `ctrl_session.c` and `vid_reasm.c`. That is
 * what blocked the smoke test from moving out of `protocol/` — it reaches into
 * `core/media/` for the decoder, so it belongs in the composition layer, but
 * `protocol/` needed this one function out of it.
 *
 * A base64url decoder buried in a smoke test is a smell on its own, and this
 * one has history: the `S49` note below records a ONE-BYTE STACK OVERFLOW that
 * lived here until the surrounding code happened to move and the compiler
 * finally pointed at it. Code with that history and no test is the first thing
 * to extract, not the last.
 *
 * Pure: no state, no I/O, no getenv, no journal. Header-only, like `ft_path.h`
 * and `clip_dir.h`, so `tests/test_jwt.c` tests it by including it.
 *
 * WHAT IT IS NOT. It does not verify a signature, decode a header, check an
 * expiry or parse JSON. It finds `"instance":N` inside the payload and returns
 * N. The instance number is what `/N/devices` and the other per-instance REST
 * paths need (KB §2), and the token is one the server just handed us over TLS —
 * so there is nothing here to authenticate, only a field to read.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One base64 character's value, or -1 when it is not one. Ranges rather than a
 * table: see the note inside `jwt_instance`. */
static inline int jwt_b64val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* The `instance` field of a JWT payload, or -1 when it cannot be read.
 *
 * JWT is `header.payload.signature`, the payload being base64url-encoded JSON.
 * Accepts a payload with or without a trailing `.signature`, with or without
 * base64 padding, and refuses a payload over 4096 bytes rather than growing a
 * buffer for one that size.
 *
 * Valid instances are 1..99; anything else reads as -1, so a malformed or
 * hostile token gives the same answer as an absent field and no caller needs a
 * second check. */
static inline int jwt_instance(const char *jwt)
{
    if (!jwt) return -1;

    const char *dot1 = strchr(jwt, '.');
    if (!dot1) return -1;
    const char *payload = dot1 + 1;
    const char *dot2 = strchr(payload, '.');
    const size_t plen = dot2 ? (size_t)(dot2 - payload) : strlen(payload);
    if (plen > 4096) return -1;

    /* base64url -> base64, then pad. */
    char b64[4200];
    size_t out = 0;
    for (size_t i = 0; i < plen && out < sizeof(b64) - 5; i++) {
        char c = payload[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        b64[out++] = c;
    }
    /* === S49 2026-08-26 - ONE BYTE PAST THE ARRAY =========================
     * The bound used to be `out < sizeof(b64)`, so `out` could reach
     * sizeof(b64) and the `b64[out] = 0` below then wrote one byte past the
     * array, onto the stack. The compiler only pointed it out once the
     * surrounding code moved - it never had before, which is why the room for
     * the terminator is now explicit in both bounds. */
    while (out % 4 != 0 && out < sizeof(b64) - 1) b64[out++] = '=';
    b64[out] = 0;

    /* === LIB2 2026-10-02 - NO DESIGNATED INITIALISERS IN A HEADER =========
     *
     * This was a 256-entry table written `['A']=0, ['B']=1, …`. That is a GNU
     * C extension, and it compiled for years because the code lived in a `.c`
     * file. As a header it is included from C++ too, and g++ answers
     * "sorry, unimplemented: non-trivial designated initializers not
     * supported" - so the table had to go for the header to be usable by the
     * very clients this extraction is for.
     *
     * Ranges instead, which is also clearer, and which lets an INVALID
     * character be an invalid character: the table mapped every non-base64
     * byte to 0, so a malformed payload decoded silently into garbage and was
     * only caught further down by the `"instance":` search failing. The
     * outcome was the same -1; the path is now explicit. */

    char json[4200];
    size_t jl = 0;
    for (size_t i = 0; i + 4 <= out && jl + 3 <= sizeof(json); i += 4) {
        if (b64[i] == '=') break;
        const int a = jwt_b64val((unsigned char)b64[i]);
        const int b = jwt_b64val((unsigned char)b64[i + 1]);
        const int c = b64[i + 2] == '=' ? -1 : jwt_b64val((unsigned char)b64[i + 2]);
        const int d = b64[i + 3] == '=' ? -1 : jwt_b64val((unsigned char)b64[i + 3]);
        /* A character that is not base64 ends the payload rather than decoding
         * as zero. Same answer as before (-1, because `"instance":` is then not
         * found) by an honest route. */
        if (a < 0 || b < 0) break;
        json[jl++] = (char)((a << 2) | (b >> 4));
        if (c >= 0) json[jl++] = (char)(((b & 0xf) << 4) | (c >> 2));
        if (d >= 0) json[jl++] = (char)(((c & 0x3) << 6) | d);
    }
    json[jl < sizeof(json) ? jl : sizeof(json) - 1] = 0;

    const char *inst = strstr(json, "\"instance\":");
    if (!inst) return -1;
    inst += 11;
    while (*inst == ' ') inst++;
    const int v = atoi(inst);
    return (v >= 1 && v <= 99) ? v : -1;
}

#ifdef __cplusplus
}
#endif
