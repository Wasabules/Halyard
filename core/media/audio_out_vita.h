/* audio_out_vita.h - the PS Vita audio sink behind media/audio.c.
 *
 * WHY IT LOOKS LIKE THE ALSA PATH AND NOT THE WINDOWS ONE. `sceAudioOutOutput`
 * BLOCKS until the device has consumed the previous buffer, so the device paces
 * the play thread exactly as ALSA does. WASAPI needs its own loop because it is
 * driven by a period event instead; this one goes behind the shared ring and
 * the shared `blocking_play_loop`.
 *
 * THE GRAIN IS THE WHOLE REASON THIS FILE EXISTS. `sceAudioOutOpenPort` takes a
 * length in frames that MUST be a multiple of 64. The play thread writes 480
 * frames (10 ms at 48 kHz), and 480 is not: 64 x 7.5. Rather than bend the
 * shared chunk to one device -- which would change what every other backend
 * measures -- this wrapper accumulates and emits whole grains. The mismatch
 * belongs to the device that has it.
 *
 * NOT VALIDATED ON HARDWARE. It compiles for arm-vita-eabi and its shape
 * follows the SDK headers. Nobody has heard it, no latency has been measured,
 * and the grain below is a reasoned choice rather than a measured one. Treat
 * every number in it as a starting point. docs/PSVITA_PORT.md.
 */
#ifndef SHADOW_AUDIO_OUT_VITA_H
#define SHADOW_AUDIO_OUT_VITA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_out_vita audio_out_vita;

/* What the port was actually opened with - the grain is rounded up to the
 * device's multiple of 64, so the caller can report the truth rather than the
 * request. */
typedef struct {
    int rate;        /* Hz, as granted */
    int channels;
    int grain;       /* frames per sceAudioOutOutput call */
    int grain_us;    /* the same, in microseconds - the floor of this sink's
                        added latency, since a partial grain waits */
} audio_out_vita_info;

/* Which of the device's ports to take. The Vita has several, and that is what
 * makes it different from the Switch: AUDC-1 was an entire campaign spent
 * handing ONE audout back and forth between the stream and the interface
 * sounds. Here they can each hold their own. */
typedef enum {
    AUDIO_OUT_VITA_MAIN = 0,   /* the stream */
    AUDIO_OUT_VITA_BGM  = 1,   /* the interface sounds */
} audio_out_vita_port;

/* Opens a port. Returns NULL on failure, and fills `info` on success.
 * `rate` must be one the device accepts (8000, 11025, 12000, 16000, 22050,
 * 24000, 32000, 44100, 48000); the stream is 48000. */
audio_out_vita *audio_out_vita_open(audio_out_vita_port port, int rate, int channels,
                                    audio_out_vita_info *info);

/* Writes `frames` interleaved frames. BLOCKS while the device consumes whole
 * grains; a remainder is held until the next call completes it, so a caller
 * writing 480 at a time sees the device's pace and not the grain's.
 *
 * Returns the frames ACCEPTED (always `frames` on success, including those
 * still held), or a negative Sce error from the failing write. */
int audio_out_vita_write(audio_out_vita *o, const int16_t *pcm, int frames);

/* Frames the device still holds (`sceAudioOutGetRestSample`) PLUS whatever this
 * wrapper is holding back as a partial grain - the caller measures a queue, and
 * the remainder is as much in front of the listener as the device's own. -1
 * when the device cannot say. This is what feeds L12. */
int audio_out_vita_queued(audio_out_vita *o);

/* Releases the port. NULL is a no-op. */
void audio_out_vita_close(audio_out_vita *o);

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_AUDIO_OUT_VITA_H */
