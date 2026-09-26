/* test_sufp.c — SUFP reassembly of the CURSOR channel (`:base+30`).
 *
 * sufp.c is that channel's multi-chunk fallback: the packets whose direct
 * decryption fails land here. So it too interprets bytes that came from the
 * server — and it ALLOCATES according to what they declare.
 *
 * This file tests what the code DOES; the known differences with the video wire
 * are flagged in comments.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../core/protocol/sufp.h"

/* S81 — the journal now has a severity and a category; modules call
 * `journal_ecrire` through their alias. So the stub must provide both of the
 * module's functions, and `journal_actif` must return FALSE: that is what makes
 * the test write nothing at all, including when the module under test logs
 * inside a per-packet loop. */
void journal_uncategorised(const char *fmt, ...) { (void)fmt; }
void journal_write(int sev, int cat, const char *fmt, ...)
{ (void)sev; (void)cat; (void)fmt; }
int journal_enabled(int sev, int cat) { (void)sev; (void)cat; return 0; }

static int total = 0, failed = 0;
static void check(bool cond, const char *what)
{
    total++;
    if (!cond) { failed++; printf("  FAIL  %s\n", what); }
}

/* What the callback saw. */
static struct { int calls; uint8_t subchan; uint32_t frame_id;
                size_t len; uint8_t first, last; } seen;
static void on_frame(uint8_t subchan, uint32_t frame_id,
                     const uint8_t *buf, size_t len, void *user)
{
    (void)user;
    seen.calls++; seen.subchan = subchan; seen.frame_id = frame_id; seen.len = len;
    seen.first = len ? buf[0] : 0; seen.last = len ? buf[len-1] : 0;
}

/* Builds a type 3 chunk (11-byte header) with a payload filled with a
 * recognisable byte. */
static size_t chunk(uint8_t *b, uint8_t sub, uint16_t idx, uint16_t max,
                    uint32_t fid, uint8_t fill, size_t payload)
{
    b[0] = (3 << 4) | SUFP_PKT_TYPE_DATA3;
    b[1] = sub;
    b[2] = idx & 0xff;  b[3] = idx >> 8;
    b[4] = max & 0xff;  b[5] = max >> 8;
    b[6] = fid & 0xff;  b[7] = (fid >> 8) & 0xff;
    b[8] = (fid >> 16) & 0xff; b[9] = (fid >> 24) & 0xff;
    b[10] = 0;
    memset(b + SUFP_HDR_TYPE3, fill, payload);
    return SUFP_HDR_TYPE3 + payload;
}

static void test_header_bounds(void)
{
    sufp_reasm *r = sufp_create(on_frame, NULL);
    check(r != NULL, "reassembler created");
    uint8_t b[256];

    /* Empty packet or null buffer. */
    check(sufp_feed(r, NULL, 10) == -1, "null buffer refused");
    check(sufp_feed(r, b, 0) == -1, "zero length refused");
    check(sufp_feed(NULL, b, 10) == -1, "null reassembler refused");

    /* Ping: reported to the caller, not a chunk. */
    b[0] = (3 << 4) | SUFP_PKT_TYPE_PING;
    check(sufp_feed(r, b, 1) == 2, "ping packet reported as 2");

    /* Packet types outside 1..3. */
    b[0] = (3 << 4) | 0x00;
    check(sufp_feed(r, b, 32) == -1, "type 0 refused");
    b[0] = (3 << 4) | 0x07;
    check(sufp_feed(r, b, 32) == -1, "type 7 refused");

    /* Length exactly equal to the header: no payload, refused. This is the
     * bound that protects the buf[6..9] reads below. */
    size_t n = chunk(b, 0, 0, 0, 1, 0xAA, 0);
    check(n == SUFP_HDR_TYPE3, "payload-less chunk built");
    check(sufp_feed(r, b, SUFP_HDR_TYPE3) == -1, "length = header refused");
    check(sufp_feed(r, b, SUFP_HDR_TYPE3 - 1) == -1, "length < header refused");

    /* A single payload byte is enough. */
    n = chunk(b, 0, 0, 0, 42, 0xAA, 1);
    check(sufp_feed(r, b, n) == 1, "max=0 means ONE chunk: the picture completes at once");

    sufp_destroy(r);
}

static void test_chunk_bounds(void)
{
    sufp_reasm *r = sufp_create(on_frame, NULL);
    uint8_t b[256];

    /* The count is max+1: the largest acceptable `max` is capacity-1. */
    size_t n = chunk(b, 0, 0, SUFP_MAX_CHUNKS_PER_FRAME, 1, 0xAA, 8);
    check(sufp_feed(r, b, n) == -1, "max = capacity refused (count = capacity+1)");

    n = chunk(b, 0, 0, SUFP_MAX_CHUNKS_PER_FRAME - 1, 2, 0xAA, 8);
    check(sufp_feed(r, b, n) == 0, "max = capacity-1 accepted (count = capacity)");

    /* max = 0xFFFF: the count would overflow to 0. Refused, without looping. */
    n = chunk(b, 0, 0, 0xFFFF, 77, 0xAA, 8);
    check(sufp_feed(r, b, n) == -1, "max = 0xFFFF refused (the count would overflow)");

    /* S32 — `max` is the LAST INDEX, as on the video wire (G4/G21): the server
     * sends indices 0..max, i.e. max+1 chunks. So an index equal to max is
     * VALID. Rejecting it was G4's mistake: on video it cost every picture's
     * tail and 626 ffmpeg errors; here it cut multi-chunk cursor frames short of
     * their last part. */
    n = chunk(b, 0, 5, 5, 3, 0xAA, 8);
    check(sufp_feed(r, b, n) == 0, "idx == max ACCEPTED (max = last index)");
    n = chunk(b, 0, 6, 5, 3, 0xAA, 8);
    check(sufp_feed(r, b, n) == -1, "idx > max refused");

    sufp_destroy(r);
}

static void test_reassembly(void)
{
    sufp_reasm *r = sufp_create(on_frame, NULL);
    uint8_t b[256];
    memset(&seen, 0, sizeof seen);

    /* Three chunks, out of order: the picture must be recomposed in index
     * order, not in arrival order. */
    size_t n;
    n = chunk(b, 7, 1, 2, 0xCAFE, 0xB1, 4); check(sufp_feed(r, b, n) == 0, "chunk 1 waiting");
    n = chunk(b, 7, 2, 2, 0xCAFE, 0xC2, 4); check(sufp_feed(r, b, n) == 0, "chunk 2 waiting");
    check(seen.calls == 0, "no picture while a chunk is missing");
    n = chunk(b, 7, 0, 2, 0xCAFE, 0xA0, 4); check(sufp_feed(r, b, n) == 1, "chunk 0 completes the picture");

    check(seen.calls == 1, "the callback is called exactly once");
    check(seen.subchan == 7, "subchannel passed through");
    check(seen.frame_id == 0xCAFE, "frame_id passed through");
    check(seen.len == 12, "the three payloads are concatenated");
    check(seen.first == 0xA0, "the picture starts with the chunk at index 0");
    check(seen.last == 0xC2, "and ends with the highest index");

    /* Duplicate after completion: the picture was freed, the chunk starts a new one. */
    memset(&seen, 0, sizeof seen);
    n = chunk(b, 7, 0, 2, 0xCAFE, 0xA0, 4);
    check(sufp_feed(r, b, n) == 0, "chunk received after completion: a new picture waits");

    /* Duplicate before completion: ignored silently, with no double allocation. */
    sufp_reasm *r2 = sufp_create(on_frame, NULL);
    n = chunk(b, 0, 0, 1, 9, 0xD3, 4);
    check(sufp_feed(r2, b, n) == 0, "first copy accepted");
    check(sufp_feed(r2, b, n) == 0, "duplicate ignored without error");
    n = chunk(b, 0, 1, 1, 9, 0xE4, 4);
    check(sufp_feed(r2, b, n) == 1, "the picture completes despite the duplicate");
    sufp_destroy(r2);

    sufp_destroy(r);
}

static void test_window_and_inconsistency(void)
{
    sufp_reasm *r = sufp_create(on_frame, NULL);
    uint8_t b[256];

    /* The same frame_id announced with a different max: the old state is
     * dropped, otherwise we would mix two pictures. */
    size_t n = chunk(b, 0, 0, 3, 1000, 0x11, 4);
    check(sufp_feed(r, b, n) == 0, "picture opened with max=3 (= 4 chunks)");
    n = chunk(b, 0, 0, 7, 1000, 0x22, 4);
    check(sufp_feed(r, b, n) == 0, "inconsistent max: state reset, no mixing");

    /* Saturated window: more simultaneous pictures than slots. No leak and no
     * overflow must result (checked under ASan). */
    for (uint32_t i = 0; i < SUFP_WINDOW_SIZE + 64; i++) {
        n = chunk(b, 0, 0, 3, 5000 + i, 0x33, 16);
        sufp_feed(r, b, n);
    }
    check(true, "saturated window handled without crashing");

    sufp_destroy(r);   /* ASan checks that no chunk is left allocated */
}

int main(void)
{
    printf("== SUFP reassembly (cursor channel) ==\n");
    test_header_bounds();
    test_chunk_bounds();
    test_reassembly();
    test_window_and_inconsistency();
    printf("%d checks, %d failure(s)\n", total, failed);
    if (!failed) printf("OK\n");
    return failed ? 1 : 0;
}
