/* cursor_state - track cursor position + bitmap from Shadow VM updates.
 *
 * CUR1 Phase 2 2026-05-18. Receives the payloads decoded by ctrl_session
 * (= post-chacha20 decrypt, post-SUFP bypass) and maintains a cursor state
 * exposed to the GUI (stream_view) for the overlay render.
 *
 * 2 kinds of Shadow event:
 *   - byte 0 = 0x02: bitmap shape upload (= 271 B, header + BGRA pixels)
 *   - byte 0 = 0x12: position update (= 8 B of coordinates)
 *
 * The bitmap format has not been reverse-engineered byte-exact yet; we store
 * the raw payload for a future decode. Position update: 2 parsing modes -
 * DELTA or ABSOLUTE - selected through the env var SHADOW_CURSOR_MODE
 * (default DELTA).
 *
 * Thread-safety: cursor_state_feed may be called from the UDP RX thread,
 * cursor_state_get_* from the UI thread (= protected by mutex).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init/destroy singleton. Idempotent. */
void cursor_state_init(void);
void cursor_state_destroy(void);

/* Feed one received payload (= post-decrypt cursor frame). Parses byte 0 to
 * determine the type, then dispatches.
 * Returns 0 when parsed OK, -1 on an unknown format. */
int  cursor_state_feed(const uint8_t *payload, size_t len);

/* === S54 2026-08-26 - THE POINTER IMAGE, COMING FROM `:base+20` ===
 *
 * `cursor_state_feed()` above is fed by `:base+30` on the strength of a channel
 * map that turned out to be wrong: `:base+30` is the AUDIO, and what it was
 * reading there as positions were Opus frames (KB.md §3.37). The real cursor
 * channel is `:base+20`, and it carries ONLY the shape - the server never sends
 * a position.
 *
 * `pixels == NULL` or `size == 0`: hide the pointer. */
typedef struct {
    const uint8_t *pixels;
    size_t         size;
    uint32_t       width, height, stride, format;
    uint32_t       hot_x, hot_y;
} cursor_image_t;

void cursor_state_feed_image(const uint8_t *pixels, uint32_t size,
                             uint32_t width, uint32_t height, uint32_t stride,
                             uint32_t format, uint32_t hot_x, uint32_t hot_y);

/* false as long as no image has arrived, or if the pointer is hidden. */
bool cursor_state_get_image(cursor_image_t *out);

void cursor_state_get_image_stats(uint32_t *received, uint32_t *hidden_count);

/* Get the current cursor position (= last update received).
 * `*x` and `*y` in pixels, relative to display_width/display_height.
 * `*has_pos` = false when no update has been received yet. */
void cursor_state_get_position(int *x, int *y, bool *has_pos);

/* Get the bitmap if available. Returns NULL+0 if none has arrived yet.
 * Lifetime: the pointer stays valid until the next cursor_state_feed with
 * type=0x02. The caller must NOT modify it. */
const uint8_t *cursor_state_get_bitmap_payload(size_t *len);

/* RE2 2026-05-18 - Parsed bitmap struct (= Agent A decoded layout).
 * Byte-exact format for the 271 B sample observed:
 *   [0]    type        = 0x02
 *   [1-4]  reserved
 *   [5-6]  width  u16_LE
 *   [7-8]  height u16_LE
 *   [9-10] hotspot_x u16_LE
 *   [11]   hotspot_y u8
 *   [12]   format u8
 *   [13]   flags u8
 *   [14-15] stride u16_LE
 *   [16+]  pixels (= width x height x bpp, depending on format)
 * Note: the 271 B observed is a blank initial cursor (w=h=1, pixels mostly
 * zeros). Bigger cursor shapes are needed to confirm the pixels[] layout
 * (BGRA vs palette). */
typedef struct {
    bool     valid;
    uint16_t width;
    uint16_t height;
    uint16_t hotspot_x;
    uint8_t  hotspot_y;
    uint8_t  format;
    uint8_t  flags;
    uint16_t stride;
    const uint8_t *pixels;
    size_t   pixels_len;
} cursor_bitmap_info_t;

/* Parse the currently cached bitmap (= last type=0x02 received). Returns true
 * if valid. */
bool cursor_state_get_bitmap_parsed(cursor_bitmap_info_t *out);

/* Set the display dimensions (= used to clip the coordinates).
 * Default 1920x1080. */
void cursor_state_set_display(int width, int height);

typedef struct {
    uint32_t feed_count;
    uint32_t bitmap_count;
    uint32_t position_count;
    uint32_t parse_errors;
    int      last_x;
    int      last_y;
} cursor_state_stats_t;

void cursor_state_get_stats(cursor_state_stats_t *out);

#ifdef __cplusplus
}
#endif
