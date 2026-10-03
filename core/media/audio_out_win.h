/* audio_out_win.h - the Windows audio sink behind media/audio.c (OUT-3,
 * 2026-09-11): WASAPI, shared mode, event-driven.
 *
 * A deliberately small surface, so audio.c never sees <windows.h> or COM:
 * open, room, write, queued_frames, close - plus the two calls the playback
 * loop needs around them (wait for the engine's period event; bracket the
 * thread with COM). Every call that can fail returns the HRESULT as a `long`,
 * 0 or positive on success and negative on failure, so audio.c can log it
 * without knowing the type.
 *
 * THREADING: an audio_out_win belongs to the thread that opened it, and every
 * call on it happens there - audio.c's playback thread, between
 * audio_out_win_thread_begin() and audio_out_win_thread_end(). No call here
 * is safe from another thread, close included.
 *
 * Compiled on every platform (the CMake glob takes every .c under core/ and clients/);
 * only a Windows build gets any code. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_out_win audio_out_win;

/* What the engine actually GRANTED - it rounds the request - so the caller
 * can log what it got rather than what it asked for (the L12b rule). In
 * frames at the requested rate. */
typedef struct {
    uint32_t buffer_frames;          /* the engine buffer; room() is measured against it */
    uint32_t period_frames;          /* default device period: the event's cadence */
    uint32_t min_period_frames;
    uint32_t stream_latency_frames;  /* IAudioClient::GetStreamLatency */
    uint32_t mix_rate;               /* the endpoint's mix format; the engine converts to it */
    uint32_t mix_channels;
    /* AUD-DEV1: a specific endpoint was asked for (`device_id` non-empty) and
     * whether it was the one opened. Asked and not found = the default was
     * opened instead, which the caller must SAY - a sound coming out of the
     * wrong speakers with nothing in the log is the defect this prevents. */
    bool     device_requested;
    bool     device_found;
} audio_out_win_info;

/* COM on the calling thread, multithreaded apartment. Returns 1 when
 * audio_out_win_thread_end() must undo it (the call succeeded), else 0. */
int  audio_out_win_thread_begin(void);
void audio_out_win_thread_end(int began);

/* === AUD-DEV1 2026-10-03 - CHOOSING THE OUTPUT DEVICE ======================
 *
 * `device_id` is an endpoint ID as `IMMDevice::GetId` writes it, in UTF-8
 * (`{0.0.0.00000000}.{guid}`) - the same string Qt's `QAudioDevice::id()`
 * carries on Windows, which is how the desktop client's settings page fills
 * it. NULL or "" = the default endpoint, exactly as before.
 *
 * An ID that no longer resolves (the headset is unplugged, the driver was
 * reinstalled and minted a new GUID) falls back to the DEFAULT rather than
 * failing: losing the chosen device must cost the choice, not the sound.
 * `info->device_found` says which happened.
 *
 * Opens the chosen render endpoint (or the default, eConsole) in shared mode, `rate` Hz,
 * `channels` x s16 interleaved, with an engine buffer of `buffer_ms`. The
 * stream is NOT started: the first successful write starts it, so the engine
 * never plays an empty buffer. NULL on failure, with the failing HRESULT in
 * *hr. It took 83 ms to 1.2 s on the dev machine: never call it on a thread
 * that something else waits for. */
audio_out_win *audio_out_win_open(uint32_t rate, uint32_t channels, uint32_t buffer_ms,
                                  const char *device_id,
                                  audio_out_win_info *info, long *hr);

/* Waits for the engine's period event, at most `timeout_ms`: 1 when it fired,
 * 0 otherwise. Before the first write the engine sends no event. */
int  audio_out_win_wait(audio_out_win *o, uint32_t timeout_ms);

/* Frames that can be written now: the buffer minus what is still queued. */
long audio_out_win_room(audio_out_win *o, uint32_t *frames);

/* Frames the engine still holds, not played yet (GetCurrentPadding). */
long audio_out_win_queued_frames(audio_out_win *o, uint32_t *frames);

/* Copies `frames` interleaved frames into the engine buffer - never more than
 * room() reported. Starts the stream on the first success. */
long audio_out_win_write(audio_out_win *o, const int16_t *pcm, uint32_t frames);

/* Stops the stream and releases everything. NULL is a no-op. */
void audio_out_win_close(audio_out_win *o);

/* 1 when `hr` means the endpoint went away - device removed, disabled or
 * reconfigured, or the audio service stopped: close it, and a reopen may
 * succeed. Any other failure is not expected to go away by reopening. */
int  audio_out_win_lost(long hr);

/* MMCSS: registers the CALLING thread as a "Pro Audio" task, the scheduling
 * class Windows gives audio threads. Per thread - nothing process-wide, unlike
 * timeBeginPeriod. Returns a token for audio_out_win_mmcss_end(), or NULL when
 * the service refuses. */
void *audio_out_win_mmcss_begin(void);
void  audio_out_win_mmcss_end(void *token);

#ifdef __cplusplus
}
#endif
