/* cursor_state - see header. */

#include "cursor_state.h"
#include "../services/log.h"

/* S81 - this module's log category. See shadow/journal.h: it is declared here,
 * never inferred from the text of the messages. */
#define cslog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
#define csdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_VIDEO, __VA_ARGS__)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define clog(fmt, ...) cslog("[cursor] " fmt, ##__VA_ARGS__)

#define BITMAP_MAX 65536  /* large enough for big cursors */

typedef enum {
    MODE_DELTA         = 0,  /* pos += raw_xy on every event */
    MODE_ABS_CENTERED  = 1,  /* pos = center + raw_xy en pixels (= signed offset from center) */
    MODE_ABS_NORMALIZED = 2  /* pos = (raw / max) * display (= unsigned mapped to screen) */
} cursor_mode_t;

static struct {
    pthread_mutex_t mtx;
    bool initialized;
    /* Position state */
    int  x, y;
    int  display_w, display_h;
    bool has_pos;
    cursor_mode_t mode;
    /* Bitmap cache (= last received shape) */
    uint8_t bitmap[BITMAP_MAX];
    size_t  bitmap_len;
    /* === S54 2026-08-26 - THE IMAGE COMING FROM `:base+20` ===
     *
     * Deliberately kept SEPARATE from `bitmap` above. That one is fed by
     * `cursor_state_feed()` from `:base+30`, on the strength of a channel map
     * that turned out to be wrong: `:base+30` is the audio (KB.md §3.37), and
     * what it read there as "positions" were Opus frames.
     *
     * Rather than rewrite that path - and risk breaking what works - we ADD the
     * correct source alongside it. The two do not step on each other, each
     * stays testable on its own, and the old one can be removed once the new
     * one has proved itself live. */
    uint8_t  img[BITMAP_MAX];
    size_t   img_len;
    uint32_t img_w, img_h, img_stride, img_format;
    uint32_t img_hot_x, img_hot_y;
    bool     img_valid;    /* an image has been received */
    bool     img_hidden;    /* the server asks to hide the pointer */
    uint32_t img_received, img_hidden_count;
    /* Stats */
    cursor_state_stats_t stats;
} g;

static void parse_mode_env(void) {
    const char *e = getenv("SHADOW_CURSOR_MODE");
    if (!e) { g.mode = MODE_ABS_CENTERED; return; }  /* default = centered (= consistent with the observed bytes) */
    if (strcmp(e, "delta") == 0)        g.mode = MODE_DELTA;
    else if (strcmp(e, "centered") == 0 || strcmp(e, "1") == 0) g.mode = MODE_ABS_CENTERED;
    else if (strcmp(e, "normalized") == 0 || strcmp(e, "2") == 0) g.mode = MODE_ABS_NORMALIZED;
    else g.mode = MODE_ABS_CENTERED;
}

static const char *mode_name(cursor_mode_t m) {
    switch (m) {
        case MODE_DELTA: return "delta";
        case MODE_ABS_CENTERED: return "abs_centered";
        case MODE_ABS_NORMALIZED: return "abs_normalized";
    }
    return "?";
}

void cursor_state_init(void) {
    if (g.initialized) return;
    pthread_mutex_init(&g.mtx, NULL);
    g.x = 960;             /* center 1920×1080 */
    g.y = 540;
    g.display_w = 1920;
    g.display_h = 1080;
    g.has_pos   = false;
    g.bitmap_len = 0;
    memset(&g.stats, 0, sizeof(g.stats));
    parse_mode_env();
    g.initialized = true;
    /* The SIZE is deliberately not logged here. It is the placeholder 1920x1080
     * that `cursor_state_set_display` overwrites on the very next line of the
     * caller, so printing it announces a coordinate space the session never
     * uses - which reads, in a 720p log, exactly like a bug. Measured cost of
     * that line: one detour per reader. The size is announced where it is
     * actually decided. */
    clog("init mode=%s", mode_name(g.mode));
}

void cursor_state_destroy(void) {
    if (!g.initialized) return;
    pthread_mutex_destroy(&g.mtx);
    g.initialized = false;
}

void cursor_state_set_display(int width, int height) {
    if (!g.initialized) cursor_state_init();
    pthread_mutex_lock(&g.mtx);
    if (width > 0)  g.display_w = width;
    if (height > 0) g.display_h = height;
    clog("coordinate space %dx%d", g.display_w, g.display_h);
    /* Re-center if no position yet */
    if (!g.has_pos) {
        g.x = g.display_w / 2;
        g.y = g.display_h / 2;
    }
    pthread_mutex_unlock(&g.mtx);
}

/* Parse a type=0x02 cursor bitmap shape (= 271 B observed).
 * The format still has to be reverse-engineered byte-exact; for Phase 2 we
 * simply store the raw payload. */
static int parse_bitmap(const uint8_t *p, size_t len) {
    if (len > BITMAP_MAX) return -1;
    pthread_mutex_lock(&g.mtx);
    memcpy(g.bitmap, p, len);
    g.bitmap_len = len;
    g.stats.bitmap_count++;
    pthread_mutex_unlock(&g.mtx);
    static int g_log = 0;
    if (g_log < 3) {
        clog("bitmap update #%u (%zu B)", g.stats.bitmap_count, len);
        g_log++;
    }
    return 0;
}

/* Parse a type=0x12 position update (= 8 B observed).
 * Hypothesised format:
 *   byte 0 = 0x12
 *   byte 1 = seq
 *   bytes 2-4 = reserved zeros
 *   bytes 5-6 = X (= int16 LE)
 *   byte 7 = Y (= int8 signed)
 *
 * DELTA mode: bytes 5-7 are cumulative signed deltas.
 * ABSOLUTE mode: bytes 5-7 are normalised absolute coordinates
 *   (X mapped [0..0xFFFF] -> [0..display_w], Y mapped [-128..127] -> [0..display_h]). */
static int parse_position(const uint8_t *p, size_t len) {
    if (len < 8) return -1;
    int16_t raw_x = (int16_t)((uint16_t)p[5] | ((uint16_t)p[6] << 8));
    int8_t  raw_y = (int8_t)p[7];

    pthread_mutex_lock(&g.mtx);
    switch (g.mode) {
        case MODE_DELTA:
            g.x += raw_x;
            g.y += raw_y;
            break;
        case MODE_ABS_CENTERED:
            /* raw_xy signed, in pixels relative to the center. */
            g.x = g.display_w / 2 + raw_x;
            g.y = g.display_h / 2 + raw_y;
            break;
        case MODE_ABS_NORMALIZED:
            /* raw_x mapped [0..0xFFFF] -> [0..display_w]
             * raw_y mapped [0..0xFF] -> [0..display_h] (= fixed-point ratio) */
            g.x = (int)(((int64_t)((uint16_t)raw_x) * g.display_w) / 0xFFFF);
            g.y = (int)(((int64_t)((uint8_t)raw_y) * g.display_h) / 0xFF);
            break;
    }
    /* Clip */
    if (g.x < 0) g.x = 0;
    if (g.y < 0) g.y = 0;
    if (g.x >= g.display_w) g.x = g.display_w - 1;
    if (g.y >= g.display_h) g.y = g.display_h - 1;
    g.has_pos = true;
    g.stats.position_count++;
    g.stats.last_x = g.x;
    g.stats.last_y = g.y;
    pthread_mutex_unlock(&g.mtx);

    static int g_log = 0;
    if (g_log < 5) {
        clog("pos update #%u raw=(%d,%d) → (%d,%d)",
             g.stats.position_count, raw_x, (int)raw_y, g.stats.last_x, g.stats.last_y);
        g_log++;
    }
    return 0;
}

int cursor_state_feed(const uint8_t *payload, size_t len) {
    if (!g.initialized) cursor_state_init();
    if (!payload || len < 1) {
        pthread_mutex_lock(&g.mtx);
        g.stats.parse_errors++;
        pthread_mutex_unlock(&g.mtx);
        return -1;
    }
    pthread_mutex_lock(&g.mtx);
    g.stats.feed_count++;
    pthread_mutex_unlock(&g.mtx);

    uint8_t type = payload[0];
    if (type == 0x02) {
        return parse_bitmap(payload, len);
    } else if (type == 0x12) {
        return parse_position(payload, len);
    }
    /* Unknown type - log the first occurrences */
    static int g_unk_log = 0;
    if (g_unk_log < 5) {
        clog("unknown type=0x%02x len=%zu", type, len);
        g_unk_log++;
    }
    pthread_mutex_lock(&g.mtx);
    g.stats.parse_errors++;
    pthread_mutex_unlock(&g.mtx);
    return -1;
}

void cursor_state_get_position(int *x, int *y, bool *has_pos) {
    if (!g.initialized) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (has_pos) *has_pos = false;
        return;
    }
    pthread_mutex_lock(&g.mtx);
    if (x) *x = g.x;
    if (y) *y = g.y;
    if (has_pos) *has_pos = g.has_pos;
    pthread_mutex_unlock(&g.mtx);
}

const uint8_t *cursor_state_get_bitmap_payload(size_t *len) {
    if (!g.initialized || g.bitmap_len == 0) {
        if (len) *len = 0;
        return NULL;
    }
    /* Note: the lifetime guarantee is "until the next bitmap feed". A GUI
     * thread reading this should snapshot the bytes quickly. The race is
     * acceptable since the data is only a visual hint. */
    if (len) *len = g.bitmap_len;
    return g.bitmap;
}

void cursor_state_get_stats(cursor_state_stats_t *out) {
    if (!g.initialized || !out) return;
    pthread_mutex_lock(&g.mtx);
    *out = g.stats;
    pthread_mutex_unlock(&g.mtx);
}

/* RE2 2026-05-18 - Parse the cached bitmap, byte-exact layout (= Agent A
 * finding). Layout proven for the 271 B sample:
 *   [0] type=0x02, [1-4] reserved, [5-6] w u16_LE, [7-8] h u16_LE,
 *   [9-10] hotspot_x u16_LE, [11] hotspot_y u8, [12] format u8,
 *   [13] flags u8, [14-15] stride u16_LE, [16+] pixels.
 *
 * For the 271 B we observed: w=1 h=1 -> 16 B header + 256 B pixels = 272, not
 * 271. Off-by-one; hotspot_y may in fact be a u16_LE (= everything after it
 * shifts by 1 B). To be settled with a capture of a non-blank cursor. For the
 * MVP we take it as it stands. */
bool cursor_state_get_bitmap_parsed(cursor_bitmap_info_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!g.initialized) return false;
    pthread_mutex_lock(&g.mtx);
    if (g.bitmap_len < 16 || g.bitmap[0] != 0x02) {
        pthread_mutex_unlock(&g.mtx);
        return false;
    }
    out->valid     = true;
    out->width     = (uint16_t)g.bitmap[5]  | ((uint16_t)g.bitmap[6]  << 8);
    out->height    = (uint16_t)g.bitmap[7]  | ((uint16_t)g.bitmap[8]  << 8);
    out->hotspot_x = (uint16_t)g.bitmap[9]  | ((uint16_t)g.bitmap[10] << 8);
    out->hotspot_y = g.bitmap[11];
    out->format    = g.bitmap[12];
    out->flags     = g.bitmap[13];
    out->stride    = (uint16_t)g.bitmap[14] | ((uint16_t)g.bitmap[15] << 8);
    /* Pixels start at offset 16. Length = bitmap_len - 16. */
    if (g.bitmap_len > 16) {
        out->pixels     = g.bitmap + 16;
        out->pixels_len = g.bitmap_len - 16;
    } else {
        out->pixels     = NULL;
        out->pixels_len = 0;
    }
    pthread_mutex_unlock(&g.mtx);
    return true;
}

/* === S54 - cursor image coming from `:base+20` ===
 *
 * `pixels == NULL` (or `size == 0`) means: the server asks for the pointer to
 * be HIDDEN. That is not an error - 21 frames out of 140 are in that case (a
 * text field, a game that captures the mouse).
 *
 * The caller has already validated the geometry against the size received
 * (`cursor_wire_parse_image`); we bound it against our own buffer anyway: this
 * is the last thing standing between the network and a `memcpy`. */
void cursor_state_feed_image(const uint8_t *pixels, uint32_t size,
                             uint32_t width, uint32_t height, uint32_t stride,
                             uint32_t format, uint32_t hot_x, uint32_t hot_y)
{
    if (!g.initialized) cursor_state_init();
    pthread_mutex_lock(&g.mtx);
    g.img_received++;
    if (!pixels || size == 0) {
        g.img_hidden = true;
        g.img_hidden_count++;
    } else if (size <= sizeof(g.img)) {
        memcpy(g.img, pixels, size);
        g.img_len     = size;
        g.img_w       = width;
        g.img_h       = height;
        g.img_stride     = stride;
        g.img_format  = format;
        g.img_hot_x = hot_x;
        g.img_hot_y = hot_y;
        g.img_valid  = true;
        g.img_hidden  = false;
    }
    pthread_mutex_unlock(&g.mtx);
}

/* Returns false as long as no image has been received, or if the pointer is
 * hidden. The buffer returned belongs to the module and stays valid until the
 * next image arrives; the caller reads it under the same lock as the rest of
 * the module. */
bool cursor_state_get_image(cursor_image_t *out)
{
    if (!out || !g.initialized) return false;
    bool ok = false;
    pthread_mutex_lock(&g.mtx);
    if (g.img_valid && !g.img_hidden) {
        out->pixels  = g.img;
        out->size  = g.img_len;
        out->width = g.img_w;
        out->height = g.img_h;
        out->stride     = g.img_stride;
        out->format  = g.img_format;
        out->hot_x = g.img_hot_x;
        out->hot_y = g.img_hot_y;
        ok = true;
    }
    pthread_mutex_unlock(&g.mtx);
    return ok;
}

void cursor_state_get_image_stats(uint32_t *received, uint32_t *hidden_count)
{
    if (!g.initialized) { if (received) *received = 0; if (hidden_count) *hidden_count = 0; return; }
    pthread_mutex_lock(&g.mtx);
    if (received)   *received   = g.img_received;
    if (hidden_count) *hidden_count = g.img_hidden_count;
    pthread_mutex_unlock(&g.mtx);
}
