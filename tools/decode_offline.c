/* decode_offline — runs OUR decoder (h264_decoder.c, unchanged) over a
 * .h264 dump, outside any live session, and measures its output picture by picture.
 *
 * Why: ffmpeg decoded our dumps with no drift, while the pictures coming
 * out of our decoder mid-session degenerated (blockiness ×2.7,
 * contrast halved). The stream being the same, the difference is in
 * the way we drive libavcodec — and it only shows up by running
 * our own code over the same file, the same hardware and software, with nothing
 * d'autre autour.
 *
 * The output: a CSV (n, pts, w, h, luma_mean, luma_stddev, blockiness) and, optionally,
 * PGMs of the luminance every N pictures.
 *
 *   SHADOW_HWACCEL=0|1  decode_offline <flux.h264> <sortie.csv> [dossier_pgm] [N]
 *   DECODE_PACE_US      cadence d'alimentation (defaut 4000 = 4x le temps reel)
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "webrtc/h264_decoder.h"

/* The decoder has a single dependency: the log. We silence it, except
 * DECODE_VERBOSE=1. */
void webrtc_log(const char *fmt, ...)
{
    if (!getenv("DECODE_VERBOSE")) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
void webrtc_log_flush(void) {}
void webrtc_log_shutdown(void) {}

static uint32_t    g_frames = 0;
static FILE       *g_csv;
static const char *g_pgm_dir;
static int         g_pgm_every;

static void on_frame(int w, int h, const uint8_t *y, int ly,
                     const uint8_t *u, int lu, const uint8_t *v, int lv,
                     int fmt, int64_t pts, void *user)
{
    (void)u; (void)lu; (void)v; (void)lv; (void)fmt; (void)user;
    g_frames++;

    /* The luminance alone: the mean, the standard deviation, and the blockiness (the deviation at
     * the 16 px macroblock boundaries relative to the interior deviation — the same
     * definition as tools/analyze_artifacts.py, so they can be compared). */
    double sum = 0, sum2 = 0;
    double bv = 0, iv = 0, bh = 0, ih = 0;
    long   nbv = 0, niv = 0, nbh = 0, nih = 0;
    for (int r = 0; r < h; r++) {
        const uint8_t *p = y + (size_t)r * ly;
        const uint8_t *q = (r > 0) ? y + (size_t)(r - 1) * ly : NULL;
        for (int c = 0; c < w; c++) {
            sum += p[c]; sum2 += (double)p[c] * p[c];
            if (c > 0) {
                int d = abs(p[c] - p[c - 1]);
                if (c % 16 == 0) { bv += d; nbv++; } else { iv += d; niv++; }
            }
            if (q) {
                int d = abs(p[c] - q[c]);
                if (r % 16 == 0) { bh += d; nbh++; } else { ih += d; nih++; }
            }
        }
    }
    double n = (double)w * h, mean = sum / n;
    double var = sum2 / n - mean * mean, sd = var > 0 ? sqrt(var) : 0;
    double blk = ((bv / nbv) / fmax(iv / niv, 0.01)
                + (bh / nbh) / fmax(ih / nih, 0.01)) / 2.0;
    uint64_t hh = 1469598103934665603ULL;
    for (int r = 0; r < h; r++) {
        const uint8_t *row = y + (size_t)r * ly;
        for (int c = 0; c < w; c++) { hh ^= row[c]; hh *= 1099511628211ULL; }
    }
    fprintf(g_csv, "%u,%lld,%d,%d,%.2f,%.2f,%.3f,%016llx\n",
            g_frames, (long long)pts, w, h, mean, sd, blk, (unsigned long long)hh);

    if (g_pgm_dir && g_pgm_every > 0 && g_frames % g_pgm_every == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/f%05u.pgm", g_pgm_dir, g_frames);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P5\n%d %d\n255\n", w, h);
            for (int r = 0; r < h; r++) fwrite(y + (size_t)r * ly, 1, w, f);
            fclose(f);
        }
    }
}

/* A start code at `i`? Return its length (3 or 4), otherwise 0. */
static int start_code(const uint8_t *b, size_t n, size_t i)
{
    if (i + 3 < n && b[i] == 0 && b[i+1] == 0 && b[i+2] == 0 && b[i+3] == 1) return 4;
    if (i + 2 < n && b[i] == 0 && b[i+1] == 0 && b[i+2] == 1) return 3;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <flux.h264> <sortie.csv> [dossier_pgm] [N]\n", argv[0]);
        return 1;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, in) != (size_t)sz) { perror("read"); return 1; }
    fclose(in);

    g_csv = fopen(argv[2], "w");
    if (!g_csv) { perror(argv[2]); return 1; }
    fprintf(g_csv, "n,pts,w,h,luma_moy,luma_ecart,blocosite,luma_hash\n");
    g_pgm_dir   = argc > 3 ? argv[3] : NULL;
    g_pgm_every = argc > 4 ? atoi(argv[4]) : 100;
    const char *pace = getenv("DECODE_PACE_US");
    int pace_us = pace ? atoi(pace) : 4000;

    h264_decoder *d = h264_decoder_create(on_frame, NULL);
    if (!d) { fprintf(stderr, "decodeur : creation impossible\n"); return 1; }

    /* One access unit per delimiter (NAL 9): that is the split the
     * client makes in session, one emission per picture. */
    size_t   n = (size_t)sz, au_start = 0;
    uint32_t au_count = 0;
    int      have_au = 0;
    for (size_t i = 0; i + 4 < n; ) {
        int sc = start_code(buf, n, i);
        if (!sc) { i++; continue; }
        uint8_t type = buf[i + sc] & 0x1F;
        if (type == 9) {
            if (have_au && i > au_start) {
                h264_decoder_feed_annexb(d, buf + au_start, i - au_start,
                                         au_count * 1500u);
                au_count++;
                if (pace_us > 0) usleep((useconds_t)pace_us);
            }
            au_start = i; have_au = 1;
        }
        i += sc;
    }
    if (have_au && n > au_start) {
        h264_decoder_feed_annexb(d, buf + au_start, n - au_start, au_count * 1500u);
        au_count++;
    }
    usleep(500000);   /* let the transfer thread drain its queue */

    h264_decoder_stats_t st;
    h264_decoder_get_stats(d, &st);
    fprintf(stderr, "unites d'acces fournies : %u\n", au_count);
    fprintf(stderr, "images decodees : %u   erreurs : %u   livrees au rappel : %u   "
                    "abandonnees (file async pleine) : %u\n",
            st.frames_decoded, st.decode_errors, g_frames, st.async_dropped);
    h264_decoder_destroy(d);
    fclose(g_csv);
    free(buf);
    return 0;
}
