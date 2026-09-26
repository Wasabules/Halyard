// H.264 decoder stack:
//   1. Annex-B access units from the native session (vid_reasm.c)
//   2. libavcodec decode -> YUV (NV12 or YUV420P), through a hardware
//      accelerator when the platform offers one (the Switch's NVDEC)
//   3. Frame callback, consumed by the display
//
// (The RTP / RFC 6184 depacketiser of the WebRTC path was removed with that
// path on 2026-09-26.)

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h264_decoder h264_decoder;

// Called for every decoded YUV frame.
// data_y/u/v : plans (NV12 = data_u contains UV interleaved, data_v=NULL)
// linesize_*: strides (may be > width, for padding)
typedef void (*h264_frame_cb)(int width, int height,
                               const uint8_t *data_y, int linesize_y,
                               const uint8_t *data_u, int linesize_u,
                               const uint8_t *data_v, int linesize_v,
                               int format,    // AVPixelFormat (e.g. NV12=23 ou YUV420P=0)
                               int64_t pts,
                               void *user);

h264_decoder *h264_decoder_create(h264_frame_cb on_frame, void *user);

/* === K18 2026-08-28 - SAY WHAT WE DO, NOT WHAT WE ASKED FOR ===
 *
 * The diagnostic panel showed neither the codec nor the decode mode. One had to
 * go and read the console's log to learn that H.265 was running in HARDWARE -
 * information the screen should have given.
 *
 * These two functions return the decoder's REAL state, not the requested
 * setting. The distinction is not theoretical: the hardware fallback is sticky
 * (S28), and a `hwaccel.disabled` marker left by a previous session forces
 * software without the setting changing. Showing the setting would lie. */
const char *h264_decoder_codec_name(const h264_decoder *d);
/* 1 = a hardware picture has been delivered, 0 = software decoding. */
int         h264_decoder_hw_active(const h264_decoder *d);
void          h264_decoder_destroy(h264_decoder *d);

// Feed an H.264 Annex-B buffer (a sequence of NAL units prefixed by
// `00 00 00 01` or `00 00 01`). Used by ctrl_session (the native Shadow
// streaming path), which produces Annex-B after stripping the VideoFrame
// header, not RFC 6184.
//
// pts: a 90 kHz tick.
void h264_decoder_feed_annexb(h264_decoder *d,
                                const uint8_t *buf, size_t len,
                                uint32_t pts);

// Stats - for debugging and the UI.
typedef struct {
    uint32_t frames_decoded;
    uint32_t nals_total;
    uint32_t single_nal_count;
    uint32_t decode_errors;
    int      last_width;
    int      last_height;
    // Async transfer worker (hwaccel only) :
    uint32_t async_processed;  // frames that went through av_hwframe_transfer_data
    uint32_t async_dropped;    // frames dropped because the async queue was full
    uint32_t bitstream_errors; // messages d'erreur libavcodec (MB corrompu,
                               // frame num gap, concealment) = derive de reference
} h264_decoder_stats_t;

void h264_decoder_get_stats(h264_decoder *d, h264_decoder_stats_t *out);

/* COL1 2026-09-10 - the colorimetry the stream DECLARES in its SPS (VUI), as of
 * the last decoded picture. `matrix`: 601, 709 or 2020; 0 when the stream does
 * not say (or names a matrix the shader does not model); -1 before the first
 * picture of the session. `full_range`: 1 full, 0 limited, -1 not declared.
 * What to do with "not declared" is the renderer's decision, not this one's. */
void h264_decoder_colorimetry(int *matrix, int *full_range);

#ifdef __cplusplus
}
#endif
