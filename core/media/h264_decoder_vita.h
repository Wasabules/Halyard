/* h264_decoder_vita.h - the PS Vita's HARDWARE H.264 decoder.
 *
 * WHY IT HAD TO EXIST. The vitasdk's libavcodec ships 22 decoders and H.264 is
 * not one of them (h263, mpeg4, svq1, cinepak are; so are FLAC and Opus, which
 * is why the audio path works). Measured on console 2026-09-13:
 * `avcodec_find_decoder FAIL`, then three reconnect attempts and a clean give
 * up. Building a local ffmpeg with H.264 would compile, but a 444 MHz
 * Cortex-A9 will not decode 720p in software: on this machine the hardware is
 * not an optimisation, it is the only option.
 *
 * WHAT MAKES IT A GOOD FIT. `SceAvcdecAu` takes an ELEMENTARY STREAM buffer
 * with a PTS - which is exactly what `streaming/vid_reasm.c` already produces,
 * one Annex-B access unit per picture. Nothing upstream changes.
 *
 * NOT VALIDATED ON HARDWARE. It follows the SDK headers and nobody has seen a
 * picture from it yet. docs/PSVITA_PORT.md.
 */
#ifndef SHADOW_H264_DECODER_VITA_H
#define SHADOW_H264_DECODER_VITA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h264_vita h264_vita;

/* One decoded picture, in the shape `h264_frame_cb` wants: NV12, so `y` is the
 * luma plane and `uv` the interleaved chroma, both with the same pitch. */
typedef struct {
    const uint8_t *y;
    const uint8_t *uv;
    int            pitch;      /* bytes per line, both planes */
    int            width;      /* the CROPPED, displayable size */
    int            height;
    int64_t        pts;
} h264_vita_picture;

/* Opens the library and the decoder for `width` x `height`. `ref_frames` is the
 * DPB depth to reserve - the stream's `max_num_ref_frames`; 4 is what Shadow
 * has been observed to use, and over-reserving only costs memory.
 * NULL on failure, with the reason logged. */
h264_vita *h264_vita_open(int width, int height, int ref_frames);

/* Feeds one Annex-B access unit. Returns 1 when `out` was filled, 0 when the
 * decoder consumed the unit without producing a picture yet, <0 on error. */
int h264_vita_decode(h264_vita *d, const uint8_t *au, size_t len, int64_t pts,
                     h264_vita_picture *out);

void h264_vita_close(h264_vita *d);

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_H264_DECODER_VITA_H */
