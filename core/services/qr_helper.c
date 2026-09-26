/* qr_helper.c - see the header. Generates a 24-bit BMP from a QR code. */
#include "qr_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* WHETHER THIS TARGET HAS libqrencode, named rather than guessed from the
 * platform. Neither the Switch's portlibs nor the Vita's package it, and that
 * is a property of the toolchain, not of the console -- if either ever ships
 * it, define SHADOW_HAVE_QRENCODE=1 and nothing else here changes.
 *
 * Losing the QR costs nothing structural: the caller (boot_activity) falls onto
 * the "QR unavailable" path and shows the code and the URL as text, which the
 * catalogue already has a string for (`boot/grant_no_qr`). */
#ifndef SHADOW_HAVE_QRENCODE
#  if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
#    define SHADOW_HAVE_QRENCODE 0
#  else
#    define SHADOW_HAVE_QRENCODE 1
#  endif
#endif

#if !SHADOW_HAVE_QRENCODE
bool qr_generate_bmp(const char *text, const char *out_path, int pixel_size) {
    (void)text; (void)out_path; (void)pixel_size;
    return false;
}
#else

#include <qrencode.h>

/* Petit writer BMP 24-bit. */
static bool write_bmp_24(const char *path, const uint8_t *rgb, int w, int h) {
    int row_bytes = w * 3;
    int padding = (4 - (row_bytes % 4)) % 4;
    int data_size = (row_bytes + padding) * h;
    int file_size = 54 + data_size;

    FILE *f = fopen(path, "wb");
    if (!f) return false;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size & 0xFF; hdr[3] = (file_size>>8)&0xFF;
    hdr[4] = (file_size>>16)&0xFF; hdr[5] = (file_size>>24)&0xFF;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xFF; hdr[19] = (w>>8)&0xFF;
    hdr[20] = (w>>16)&0xFF; hdr[21] = (w>>24)&0xFF;
    hdr[22] = h & 0xFF; hdr[23] = (h>>8)&0xFF;
    hdr[24] = (h>>16)&0xFF; hdr[25] = (h>>24)&0xFF;
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = data_size & 0xFF; hdr[35] = (data_size>>8)&0xFF;
    hdr[36] = (data_size>>16)&0xFF; hdr[37] = (data_size>>24)&0xFF;
    fwrite(hdr, 1, 54, f);

    uint8_t pad_bytes[4] = {0,0,0,0};
    /* BMP rows go bottom-up */
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *row = rgb + y * w * 3;
        /* BMP is BGR, swap */
        for (int x = 0; x < w; x++) {
            uint8_t bgr[3] = { row[x*3+2], row[x*3+1], row[x*3] };
            fwrite(bgr, 1, 3, f);
        }
        if (padding) fwrite(pad_bytes, 1, padding, f);
    }
    fclose(f);
    return true;
}

bool qr_generate_bmp(const char *text, const char *out_path, int pixel_size) {
    if (!text || !out_path) return false;
    if (pixel_size < 1) pixel_size = 8;

    QRcode *qr = QRcode_encodeString(text, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!qr) return false;

    int q = 4;  /* quiet zone (modules) */
    int modules = qr->width;
    int img_w = (modules + 2*q) * pixel_size;
    int img_h = img_w;

    uint8_t *img = (uint8_t *)malloc((size_t)img_w * img_h * 3);
    if (!img) { QRcode_free(qr); return false; }
    memset(img, 0xFF, (size_t)img_w * img_h * 3);  /* white bg */

    for (int my = 0; my < modules; my++) {
        for (int mx = 0; mx < modules; mx++) {
            int dark = qr->data[my * modules + mx] & 1;
            if (!dark) continue;
            for (int py = 0; py < pixel_size; py++) {
                for (int px = 0; px < pixel_size; px++) {
                    int x = (q + mx) * pixel_size + px;
                    int y = (q + my) * pixel_size + py;
                    uint8_t *p = img + (y * img_w + x) * 3;
                    p[0] = p[1] = p[2] = 0;  /* black */
                }
            }
        }
    }

    bool ok = write_bmp_24(out_path, img, img_w, img_h);
    free(img);
    QRcode_free(qr);
    return ok;
}

#endif /* __SWITCH__ */
