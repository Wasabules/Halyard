/* qr_helper - a small wrapper that generates a QR code as a BMP from a text.
 * Uses libqrencode (= apt install libqrencode-dev).
 *
 * Output: a BMP file you can load with brls::Image::setImageFromFile.
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generates a BMP QR code encoding `text` (a URL, or a URL plus a code) into
 * `out_path`. `pixel_size` = the size in pixels of one QR module (= a zoom
 * factor, 8 to 12 recommended). Returns true on success. */
bool qr_generate_bmp(const char *text, const char *out_path, int pixel_size);

#ifdef __cplusplus
}
#endif
