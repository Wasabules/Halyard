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

/* === QR1 2026-10-03 — THE MODULE GRID, FOR A CLIENT THAT CAN DRAW ========
 *
 * The BMP above exists because Borealis loads images from files
 * (`brls::Image::setImageFromFile`), so a path was the only currency it
 * accepted. A Qt client can paint, and routing a QR through a temporary file
 * to do so means a disk write, a path to invent, a read back, and a file to
 * delete on every refresh - for a few hundred bits.
 *
 * `qr_matrix` hands over the grid instead: `*width` modules a side, and
 * `out[y * width + x]` non-zero where the module is dark. The QUIET ZONE is
 * NOT included - it is a rendering decision (four modules is the standard
 * minimum) and a caller drawing into a padded widget already has the margin.
 *
 * `out` must hold `cap` bytes; the needed size is `width * width`, and the
 * call fails rather than truncating if it does not fit. A version-40 code is
 * 177 modules, so 32 KiB covers anything libqrencode can produce - which is
 * why this takes a buffer from the caller instead of allocating.
 *
 * Returns false when libqrencode is absent (consoles) or the text cannot be
 * encoded; `*width` is then 0 and the caller falls back to showing the URL as
 * text, exactly as the BMP path already does. */
bool qr_matrix(const char *text, unsigned char *out, int cap, int *width);

#ifdef __cplusplus
}
#endif
