/* audio_out_win.c - the Windows audio sink (OUT-3, 2026-09-11). The contract
 * is in audio_out_win.h; this comment is about the choices.
 *
 * === WHY WASAPI, AND WHY EVENT-DRIVEN ===
 * The finding proposed waveOut and measured it feasible - but under
 * TIME_CRITICAL priority and timeBeginPeriod(1), two conditions the client
 * does not run under. Re-measured under the client's conditions on the dev
 * machine (i7-9750H, 8 s rows, the ring and producer of audio.c):
 *   - waveOut with 3 headers ran at x0.23 to x0.82 of real time, with 97 to
 *     582 ring overflows: the process timer (15.6 ms) cannot refill 10 ms
 *     headers in time;
 *   - timeBeginPeriod(1) rescues waveOut but changes the WHOLE process:
 *     WSAPoll(5) goes from 15.4 to 5.3 ms, which would silently move the
 *     Windows baselines of KB ING-1/ING-2. Nothing here calls it;
 *   - WASAPI shared mode, event-driven: 14 rows of 14 at x1.000 of real time,
 *     0 overflows, queue p50 10-30 ms idle and 40 ms under 12 busy threads, no
 *     process-wide setting touched. The engine signals an event once per
 *     device period (10 ms here): the playback thread's clock is the audio
 *     engine, not the process timer.
 *
 * FORMAT: we ask for what the stream is - 48 kHz, 16 bits, stereo - and let
 * the engine convert to the endpoint's mix format (AUTOCONVERTPCM with
 * SRC_DEFAULT_QUALITY) rather than converting ourselves. A 20 ms buffer: the
 * prototype's 20 and 30 ms rows measured alike, and 20 ms is the ALSA path's
 * measured minimum (L12) - two 10 ms write blocks.
 *
 * GUIDS: defined here under local names, so the link needs ole32 (and avrt,
 * for MMCSS) and nothing else - no uuid, no ksuser, and no initguid.h whose
 * definitions could collide with the ones libuuid already provides to the rest
 * of the link.
 *
 * MMCSS: the playback thread can register as a "Pro Audio" task
 * (audio_out_win_mmcss_begin). That raises THIS thread's scheduling class and
 * nothing else; why it is on by default is in media/audio.c
 * (SHADOW_WIN_AUDIO_MMCSS).
 *
 * Compiled on every platform by the CMake glob; only _WIN32 builds get code. */
#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out_win.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#  define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#  define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static const GUID k_clsid_mmdevice_enumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_iid_immdevice_enumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_iid_iaudio_client =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID k_iid_iaudio_render_client =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

struct audio_out_win {
    IMMDeviceEnumerator *en;
    IMMDevice           *dev;
    IAudioClient        *ac;
    IAudioRenderClient  *rc;
    HANDLE               ev;
    UINT32               buffer_frames;
    uint32_t             frame_bytes;
    bool                 started;
};

int audio_out_win_thread_begin(void)
{
    /* S_FALSE (already initialised in this apartment) must be balanced as
     * well; RPC_E_CHANGED_MODE (an STA is already there) must not. */
    return SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) ? 1 : 0;
}

void audio_out_win_thread_end(int began)
{
    if (began) CoUninitialize();
}

static uint32_t hns_to_frames(REFERENCE_TIME hns, uint32_t rate)
{
    return hns > 0 ? (uint32_t)(((uint64_t)hns * rate + 5000000u) / 10000000u) : 0;
}

audio_out_win *audio_out_win_open(uint32_t rate, uint32_t channels, uint32_t buffer_ms,
                                  const char *device_id,
                                  audio_out_win_info *info, long *hr_out)
{
    HRESULT hr = E_OUTOFMEMORY;
    if (info) memset(info, 0, sizeof(*info));
    audio_out_win *o = (audio_out_win *)calloc(1, sizeof(*o));
    if (!o) goto fail;

    hr = CoCreateInstance(&k_clsid_mmdevice_enumerator, NULL, CLSCTX_ALL,
                          &k_iid_immdevice_enumerator, (void **)&o->en);
    if (FAILED(hr)) goto fail;
    /* AUD-DEV1 - the chosen endpoint first, the default when it does not
     * resolve (see the header). */
    if (device_id && device_id[0]) {
        if (info) info->device_requested = true;
        wchar_t wid[512];
        if (MultiByteToWideChar(CP_UTF8, 0, device_id, -1, wid,
                                (int)(sizeof wid / sizeof wid[0])) > 0
            && SUCCEEDED(IMMDeviceEnumerator_GetDevice(o->en, wid, &o->dev))) {
            if (info) info->device_found = true;
        } else {
            o->dev = NULL;
        }
    }
    if (!o->dev) {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(o->en, eRender, eConsole, &o->dev);
        if (FAILED(hr)) goto fail;
    }
    hr = IMMDevice_Activate(o->dev, &k_iid_iaudio_client, CLSCTX_ALL, NULL, (void **)&o->ac);
    if (FAILED(hr)) goto fail;

    if (info) {
        WAVEFORMATEX *mix = NULL;
        if (SUCCEEDED(IAudioClient_GetMixFormat(o->ac, &mix)) && mix) {
            info->mix_rate     = (uint32_t)mix->nSamplesPerSec;
            info->mix_channels = (uint32_t)mix->nChannels;
            CoTaskMemFree(mix);
        }
    }

    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = (WORD)channels;
    wf.nSamplesPerSec  = rate;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = rate * channels * 2;
    hr = IAudioClient_Initialize(o->ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                 | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                 | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 (REFERENCE_TIME)buffer_ms * 10000, 0, &wf, NULL);
    if (FAILED(hr)) goto fail;

    o->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!o->ev) { hr = HRESULT_FROM_WIN32(GetLastError()); goto fail; }
    hr = IAudioClient_SetEventHandle(o->ac, o->ev);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetBufferSize(o->ac, &o->buffer_frames);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetService(o->ac, &k_iid_iaudio_render_client, (void **)&o->rc);
    if (FAILED(hr)) goto fail;
    o->frame_bytes = channels * (uint32_t)sizeof(int16_t);

    if (info) {
        REFERENCE_TIME per_def = 0, per_min = 0, lat = 0;
        IAudioClient_GetDevicePeriod(o->ac, &per_def, &per_min);
        IAudioClient_GetStreamLatency(o->ac, &lat);
        info->buffer_frames         = (uint32_t)o->buffer_frames;
        info->period_frames         = hns_to_frames(per_def, rate);
        info->min_period_frames     = hns_to_frames(per_min, rate);
        info->stream_latency_frames = hns_to_frames(lat, rate);
    }
    if (hr_out) *hr_out = (long)S_OK;
    return o;

fail:
    if (hr_out) *hr_out = (long)hr;
    audio_out_win_close(o);
    return NULL;
}

int audio_out_win_wait(audio_out_win *o, uint32_t timeout_ms)
{
    return WaitForSingleObject(o->ev, (DWORD)timeout_ms) == WAIT_OBJECT_0;
}

long audio_out_win_queued_frames(audio_out_win *o, uint32_t *frames)
{
    UINT32 pad = 0;
    const HRESULT hr = IAudioClient_GetCurrentPadding(o->ac, &pad);
    *frames = SUCCEEDED(hr) ? (uint32_t)pad : 0;
    return (long)hr;
}

long audio_out_win_room(audio_out_win *o, uint32_t *frames)
{
    UINT32 pad = 0;
    const HRESULT hr = IAudioClient_GetCurrentPadding(o->ac, &pad);
    *frames = (SUCCEEDED(hr) && pad < o->buffer_frames) ? (uint32_t)(o->buffer_frames - pad) : 0;
    return (long)hr;
}

long audio_out_win_write(audio_out_win *o, const int16_t *pcm, uint32_t frames)
{
    BYTE *dst = NULL;
    HRESULT hr = IAudioRenderClient_GetBuffer(o->rc, (UINT32)frames, &dst);
    if (FAILED(hr)) return (long)hr;
    memcpy(dst, pcm, (size_t)frames * o->frame_bytes);
    hr = IAudioRenderClient_ReleaseBuffer(o->rc, (UINT32)frames, 0);
    if (FAILED(hr)) return (long)hr;
    if (!o->started) {
        /* The first write starts the stream; from here on the engine's
         * period event paces the caller. */
        hr = IAudioClient_Start(o->ac);
        if (FAILED(hr)) return (long)hr;
        o->started = true;
    }
    return (long)S_OK;
}

void audio_out_win_close(audio_out_win *o)
{
    if (!o) return;
    if (o->ac && o->started) IAudioClient_Stop(o->ac);
    if (o->rc)  IAudioRenderClient_Release(o->rc);
    if (o->ac)  IAudioClient_Release(o->ac);
    if (o->dev) IMMDevice_Release(o->dev);
    if (o->en)  IMMDeviceEnumerator_Release(o->en);
    if (o->ev)  CloseHandle(o->ev);
    free(o);
}

int audio_out_win_lost(long hr)
{
    return hr == (long)AUDCLNT_E_DEVICE_INVALIDATED
        || hr == (long)AUDCLNT_E_SERVICE_NOT_RUNNING;
}

void *audio_out_win_mmcss_begin(void)
{
    DWORD task_index = 0;
    return (void *)AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
}

void audio_out_win_mmcss_end(void *token)
{
    if (token) AvRevertMmThreadCharacteristics((HANDLE)token);
}

#else
/* ISO C wants at least one declaration in a translation unit. */
typedef int audio_out_win_windows_only;
#endif
