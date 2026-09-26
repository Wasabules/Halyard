/* audio_out_vita.c - SceAudioOut behind the shared ring. See the header for
 * WHY the grain accumulator exists and what has NOT been validated.
 *
 * Compiled on every platform, like `audio_out_win.c`: outside the Vita it
 * yields no symbol at all, so no build needs to know about it.
 */
#include "audio_out_vita.h"

#if defined(__vita__) || defined(__psp2__)

#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>

/* The device wants a multiple of 64. 512 frames is 10.67 ms at 48 kHz - the
 * nearest legal grain above the play thread's 480-frame chunk, so a chunk
 * almost fills a grain and the remainder never grows.
 *
 * A SMALLER grain would mean less latency and more calls; a larger one the
 * reverse. 512 is a reasoned starting point, NOT a measured one: on this device
 * nobody has yet measured what a wake costs against what the queue holds. When
 * someone does, this is the constant to move, and `grain_us` in the info struct
 * is what makes its cost visible from the outside. */
#define VITA_GRAIN 512

struct audio_out_vita {
    int      port;
    int      channels;
    int      grain;                    /* frames per device write */
    /* === TWO BUFFERS, AND THAT IS NOT AN OPTIMISATION =====================
     *
     * `sceAudioOutOutput` does NOT copy what you hand it. It returns when the
     * PREVIOUS grain has finished playing, which means the grain just passed
     * is the one the hardware is reading from at that moment. Filling a single
     * accumulator therefore overwrites the samples currently being played.
     *
     * Reported from a real console 2026-09-13: "image noire, son brouille et
     * tres gresillant". Every counter was clean - rate settled at 1.00, ring
     * overruns frozen at 42 (all during the startup catch-up), 0 discards, 0
     * decode errors - which is precisely what pointed here: the fault had to
     * be below everything the pipeline measures, in the one part that was new
     * and observed by nothing.
     *
     * Two grains alternating is the minimum: while the device plays `acc[cur]`
     * we fill `acc[cur ^ 1]`. */
    int16_t *acc[2];                   /* grain x channels each */
    int      cur;                      /* the one being filled */
    int      acc_frames;               /* what it currently holds */
};

audio_out_vita *audio_out_vita_open(audio_out_vita_port port, int rate, int channels,
                                    audio_out_vita_info *info)
{
    if (channels != 1 && channels != 2) return NULL;

    audio_out_vita *o = (audio_out_vita *)calloc(1, sizeof *o);
    if (!o) return NULL;

    o->channels = channels;
    o->grain    = VITA_GRAIN;
    const size_t bytes = (size_t)o->grain * (size_t)channels * sizeof **o->acc;
    o->acc[0] = (int16_t *)malloc(bytes);
    o->acc[1] = (int16_t *)malloc(bytes);
    o->cur    = 0;
    if (!o->acc[0] || !o->acc[1]) {
        free(o->acc[0]); free(o->acc[1]); free(o); return NULL;
    }

    const SceAudioOutMode mode = (channels == 2) ? SCE_AUDIO_OUT_MODE_STEREO
                                                 : SCE_AUDIO_OUT_MODE_MONO;
    const SceAudioOutPortType t = (port == AUDIO_OUT_VITA_BGM)
                                ? SCE_AUDIO_OUT_PORT_TYPE_BGM
                                : SCE_AUDIO_OUT_PORT_TYPE_MAIN;
    o->port = sceAudioOutOpenPort(t, o->grain, rate, mode);
    if (o->port < 0) {
        free(o->acc[0]); free(o->acc[1]); free(o); return NULL;
    }

    /* The port opens at full volume already, but saying so explicitly keeps the
     * device out of the argument when the stream sounds quiet: our own gain is
     * applied in `audio.c`, at the last moment, and it is the only one that
     * should be moving. */
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(o->port,
                         (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH |
                                                  SCE_AUDIO_VOLUME_FLAG_R_CH), vol);

    if (info) {
        info->rate     = rate;
        info->channels = channels;
        info->grain    = o->grain;
        info->grain_us = (int)((int64_t)o->grain * 1000000 / rate);
    }
    return o;
}

int audio_out_vita_write(audio_out_vita *o, const int16_t *pcm, int frames)
{
    if (!o || !pcm || frames < 0) return -1;

    const int ch  = o->channels;
    int       done = 0;

    while (done < frames) {
        const int room = o->grain - o->acc_frames;
        int take = frames - done;
        if (take > room) take = room;

        memcpy(o->acc[o->cur] + (size_t)o->acc_frames * ch,
               pcm + (size_t)done * ch,
               (size_t)take * (size_t)ch * sizeof *pcm);
        o->acc_frames += take;
        done          += take;

        if (o->acc_frames == o->grain) {
            /* THIS is the blocking call - the device's pace, and the only place
             * this thread waits. A short write is not a thing here: the port
             * takes exactly one grain or it fails. */
            const int rc = sceAudioOutOutput(o->port, o->acc[o->cur]);
            /* Swap BEFORE returning to the caller: the buffer just submitted
             * belongs to the hardware until the next call comes back. */
            o->cur        ^= 1;
            o->acc_frames  = 0;
            if (rc < 0) return rc;
        }
    }
    return frames;
}

int audio_out_vita_queued(audio_out_vita *o)
{
    if (!o) return -1;
    const int rest = sceAudioOutGetRestSample(o->port);
    if (rest < 0) return -1;
    /* The partial grain is as much in front of the listener as the device's own
     * backlog: it is decided audio that has not been heard. Counting only the
     * device would under-report the queue by up to one grain, which is the same
     * class of blind spot L12 was written to close on ALSA. */
    return rest + o->acc_frames;
}

void audio_out_vita_close(audio_out_vita *o)
{
    if (!o) return;
    /* The remainder is DROPPED, not padded with silence: at most 10.67 ms, and
     * emitting a grain of half-silence at shutdown would be audible where
     * losing it is not. */
    if (o->port >= 0) sceAudioOutReleasePort(o->port);
    free(o->acc[0]);
    free(o->acc[1]);
    free(o);
}

#else
/* Not a Vita: this translation unit is empty on purpose, exactly like
 * audio_out_win.c off Windows. ISO C forbids an empty translation unit; a
 * `static const` put here to satisfy it is DEFINED and unused, which the other
 * console's build reports as a warning. A typedef declares no storage and
 * emits no symbol, so it satisfies the standard silently. */
typedef int audio_out_vita_vita_only;
#endif /* __vita__ */
