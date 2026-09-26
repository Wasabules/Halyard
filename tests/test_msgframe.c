/* test_msgframe.c - framing the input channel's messages by a u32 LE prefix.
 *
 * A framing that slips by ONE byte never recovers: all the rest of the
 * connection is read crooked. Hence these cases at the exact bounds.
 *
 * The bytes used here come from real captures: our Connect blob (96 B, prefix
 * 0x5c = 92) and the server echo (104 B, prefix 0x64 = 100).
 */
#include <stdio.h>
#include <string.h>
#include "../core/protocol/msgframe.h"

static int total = 0, failed = 0;
static void check(int cond, const char *what)
{
    total++;
    if (!cond) { failed++; printf("  FAIL  %s\n", what); }
}

#define CAP 16384

/* Writes a message of `payload` useful bytes at offset `off`. */
static size_t place(uint8_t *b, size_t off, uint32_t charge, uint8_t pattern)
{
    b[off+0] = (uint8_t)(charge      ); b[off+1] = (uint8_t)(charge >>  8);
    b[off+2] = (uint8_t)(charge >> 16); b[off+3] = (uint8_t)(charge >> 24);
    memset(b + off + 4, pattern, charge);
    return off + 4 + charge;
}

static void test_cas_reels(void)
{
    uint8_t b[CAP];

    /* A server echo: 104 bytes in total, a prefix announcing 100. */
    size_t n = place(b, 0, 100, 0xEE);
    check(n == 104, "a fabricated 104-byte server echo");
    check(msgframe_next(b, 104, CAP) == 104, "echo complet reconnu (prefixe compris)");

    /* Blob Connect : 96 octets, prefixe 92. */
    n = place(b, 0, 92, 0xC0);
    check(n == 96, "a fabricated 96-byte Connect blob");
    check(msgframe_next(b, 96, CAP) == 96, "Connect complet reconnu");
}

static void test_incomplet(void)
{
    uint8_t b[CAP];
    place(b, 0, 100, 0xEE);            /* message de 104 o */

    /* Any length strictly below the total must return 0 - and NEVER a truncated
     * total, which would desynchronise the rest of the stream. */
    for (size_t dispo = 0; dispo < 104; dispo++) {
        if (msgframe_next(b, dispo, CAP) != 0) {
            char q[80]; snprintf(q, sizeof q, "%zu bytes available: must wait", dispo);
            check(0, q);
            return;
        }
    }
    check(1, "every length from 0 to 103 waits, none concludes too early");
    check(msgframe_next(b, 104, CAP) == 104, "the 104th byte triggers delivery");
    check(msgframe_next(b, 105, CAP) == 104, "one extra byte does not change the returned length");
}

static void test_coalescence(void)
{
    uint8_t b[CAP];
    /* Three echoes delivered in a single TCP read: this is the case the old code
     * ("one read = one message") wrongly routed to the audio. */
    size_t off = 0;
    off = place(b, off, 100, 0xA1);
    off = place(b, off, 100, 0xB2);
    off = place(b, off, 100, 0xC3);
    check(off == 312, "three coalesced echoes = 312 bytes");

    size_t pos = 0; int lus = 0;
    for (;;) {
        int t = msgframe_next(b + pos, off - pos, CAP);
        if (t <= 0) break;
        check(t == 104, "each extracted message is indeed 104 bytes");
        pos += (size_t)t; lus++;
    }
    check(lus == 3, "the three messages are separated");
    check(pos == off, "not a single byte is left behind");

    /* Two whole messages followed by a fragment: we deliver the two and keep the
     * rest. */
    off = 0;
    off = place(b, off, 100, 0xA1);
    off = place(b, off, 100, 0xB2);
    size_t partial = off + 40;                 /* 40 bytes of the 3rd message */
    place(b, off, 100, 0xC3);

    pos = 0; lus = 0;
    for (;;) {
        int t = msgframe_next(b + pos, partial - pos, CAP);
        if (t <= 0) break;
        pos += (size_t)t; lus++;
    }
    check(lus == 2, "two messages delivered, the fragment stays pending");
    check(partial - pos == 40, "the 40 bytes of the partial message are kept");
}

static void test_aberrant(void)
{
    uint8_t b[CAP];

    /* A size that will never fit the buffer: reported at once, rather than
     * waiting forever for bytes that will not fit. */
    place(b, 0, 0, 0);
    b[0] = 0xFF; b[1] = 0xFF; b[2] = 0xFF; b[3] = 0xFF;   /* 2^32-1 */
    check(msgframe_next(b, 64, CAP) == -1, "a size of 2^32-1 is reported as absurd");

    /* Exactly the capacity minus the prefix: this is the largest acceptable
     * size, and it must NOT be refused. */
    b[0] = (uint8_t)((CAP-4)      ); b[1] = (uint8_t)((CAP-4) >>  8);
    b[2] = (uint8_t)((CAP-4) >> 16); b[3] = (uint8_t)((CAP-4) >> 24);
    check(msgframe_next(b, 64, CAP) == 0, "the maximum size is accepted: we wait");
    check(msgframe_next(b, CAP, CAP) == CAP, "and delivered when everything is there");

    /* One byte more than the capacity: refused. */
    b[0] = (uint8_t)((CAP-3)      ); b[1] = (uint8_t)((CAP-3) >>  8);
    b[2] = (uint8_t)((CAP-3) >> 16); b[3] = (uint8_t)((CAP-3) >> 24);
    check(msgframe_next(b, 64, CAP) == -1, "one byte beyond the capacity is refused");

    /* An empty message (size 0): the prefix alone is a complete 4-byte message.
     * It MUST advance the offset, otherwise the receive thread's loop would spin
     * forever - the same trap as in proto.c. */
    b[0] = b[1] = b[2] = b[3] = 0;
    check(msgframe_next(b, 4, CAP) == 4, "an empty message: advances by 4, does not stall");

    /* Entrees degenerees. */
    check(msgframe_next(NULL, 100, CAP) == -1, "a null buffer is refused");
    check(msgframe_next(b, 100, 2) == -1, "a capacity smaller than the prefix is refused");
    check(msgframe_next(b, 0, CAP) == 0, "an empty buffer: we wait");
}

int main(void)
{
    printf("== framing by size prefix (the input channel) ==\n");
    test_cas_reels();
    test_incomplet();
    test_coalescence();
    test_aberrant();
    printf("%d checks, %d failure(s)\n", total, failed);
    if (!failed) printf("OK\n");
    return failed ? 1 : 0;
}
