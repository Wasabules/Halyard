/* bench_decode.c - the cost of DECODING, measured offline on a recorded stream.
 *
 * Reproduces EXACTLY the configuration of webrtc/h264_decoder.c (the software
 * path) and the access-unit splitting of streaming/vid_reasm.c (rule G40,
 * through vid_wire.c).
 *
 * Measures:
 *   - has_b_frames after avcodec_open2 AND after the first picture comes out
 *   - the number of access units fed BEFORE the first picture comes out
 *   - the time per access unit (send_packet + receive_frame), distribution
 *   - pictures produced per send_packet
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include "vid_wire.h"

static double now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* --- decoupage en NAL puis en unites d'acces (regle G40) --- */
typedef struct { size_t off, len; } nal_t;

static size_t find_nals(const uint8_t *b, size_t n, nal_t *out, size_t cap) {
    size_t cnt = 0, i = 0, start = 0; int have = 0;
    while (i + 3 <= n) {
        int sc4 = (i + 4 <= n) && !b[i] && !b[i+1] && !b[i+2] && b[i+3] == 1;
        int sc3 = !sc4 && !b[i] && !b[i+1] && b[i+2] == 1;
        if (sc4 || sc3) {
            if (have && cnt < cap) { out[cnt].off = start; out[cnt].len = i - start; cnt++; }
            i += sc4 ? 4 : 3; start = i; have = 1; continue;
        }
        i++;
    }
    if (have && start < n && cnt < cap) { out[cnt].off = start; out[cnt].len = n - start; cnt++; }
    return cnt;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : NULL;
    int lowdelay = argc > 2 ? atoi(argv[2]) : 0;
    int limit_au = argc > 3 ? atoi(argv[3]) : 0;      /* 0 = tout */
    if (!path) { fprintf(stderr, "usage: %s <fichier.h264> [lowdelay 0|1] [max_au]\n", argv[0]); return 2; }

    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); return 1; }
    fclose(f);

    size_t ncap = (size_t)sz / 8 + 16;
    nal_t *nals = malloc(ncap * sizeof(nal_t));
    size_t nn = find_nals(buf, (size_t)sz, nals, ncap);

    /* Grouping into access units: a new unit starts at a VCL slice with
     * first_mb==0 (G40), or at a non-VCL NAL when the current unit already
     * contains a slice. */
    size_t *au_start = malloc((nn + 2) * sizeof(size_t));   /* index de NAL */
    size_t nau = 0; int au_has_vcl = 0;
    unsigned n_vcl = 0, n_vcl_mb0 = 0, n_sps = 0, n_pps = 0, n_idr = 0, n_other = 0;
    for (size_t k = 0; k < nn; k++) {
        const uint8_t *p = buf + nals[k].off;
        uint8_t nt = p[0] & 0x1f;
        int is_vcl = (nt == 1 || nt == 5);
        int mb0 = is_vcl && nals[k].len > 1 && vid_wire_first_mb_is_zero(p[1]);
        if (nt == 7) n_sps++; else if (nt == 8) n_pps++;
        else if (is_vcl) { n_vcl++; if (nt == 5) n_idr++; if (mb0) n_vcl_mb0++; }
        else n_other++;
        int starts = au_has_vcl && (mb0 || !is_vcl);
        if (k == 0 || starts) { au_start[nau++] = k; au_has_vcl = 0; }
        if (is_vcl) au_has_vcl = 1;
    }
    au_start[nau] = nn;

    printf("== FLUX %s\n", path);
    printf("   octets=%ld  NAL=%zu  (SPS=%u PPS=%u tranches=%u dont first_mb==0 : %u, IDR=%u, autres=%u)\n",
           sz, nn, n_sps, n_pps, n_vcl, n_vcl_mb0, n_idr, n_other);
    printf("   unites d'acces reconstruites = %zu   (tranches/AU = %.2f)\n",
           nau, nau ? (double)n_vcl / (double)nau : 0.0);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    /* configuration IDENTIQUE a h264_decoder.c, chemin logiciel */
    ctx->thread_count = 1;
    ctx->thread_type  = 0;
    if (lowdelay) ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->has_b_frames = 0;
    ctx->flags  |= AV_CODEC_FLAG_OUTPUT_CORRUPT;   /* SHADOW_OUTPUT_CORRUPT=1 */
    ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;        /* SHADOW_SHOW_ALL=1 */
    ctx->error_concealment = 0;                     /* F2 */
    ctx->err_recognition   = 0;                     /* G29 */
    printf("   has_b_frames AVANT open = %d   (LOW_DELAY=%d)\n", ctx->has_b_frames, lowdelay);
    if (avcodec_open2(ctx, codec, NULL) < 0) { fprintf(stderr, "open2 FAIL\n"); return 1; }
    printf("   has_b_frames APRES open  = %d\n", ctx->has_b_frames);

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();

    double *t_au = malloc(nau * sizeof(double));
    double *t_send = malloc(nau * sizeof(double));
    unsigned au_fed = 0, frames_out = 0, first_out_at = 0, hbf_at_first = -1;
    unsigned au_with_0 = 0, au_with_1 = 0, au_with_2plus = 0, send_err = 0;
    double t_total = 0;
    size_t nmax = limit_au > 0 && (size_t)limit_au < nau ? (size_t)limit_au : nau;

    /* the access unit's buffer: we copy, as flush_access_unit does */
    uint8_t *au = malloc(4u * 1024 * 1024);

    for (size_t a = 0; a < nmax; a++) {
        size_t k0 = au_start[a], k1 = au_start[a + 1];
        size_t off = 0;
        for (size_t k = k0; k < k1; k++) {
            static const uint8_t sc[4] = {0,0,0,1};
            memcpy(au + off, sc, 4); off += 4;
            memcpy(au + off, buf + nals[k].off, nals[k].len); off += nals[k].len;
        }
        av_packet_unref(pkt);
        pkt->data = au; pkt->size = (int)off;
        pkt->pts = pkt->dts = (int64_t)a;

        double t0 = now_us();
        int rc = avcodec_send_packet(ctx, pkt);
        double t1 = now_us();
        if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) send_err++;
        int got = 0;
        while (1) {
            rc = avcodec_receive_frame(ctx, frm);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF || rc < 0) break;
            got++; frames_out++;
            if (frames_out == 1) { first_out_at = (unsigned)a + 1; hbf_at_first = ctx->has_b_frames;
                printf("   1re image : %dx%d fmt=%s pict_type=%d\n", frm->width, frm->height,
                       av_get_pix_fmt_name(frm->format) ? av_get_pix_fmt_name(frm->format) : "?", frm->pict_type); }
            av_frame_unref(frm);
        }
        double t2 = now_us();
        t_send[a] = t1 - t0;
        t_au[a] = t2 - t0;
        t_total += t_au[a];
        au_fed++;
        if (got == 0) au_with_0++; else if (got == 1) au_with_1++; else au_with_2plus++;
    }

    printf("\n== RETENUE DU DECODEUR\n");
    printf("   1re image sortie apres %u unite(s) d'acces fournie(s)\n", first_out_at);
    printf("   has_b_frames a la 1re image = %d\n", hbf_at_first);
    printf("   unites -> 0 image : %u   -> 1 image : %u   -> 2+ images : %u\n",
           au_with_0, au_with_1, au_with_2plus);
    printf("   unites fournies=%u  images sorties=%u  erreurs send=%u\n",
           au_fed, frames_out, send_err);
    printf("   has_b_frames A LA FIN = %d   (delay ctx=%d)\n", ctx->has_b_frames, ctx->delay);

    double *s = malloc(au_fed * sizeof(double));
    memcpy(s, t_au, au_fed * sizeof(double));
    qsort(s, au_fed, sizeof(double), cmp_d);
    double mean = t_total / au_fed, var = 0;
    for (unsigned i = 0; i < au_fed; i++) { double d = t_au[i] - mean; var += d * d; }
    var /= au_fed;
    printf("\n== TEMPS PAR UNITE D'ACCES (send_packet + receive_frame), en ms\n");
    printf("   moyenne=%.3f  ecart-type=%.3f\n", mean / 1000.0, sqrt(var) / 1000.0);
    printf("   min=%.3f  p50=%.3f  p90=%.3f  p95=%.3f  p99=%.3f  p99.9=%.3f  max=%.3f\n",
           s[0]/1000.0, s[au_fed/2]/1000.0, s[(size_t)(au_fed*0.90)]/1000.0,
           s[(size_t)(au_fed*0.95)]/1000.0, s[(size_t)(au_fed*0.99)]/1000.0,
           s[(size_t)(au_fed*0.999)]/1000.0, s[au_fed-1]/1000.0);
    double ts_tot = 0; for (unsigned i = 0; i < au_fed; i++) ts_tot += t_send[i];
    printf("   dont send_packet seul : moyenne=%.3f ms\n", ts_tot / au_fed / 1000.0);
    printf("   sustainable rate = %.1f fps on this processor\n", 1e6 / mean);

    /* the 10 slowest units, with their rank */
    printf("   10 plus lentes (ms) :");
    for (unsigned i = 0; i < 10 && i < au_fed; i++) printf(" %.2f", s[au_fed-1-i]/1000.0);
    printf("\n");

    av_frame_free(&frm); av_packet_free(&pkt); avcodec_free_context(&ctx);
    return 0;
}
