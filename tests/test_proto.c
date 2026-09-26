/* test_proto.c - offline verification of the mini-protobuf.
 *
 * proto.c encodes our control messages AND decodes the SERVER'S REPLIES (the
 * chacha20 key, the authentication hash). That last point makes it the only
 * place in the project where code of ours interprets bytes we did not produce:
 * every bound must hold against a malformed message, not only against an honest
 * one.
 *
 * The cases marked [DoS] reproduce REAL defects found while auditing this file
 * on 2026-08-25; they hung the control thread forever.
 */
#include <stdio.h>
#include <string.h>
#include "../core/protocol/proto.h"
#include "ci_bodies.h"

static int total = 0, failed = 0;
static void check(bool cond, const char *what)
{
    total++;
    if (!cond) { failed++; printf("  FAIL  %s\n", what); }
}

/* Encodes a varint without going through the module (to build malformed
 * input). */
static int mk_varint(uint8_t *b, int off, unsigned long long v)
{
    while (v >= 0x80) { b[off++] = (uint8_t)((v & 0x7f) | 0x80); v >>= 7; }
    b[off++] = (uint8_t)v; return off;
}

static void test_encodeur(void)
{
    uint8_t b[256];

    /* Varint bounds: 1 byte up to 127, 2 from 128 on. */
    check(pb_write_varint(b, sizeof b, 0, 0)   == 1, "varint 0 fits in 1 byte");
    check(pb_write_varint(b, sizeof b, 0, 127) == 1, "varint 127 fits in 1 byte");
    check(pb_write_varint(b, sizeof b, 0, 128) == 2, "varint 128 takes 2 bytes");
    check(pb_write_varint(b, sizeof b, 0, ~0ULL) == 10, "varint 2^64-1 takes 10 bytes");

    /* Not enough capacity: refuse, never write out of bounds. */
    uint8_t petit[3]; memset(petit, 0xCC, sizeof petit);
    check(pb_write_varint(petit, 2, 0, ~0ULL) == -1, "capacite depassee refusee");
    check(petit[2] == 0xCC, "no write past the announced capacity");

    /* Tag = numero<<3 | type. */
    int n = pb_write_tag(b, sizeof b, 0, 5, PB_WIRE_LENDELIM);
    check(n == 1 && b[0] == ((5 << 3) | 2), "tag champ 5 lendelim");

    /* A string, bytes, a sub-message. */
    n = pb_write_string(b, sizeof b, 0, 1, "abc");
    check(n == 5 && b[1] == 3 && !memcmp(b + 2, "abc", 3), "chaine encodee");
    check(pb_write_string(b, sizeof b, 0, 1, NULL) == 2, "a null string is treated as empty");
    const uint8_t empty[1] = {0};
    check(pb_write_bytes(b, sizeof b, 0, 1, empty, 0) == 2, "zero-length bytes");
    check(pb_write_bool(b, sizeof b, 0, 3, true) == 2, "booleen vrai");

    /* The buffer must not overflow when the payload does not fit. */
    uint8_t court[8]; memset(court, 0xCC, sizeof court);
    check(pb_write_string(court, 6, 0, 1, "abcdefgh") == -1, "chaine trop longue refusee");
    check(court[7] == 0xCC, "no overflow on an over-long string");
}

static void test_aller_retour(void)
{
    uint8_t b[256]; int off = 0;
    off = pb_write_uint(b, sizeof b, off, 1, 300);
    off = pb_write_string(b, sizeof b, off, 2, "shadow");
    const uint8_t brut[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    off = pb_write_bytes(b, sizeof b, off, 3, brut, 4);
    check(off > 0, "message compose encode");

    int p = 0; uint32_t fn, wt; uint64_t v;
    p = pb_read_tag(b, off, p, &fn, &wt);
    check(fn == 1 && wt == PB_WIRE_VARINT, "champ 1 varint relu");
    p = pb_read_varint(b, off, p, &v);
    check(v == 300, "value 300 read back");

    const uint8_t *d; size_t dl;
    p = pb_read_tag(b, off, p, &fn, &wt);
    check(fn == 2 && wt == PB_WIRE_LENDELIM, "champ 2 lendelim relu");
    p = pb_read_lendelim(b, off, p, &d, &dl);
    check(dl == 6 && !memcmp(d, "shadow", 6), "chaine relue a l'identique");

    p = pb_read_tag(b, off, p, &fn, &wt);
    p = pb_read_lendelim(b, off, p, &d, &dl);
    check(fn == 3 && dl == 4 && !memcmp(d, brut, 4), "bytes read back identically");
    check(p == off, "the whole message is consumed");

    /* A full walk with no callback: must accept a well-formed message. */
    check(pb_iter_fields(b, off, NULL, NULL), "a walk over a valid message");
}

static void test_malforme(void)
{
    uint8_t b[64]; int n;

    /* [DoS] Longueur 2^64-1 : `off + dlen > len` DEBORDE et laissait passer.
     * pb_read_lendelim rendait alors out_len = 2^64-1 a l'appelant. */
    n = 0; b[n++] = (1 << 3) | 2; n = mk_varint(b, n, 0xFFFFFFFFFFFFFFFFULL); b[n++] = 0x42;
    const uint8_t *d = NULL; size_t dl = 0;
    check(pb_read_lendelim(b, n, 1, &d, &dl) == -1, "[DoS] longueur 2^64-1 refusee");
    check(pb_skip_field(b, n, 1, PB_WIRE_LENDELIM) == -1, "[DoS] a skip over length 2^64-1 is refused");

    /* [DoS] Length 2^64-11: the skip returned 0 while the offset was 1. The
     * offset went BACKWARDS, so ctrl_msgs.c's loops span forever. */
    n = 0; b[n++] = (1 << 3) | 2; n = mk_varint(b, n, 0xFFFFFFFFFFFFFFF5ULL); b[n++] = 0x42;
    int after = pb_skip_field(b, n, 1, PB_WIRE_LENDELIM);
    check(after == -1, "[DoS] longueur 2^64-11 refusee");
    check(after < 0 || after > 1, "[DoS] the offset never goes backwards");
    check(!pb_iter_fields(b, n, NULL, NULL), "[DoS] the walk refuses instead of looping");

    /* A length that is simply too large for the message. */
    n = 0; b[n++] = (1 << 3) | 2; b[n++] = 50; b[n++] = 0x42;
    check(pb_read_lendelim(b, n, 1, &d, &dl) == -1, "a length larger than the message is refused");

    /* A truncated varint: every byte has the continuation bit. */
    n = 0; for (int i = 0; i < 5; i++) b[n++] = 0xFF;
    uint64_t v;
    check(pb_read_varint(b, n, 0, &v) == -1, "a truncated varint is refused");

    /* A varint longer than 10 bytes. */
    n = 0; for (int i = 0; i < 11; i++) b[n++] = 0xFF; b[n++] = 0x01;
    check(pb_read_varint(b, n, 0, &v) == -1, "a varint longer than 10 bytes is refused");

    /* A non-canonical 10th byte: it can only carry bit 63. */
    n = 0; for (int i = 0; i < 9; i++) b[n++] = 0xFF; b[n++] = 0x7F;
    check(pb_read_varint(b, n, 0, &v) == -1, "a non-canonical 10th byte is refused");
    n = 0; for (int i = 0; i < 9; i++) b[n++] = 0xFF; b[n++] = 0x01;
    check(pb_read_varint(b, n, 0, &v) > 0 && v == ~0ULL, "a 10th byte of 0x01 is accepted (= 2^64-1)");

    /* Types de fil a size fixe, tronques. */
    n = 0; b[n++] = 0x11; b[n++] = 0x00;              /* champ 2, fixed64, 1 octet suit */
    check(pb_skip_field(b, n, 1, PB_WIRE_FIXED64) == -1, "a truncated fixed64 is refused");
    check(pb_skip_field(b, n, 1, PB_WIRE_FIXED32) == -1, "a truncated fixed32 is refused");

    /* An unknown wire type (3 and 4 are the groups, abandoned). */
    check(pb_skip_field(b, sizeof b, 0, 3) == -1, "wire type 3 is refused");
    check(pb_skip_field(b, sizeof b, 0, 6) == -1, "wire type 6 is refused");

    /* Entrees degenerees. */
    check(pb_read_varint(NULL, 10, 0, &v) == -1, "a null buffer is refused");
    check(pb_read_varint(b, 10, -1, &v) == -1, "a negative offset is refused");
    check(pb_skip_field(b, 10, -1, PB_WIRE_VARINT) == -1, "a skip at a negative offset is refused");
    check(pb_read_lendelim(b, 10, -1, &d, &dl) == -1, "a lendelim at a negative offset is refused");
    check(pb_iter_fields(b, 0, NULL, NULL), "an empty message is accepted");
}

/* A faulty callback: returns an offset that does not advance. pb_iter_fields
 * must protect itself against it, otherwise a caller's bug freezes the thread
 * instead of surfacing the error. */
static bool cb_immobile(uint32_t fn, uint32_t wt, const uint8_t *buf, size_t len,
                        int off, int *next_off, void *user)
{
    (void)fn; (void)wt; (void)buf; (void)len; (void)user;
    *next_off = off > 0 ? off - 1 : 0;   /* recule volontairement */
    return true;
}

static void test_progression(void)
{
    uint8_t b[16]; int off = 0;
    off = pb_write_uint(b, sizeof b, off, 1, 7);
    check(!pb_iter_fields(b, off, cb_immobile, NULL),
          "[DoS] a callback that does not advance is refused, not looped on");
}


/* -- Non-regression against REAL Shadow protobuf --------------------------
 * Channel announcement bodies captured byte-exact from the official client (a
 * copy of ctrl_msgs.c's SHADOW_CHANNEL_INFO_BODIES; frozen capture data, which
 * no longer changes). The hardened decoder must keep accepting them: without
 * this guard rail, a future hardening could reject legitimate traffic and break
 * the bootstrap with no test flinching.
 * ------------------------------------------------------------------------ */
/* Descends recursively into the sub-messages: a body is only really valid when
 * its nested fields are too. */
static bool recursive_parse(const uint8_t *b, size_t len, int profondeur)
{
    if (profondeur > 8) return true;          /* a guard rail for the test itself */
    int off = 0;
    while ((size_t)off < len) {
        uint32_t fn = 0, wt = 0;
        off = pb_read_tag(b, len, off, &fn, &wt);
        if (off < 0 || fn == 0) return false;
        if (wt == PB_WIRE_LENDELIM) {
            const uint8_t *d = NULL; size_t dl = 0;
            int after = pb_read_lendelim(b, len, off, &d, &dl);
            if (after < 0) return false;
            if (dl > 0 && !recursive_parse(d, dl, profondeur + 1)) return false;
            off = after;
        } else {
            int after = pb_skip_field(b, len, off, wt);
            if (after <= off) return false;   /* doit avancer */
            off = after;
        }
    }
    return true;
}

static void test_captures_reelles(void)
{
    struct { const uint8_t *b; size_t n; const char *name; } corps[] = {
        {CI_5, sizeof CI_5, "channel 5 (video)"},   {CI_6,  sizeof CI_6,  "channel 6 (audio)"},
        {CI_7, sizeof CI_7, "channel 7 (input)"},  {CI_8,  sizeof CI_8,  "channel 8 (cursor)"},
        {CI_9, sizeof CI_9, "channel 9 (microphone)"},   {CI_10, sizeof CI_10, "channel 10 (gamepad)"},
        {CI_11,sizeof CI_11,"channel 11 (clipboard)"},
        {CI_12,sizeof CI_12,"channel 12 (file transfer)"},
    };
    for (size_t i = 0; i < sizeof corps / sizeof corps[0]; i++) {
        char what[96];
        snprintf(what, sizeof what, "capture reelle acceptee : %s", corps[i].name);
        check(pb_iter_fields(corps[i].b, corps[i].n, NULL, NULL), what);
        snprintf(what, sizeof what, "valid sub-messages: %s", corps[i].name);
        check(recursive_parse(corps[i].b, corps[i].n, 0), what);
    }
}

int main(void)
{
    printf("== mini-protobuf (encoding + server replies) ==\n");
    test_encodeur();
    test_aller_retour();
    test_malforme();
    test_progression();
    test_captures_reelles();
    printf("%d checks, %d failure(s)\n", total, failed);
    if (!failed) printf("OK\n");
    return failed ? 1 : 0;
}
