/* bench_chacha.c - how fast can this CPU open the video stream's AEAD chunks?
 *
 * WHY THIS EXISTS. Every video chunk on `:base+10` is a separate
 * chacha20-poly1305 box: `[ct][nonce 12][tag 16]`, opened one at a time by
 * `core/protocol/encryption.c`. On x86 and on the Switch's A57 that cost has
 * never been worth measuring. On a target where it might not be free -- a
 * PS Vita is a Cortex-A9 at 444 MHz -- it is the first question to settle, and
 * settling it costs an hour against a month of writing a backend that then
 * cannot keep up.
 *
 * So this measures the REAL code path on whatever CPU it is compiled for, with
 * the REAL chunk size, and prints the answer next to what the stream demands.
 *
 *   cc -O2 -o bench_chacha tools/client/bench_chacha.c core/protocol/encryption.c \
 *      -Ithird_party/wolfssl -Ithird_party/wolfssl/build_linux \
 *      third_party/wolfssl/build_linux/libwolfssl.a -lm -lpthread
 *   ./bench_chacha [seconds]
 *
 * READ THE RESULT AS A CEILING, not as a verdict. It measures the cipher alone,
 * on an idle machine, with the data already in cache. A real session also
 * reassembles, decodes and draws.
 *
 * MEASURED 2026-09-12, Linux desktop (x86-64), wolfSSL from third_party:
 *
 *     327 960 chunks/s = 407 MB/s = 3 256 Mb/s of video, 3.05 us per chunk
 *     720p needs 806 chunks/s   -> 0.2 % of this CPU
 *     1080p at the 25 Mb/s ceiling, 2 518 chunks/s -> 0.8 %
 *
 * WHAT THAT SAYS ABOUT THE VITA, AND WHAT IT DOES NOT. It is a desktop number;
 * a Cortex-A9 at 444 MHz is not this CPU and nobody has run this there. But the
 * margin is the point: even a HUNDREDFOLD penalty -- far worse than scalar
 * crypto on ARMv7 has any business being -- leaves 720p at about 20 % of one
 * core. So the cipher is very unlikely to be what stops a Vita port, and the
 * question worth asking first moves elsewhere: SceVideodec's throughput, the
 * 512 MB of RAM, and the network stack.
 *
 * That is an EXTRAPOLATION. Run this on the hardware before believing it -- it
 * is why it exists and why it takes no dependency beyond the cipher.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../../core/protocol/encryption.h"

/* `encryption.c` logs its failures. Nothing here should fail, and a benchmark
 * that pulled in the whole journal would be measuring the journal too. */
void journal_write(int sev, int cat, const char *fmt, ...);
void journal_write(int sev, int cat, const char *fmt, ...)
{ (void)sev; (void)cat; (void)fmt; }

/* Measured on the wire, 2026-05: a data chunk carries 1241 B, and the AEAD adds
 * 12 of nonce and 16 of tag. `KB.md §3.15`. */
#define CHUNK_PLAIN 1241
#define CHUNK_WIRE  (CHUNK_PLAIN + 12 + 16)

/* What the stream asks for, from the settings that are actually used. Above
 * ~25 Mb/s the picture breaks up on console for reasons that have nothing to do
 * with the cipher (DEBIT-1), so that is the honest ceiling to size against. */
static const struct { const char *name; double mbps; } LOADS[] = {
    { "720p, the Vita's screen is 960x544",  8.0 },
    { "1080p, ordinary",                    15.0 },
    { "1080p, the console ceiling (DEBIT-1)", 25.0 },
};

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const double budget = (argc > 1) ? atof(argv[1]) : 2.0;
    uint8_t key_tx[SHADOW_KEY_LEN], key_rx[SHADOW_KEY_LEN];
    for (int i = 0; i < SHADOW_KEY_LEN; i++) { key_tx[i] = (uint8_t)i; key_rx[i] = (uint8_t)(255 - i); }

    shadow_cipher *tx = shadow_cipher_create(key_tx, key_rx);
    shadow_cipher *rx = shadow_cipher_create(key_rx, key_tx);
    if (!tx || !rx) { fprintf(stderr, "cipher_create failed\n"); return 1; }

    static uint8_t plain[CHUNK_WIRE], wire[CHUNK_WIRE];
    for (int i = 0; i < CHUNK_PLAIN; i++) plain[i] = (uint8_t)(i * 7);

    /* One sealed chunk, re-opened over and over. The receive path uses the
     * _unsafe variant on UDP -- replay rejection would refuse the second
     * iteration, and it is the CIPHER we are timing, not the window. */
    memcpy(wire, plain, CHUNK_PLAIN);
    const int wire_len = shadow_cipher_encrypt(tx, wire, CHUNK_PLAIN);
    if (wire_len <= 0) { fprintf(stderr, "encrypt failed\n"); return 1; }

    long n = 0;
    const double t0 = now_s();
    double t1;
    do {
        for (int i = 0; i < 512; i++) {
            static uint8_t buf[CHUNK_WIRE];
            memcpy(buf, wire, (size_t)wire_len);
            if (!shadow_cipher_decrypt_unsafe(rx, buf, wire_len - 28, buf + wire_len - 28,
                                              buf + wire_len - 16))
                { fprintf(stderr, "decrypt failed at %ld\n", n); return 1; }
            n++;
        }
        t1 = now_s();
    } while (t1 - t0 < budget);

    const double secs   = t1 - t0;
    const double chunks = (double)n / secs;
    const double mbps   = chunks * CHUNK_PLAIN * 8.0 / 1e6;

    printf("chacha20-poly1305, opening real %d B chunks\n", CHUNK_PLAIN);
    printf("  %ld chunks in %.2f s\n", n, secs);
    printf("  %.0f chunks/s = %.1f MB/s = %.0f Mb/s of video\n",
           chunks, chunks * CHUNK_PLAIN / 1e6, mbps);
    printf("  %.2f us per chunk\n", 1e6 / chunks);
    printf("\nwhat a session would need of it:\n");
    for (size_t i = 0; i < sizeof LOADS / sizeof LOADS[0]; i++) {
        const double need = LOADS[i].mbps * 1e6 / 8.0 / CHUNK_PLAIN;   /* chunks/s */
        printf("  %-40s %6.0f chunks/s -> %5.1f%% of this CPU\n",
               LOADS[i].name, need, need / chunks * 100.0);
    }
    shadow_cipher_destroy(tx);
    shadow_cipher_destroy(rx);
    return 0;
}
