/* ui::sfx - see sfx.hpp for the why, and in particular for the constraint that
 * dictates the whole design: the console has only one audio output, and during a
 * session the stream owns it.
 */
#include "sfx.hpp"

#include <borealis/core/assets.hpp>

extern "C" {
#include "wav.h"
#include "../../../core/services/journal.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <time.h>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#  include <switch.h>
#  define SFX_AUDOUT 1
#  define SFX_ALSA   0
#elif defined(_WIN32)
#  define SFX_AUDOUT 0
#  define SFX_ALSA   0
#elif defined(__vita__) || defined(__psp2__)
/* PS Vita: its own SceAudioOut port, and that is the whole difference from the
 * Switch. AUDC-1 was an entire campaign spent handing ONE audout back and forth
 * between the stream and these sounds, because HOS grants a single output. The
 * Vita has several ports, so sfx takes BGM and the stream keeps MAIN - no
 * claim, no hand-over, no silence after a reconnect.
 *
 * Reported silent from a real console on 2026-09-13; it was silent BY DESIGN in
 * the first port, which is not the same as broken, but is not the answer
 * either. NOT VALIDATED ON HARDWARE either way. */
#  define SFX_AUDOUT   0
#  define SFX_ALSA     0
#  define SFX_SCEAUDIO 1
#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#  include <alsa/asoundlib.h>
#  define SFX_AUDOUT 0
#  define SFX_ALSA   1
#else
/* A target this ladder does not know: no sound, and it still builds. */
#  define SFX_AUDOUT 0
#  define SFX_ALSA   0
#endif

#ifndef SFX_SCEAUDIO
#  define SFX_SCEAUDIO 0
#endif

#define slog(...) JOURNAL_INFO_(JOURNAL_CAT_UI, __VA_ARGS__)
#define sdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_UI, __VA_ARGS__)
#define swarn(...) JOURNAL_WARN_(JOURNAL_CAT_UI, __VA_ARGS__)

namespace ui {
namespace sfx {

namespace {

/* Output at 48 kHz stereo: the NATIVE rate of `audout` on console. Anything else
 * would force a resample on every sound, for a worse result - and it is also the
 * rate the source files are specified at (see docs/UI_SOUNDS.md), so in
 * practice no conversion happens at all. */
const uint32_t OUT_HZ   = 48000;
const int      CHANNELS = 2;

/* Size of one block written at a time. 480 frames = 10 ms: short enough that
 * closing the device responds quickly, long enough not to wake the thread a
 * hundred times a second. */
const size_t   BLOCK_FRAMES = 480;

/* How long the output stays open after the last sound. Two seconds: enough to
 * chain a run of navigation without reopening at every step, little enough that
 * the device is free by the time a session starts. */
const double   IDLE_S = 2.0;

/* A loaded sound. Samples are converted ONCE, at load time, to interleaved
 * stereo at the output rate: the mixer then only has to add, which is exactly
 * what you want inside an audio thread. */
struct Sample {
    std::vector<int16_t> pcm;   /* interleaved stereo */
    size_t frames = 0;
};

/* A voice in flight. Four at most: in practice two sounds overlap (confirm +
 * open), and a fixed ceiling keeps a burst of navigation from growing the list -
 * which would clip before it ever reached anyone's ears.
 *
 * === S89 2026-08-29 - THE VARIATION HAPPENS AT PLAYBACK ===
 *
 * Three navigation variants cut down the repetition; they do not remove it. A
 * held direction replays the same cycle of three every 375 ms, and the ear hears
 * a pattern - which is worse than a single sound, because a pattern gets
 * listened to whereas a noise gets forgotten.
 *
 * So we draw a pitch and a gain from a narrow range ON EVERY PLAYBACK. This is
 * the ordinary trick of game audio, and it has two advantages over multiplying
 * the files: it costs zero bytes, and it covers the sounds we only have one copy
 * of - the end-of-list bump in particular, which fires in bursts when you hold
 * the direction at the bottom of a page.
 *
 * The read position therefore becomes FRACTIONAL, and playback interpolates. */
const int VOICE_MAX = 4;
struct Voice {
    const Sample *sample = nullptr;
    double position = 0.0;   /* in frames, fractional (S89) */
    float  step     = 1.0f;  /* > 1 = higher pitched and shorter */
    float  gain     = 1.0f;
};

/* Amplitude of the variation, per sound.
 *
 * IT IS ZERO ON THE EVENT SOUNDS, and that is the point: a signature that
 * changes pitch every time no longer reads as a signature, it reads as a defect.
 * `connecte` marks the end of seven steps of waiting, `demarrage` identifies the
 * application - those must be IDENTICAL every time you hear them.
 *
 * Conversely, the more frequent a sound is, the more it may vary: repetition is
 * what makes the variation necessary, and it is also what makes it
 * imperceptible. 4 % of pitch is less than a semitone - you do not hear it as a
 * different note, only as "not exactly the same". */
struct Variation { float pitch; float gain_db; };
const Variation VARIATION[(size_t)Sound::Count] = {
    { 0.040f, 1.5f },   /* Navigation     - up to 8 times a second */
    { 0.035f, 1.2f },   /* NavigationEdge - fires in bursts on a held end-of-list */
    { 0.030f, 1.0f },   /* Value */
    { 0.015f, 0.7f },   /* Section */
    { 0.012f, 0.6f },   /* Confirm */
    { 0.012f, 0.6f },   /* Back */
    { 0.015f, 0.7f },   /* ToggleOn */
    { 0.015f, 0.7f },   /* ToggleOff */
    { 0.020f, 0.8f },   /* Open */
    { 0.020f, 0.8f },   /* Close */
    { 0.000f, 0.0f },   /* Connected - signature: never */
    { 0.000f, 0.0f },   /* Failed    - same */
    { 0.010f, 0.5f },   /* Alert     - can repeat, so a breath of variation */
    { 0.000f, 0.0f },   /* Startup   - signature: never */
};

/* xorshift generator. It lives HERE, at file scope, and it is seeded once from
 * the clock: an identical sequence on every launch would give exactly the same
 * pattern from one session to the next, which is precisely what we are trying to
 * break. */
uint32_t g_rand = 0;

float rand_signed()   /* [-1, 1] */
{
    if (g_rand == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        g_rand = (uint32_t)(ts.tv_nsec ^ (ts.tv_sec << 11)) | 1u;
    }
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 17;
    g_rand ^= g_rand << 5;
    return (float)((double)g_rand / 2147483648.0 - 1.0);
}

/* --- Module state. It lives HERE, at file scope, and not in function-local
 * `static`s: this repo has paid for four outages caused by storing state that
 * outlives a call inside a function body. --- */
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
Sample          g_sounds[(size_t)Sound::Count];
Voice           g_voices[VOICE_MAX];
bool            g_loaded  = false;
bool            g_enabled = true;
uint32_t        g_volume  = 70;      /* percent */
volatile bool   g_stop    = false;
bool            g_thread_started = false;
pthread_t       g_thread;
/* Timestamp of the last sound requested, in monotonic seconds. The thread uses
 * it to decide when to close the output. */
double          g_last    = 0.0;
/* True while a buffer is still to be played: the thread does not close in the
 * middle of a sound. */
bool            g_playing = false;
/* === AUDC-1 / OUT-2 2026-09-11 - THE STREAM'S CLAIM ON THE OUTPUT ===
 * `g_suspended`: while set, the stream owns the output - play() does nothing,
 * and the thread closes the output at once and does not reopen it.
 * `g_suspend_ack`: set by the THREAD, under the lock, once it has SEEN the flag
 * and closed; never by the caller. Waiting for "the device is closed" instead
 * races an iteration that has already read the flag as clear and is inside
 * out_open(): the verifier's harness saw that variant return with the UI output
 * still started in 90-110 of 300 claims; the acknowledgement, 0 of 900 there
 * and 0 of 3000 in the AUDC-1 bench.
 * File scope like the rest of the module's state, and cleared by the claim's
 * destructor on every exit path: a flag left set would mute the interface for
 * the rest of the process. */
bool            g_suspended   = false;
bool            g_suspend_ack = false;
/* AUDC-1 - SHADOW_SFX_HANDOFF, read once (-1 = not read yet). 0 restores the
 * previous behaviour exactly: no claim, and release() cuts the voices. */
int             g_handoff     = -1;

/* Assumes `g_lock` is held. init() reads the variable; the other entry points
 * fall back on the same single read if init() was never called (sfx.hpp says
 * forgetting it is allowed). */
bool handoff_locked()
{
    if (g_handoff < 0) {
        const char *e = std::getenv("SHADOW_SFX_HANDOFF");
        g_handoff = (e && std::atoi(e) == 0) ? 0 : 1;
    }
    return g_handoff != 0;
}

double now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* --- Loading ---------------------------------------------------------- */

const char *NAME[(size_t)Sound::Count] = {
    "nav_1", "nav_bord", "valeur", "rubrique",
    "valider", "retour", "bascule_on", "bascule_off",
    "ouvrir", "fermer",
    "connecte", "echec", "alerte", "demarrage",
};

/* The navigation variants. They are NOT decoration: a held direction fires this
 * sound eight times a second, and the same sample repeated identically sounds
 * like a machine gun - a well known effect, and one that makes a list unpleasant
 * to walk through. A missing file is simply skipped; if only one is left, it
 * plays on its own. */
const char *NAV_VARIANTS[] = { "nav_1", "nav_2", "nav_3" };
std::vector<Sample> g_nav;
size_t g_nav_next = 0;

/* Reads a whole file. Returns an empty vector if it is missing or implausible.
 *
 * The cap is not a comfort precaution: the path is built from names we write,
 * but the FILE is dropped in by a human onto an SD card, and its size would
 * otherwise dictate an allocation. An interface sound larger than one megabyte
 * does not exist - at 48 kHz stereo that is five seconds. */
std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> v;
    FILE *f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0 && size <= 1024L * 1024L) {
        v.resize((size_t)size);
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
}

/* Converts once and for all to interleaved stereo at `OUT_HZ`.
 *
 * The resampling is the SIMPLEST possible - nearest neighbour - and that is a
 * choice, not a shortcut: the files are specified at 48 kHz, so this path is
 * only ever taken if someone drops in an off-spec file. In that case a slightly
 * rough sound beats a silent one, and it is not worth shipping a filter for a
 * situation that should not happen. */
bool convert(const std::vector<uint8_t> &raw, Sample &out)
{
    wav_info_t w;
    if (!wav_analyser(raw.data(), raw.size(), &w)) return false;

    const int16_t *src = (const int16_t *)(raw.data() + w.offset);
    const size_t src_frames = w.frames;

    size_t dst_frames = src_frames;
    if (w.frequency != OUT_HZ) {
        dst_frames = (size_t)((double)src_frames * (double)OUT_HZ / (double)w.frequency);
        if (dst_frames == 0) return false;
    }
    /* Sanity cap AFTER conversion: an 8 kHz file stretches by six, and it is the
     * final size that decides the allocation. */
    if (dst_frames > OUT_HZ * 5u) return false;

    out.pcm.assign(dst_frames * (size_t)CHANNELS, 0);
    out.frames = dst_frames;

    for (size_t i = 0; i < dst_frames; i++) {
        const size_t j = (w.frequency == OUT_HZ)
                       ? i
                       : (size_t)((double)i * (double)w.frequency / (double)OUT_HZ);
        const size_t js = (j < src_frames) ? j : src_frames - 1;
        if (w.channels == 1) {
            const int16_t m = src[js];
            out.pcm[i * 2]     = m;
            out.pcm[i * 2 + 1] = m;
        } else {
            out.pcm[i * 2]     = src[js * 2];
            out.pcm[i * 2 + 1] = src[js * 2 + 1];
        }
    }
    return true;
}

bool load_one(const char *name, Sample &out)
{
    const std::string path = std::string(BRLS_RESOURCES) + "sfx/" + name + ".wav";
    const std::vector<uint8_t> raw = read_file(path.c_str());
    if (raw.empty()) return false;
    if (!convert(raw, out)) {
        /* A file that is PRESENT but refused is information: that is a
         * production mistake, not an absence, and silence alone would not say
         * so. */
        slog("[S88] sound '%s' present but unreadable (expected format: "
             "WAV PCM 16 bits mono/stereo)", name);
        return false;
    }
    return true;
}

/* Assumes `g_lock` is held. */
void load_all_locked()
{
    if (g_loaded) return;
    g_loaded = true;

    int found = 0;
    for (size_t i = 0; i < (size_t)Sound::Count; i++)
        if (load_one(NAME[i], g_sounds[i])) found++;

    g_nav.clear();
    for (const char *n : NAV_VARIANTS) {
        Sample e;
        if (load_one(n, e)) g_nav.push_back(std::move(e));
    }

    /* %u, not %zu: MinGW checks this format as msvcrt's, which has no 'z' - a
     * -Werror failure once tests/test_sfx_handoff.cpp compiles this file
     * (AUDC-1, 2026-09-11). The text written is the same. */
    slog("[S88] interface sounds: %d/%d loaded, %u navigation variant(s)",
         found, (int)Sound::Count, (unsigned)g_nav.size());
}

/* --- Mixing ----------------------------------------------------------- */

/* Fills `dst` (BLOCK_FRAMES * CHANNELS) with the sum of the active voices.
 * Returns true if there is still something left to play.
 *
 * Assumes `g_lock` is held - the call is short and purely arithmetic, never I/O:
 * the mixing lock must not be the same one as the write to the device, which is
 * the lesson of `feedback_wss_mutex_deadlock`. */
bool mix_locked(int16_t *dst)
{
    std::memset(dst, 0, BLOCK_FRAMES * CHANNELS * sizeof(int16_t));
    if (!g_enabled || g_volume == 0) return false;

    bool remaining = false;
    for (int v = 0; v < VOICE_MAX; v++) {
        Voice &x = g_voices[v];
        if (!x.sample) continue;

        const int16_t *src = x.sample->pcm.data();
        const double   end = (double)x.sample->frames - 1.0;

        for (size_t t = 0; t < BLOCK_FRAMES; t++) {
            if (x.position > end) { x.sample = nullptr; break; }

            /* === S89 - FRACTIONAL-STEP PLAYBACK ===
             * The step carries the pitch: 1.04 plays 4 % faster, so 4 % higher
             * and 4 % shorter. On a sixty-millisecond tick, a shift in speed is
             * indistinguishable from a shift in pitch - and the duration that
             * comes with it is a bonus, since two repeats that also differ in
             * length blend together even less.
             *
             * Linear interpolation is plenty: the aliasing it produces sits
             * thirty decibels under the signal, on a percussive sound of less
             * than a second. A more careful filter would be wasted work here. */
            const size_t j  = (size_t)x.position;
            const size_t j2 = (j + 1 <= (size_t)end) ? j + 1 : j;
            const float  f  = (float)(x.position - (double)j);

            for (int c = 0; c < CHANNELS; c++) {
                const float a = (float)src[j  * CHANNELS + c];
                const float b = (float)src[j2 * CHANNELS + c];
                const float e = (a + (b - a) * f) * x.gain;

                /* Sum in 32 bits THEN clamp. Adding directly in 16 bits would
                 * wrap through zero on overflow - a click, exactly when two
                 * sounds overlap, so at the most audible moment. */
                int32_t s = (int32_t)dst[t * CHANNELS + c] + (int32_t)e;
                if (s >  32767) s =  32767;
                if (s < -32768) s = -32768;
                dst[t * CHANNELS + c] = (int16_t)s;
            }
            x.position += (double)x.step;
        }

        if (x.sample) remaining = true;
    }

    if (g_volume != 100) {
        for (size_t i = 0; i < BLOCK_FRAMES * CHANNELS; i++)
            dst[i] = (int16_t)((int32_t)dst[i] * (int32_t)g_volume / 100);
    }
    return remaining;
}

/* --- Output, per platform --------------------------------------------- */

#if SFX_ALSA
snd_pcm_t *g_pcm = nullptr;

bool out_open()
{
    if (g_pcm) return true;
    if (snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        g_pcm = nullptr;
        return false;
    }
    unsigned int hz = OUT_HZ;
    if (snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED, CHANNELS, hz, 1,
                           100000 /* 100 ms of latency: this is a short sound */) < 0) {
        snd_pcm_close(g_pcm);
        g_pcm = nullptr;
        return false;
    }
    return true;
}

void out_close()
{
    if (!g_pcm) return;
    snd_pcm_drain(g_pcm);
    snd_pcm_close(g_pcm);
    g_pcm = nullptr;
}

void out_write(const int16_t *pcm)
{
    if (!g_pcm) return;
    const snd_pcm_sframes_t w = snd_pcm_writei(g_pcm, pcm, BLOCK_FRAMES);
    if (w < 0) snd_pcm_recover(g_pcm, (int)w, 1);
}

#elif SFX_SCEAUDIO
/* PS Vita, on the BGM port. The device wants a grain that is a multiple of 64
 * and this file writes 480 frames, so it goes through the SAME wrapper the
 * stream uses (`core/media/audio_out_vita.c`), which absorbs that mismatch
 * once instead of twice. */
extern "C" {
#include "../../../core/media/audio_out_vita.h"
}

audio_out_vita *g_vita_sfx = nullptr;

bool out_open()
{
    if (g_vita_sfx) return true;
    audio_out_vita_info inf;
    g_vita_sfx = audio_out_vita_open(AUDIO_OUT_VITA_BGM, (int)OUT_HZ, CHANNELS, &inf);
    if (!g_vita_sfx) {
        slog("sfx: SceAudioOut BGM unavailable - the interface stays silent");
        return false;
    }
    slog("sfx: SceAudioOut BGM opened - %d Hz, %d ch, grain %d frames (%.2f ms)",
         inf.rate, inf.channels, inf.grain, inf.grain_us / 1000.0);
    return true;
}

void out_close()
{
    if (!g_vita_sfx) return;
    audio_out_vita_close(g_vita_sfx);
    g_vita_sfx = nullptr;
}

void out_write(const int16_t *pcm)
{
    if (!g_vita_sfx) return;
    /* Blocking, like ALSA's write: this runs on the sfx thread, whose lateness
     * penalises nobody else. A negative return means the port is gone; there is
     * nothing to recover, and the next `out_open` will say so. */
    audio_out_vita_write(g_vita_sfx, pcm, (int)BLOCK_FRAMES);
}

#elif SFX_AUDOUT
/* `audout` requires buffers aligned on 0x1000 AND whose size is a multiple of
 * 0x1000. We keep four of them in rotation, like the stream's decoder does. */
const int NUM_BUFFERS = 4;
alignas(0x1000) int16_t g_buffer[NUM_BUFFERS][BLOCK_FRAMES * CHANNELS];
AudioOutBuffer g_aob[NUM_BUFFERS];
int  g_buffer_next = 0;
bool g_audout = false;

bool out_open()
{
    if (g_audout) return true;
    if (R_FAILED(audoutInitialize())) return false;
    if (R_FAILED(audoutStartAudioOut())) { audoutExit(); return false; }
    g_audout = true;
    g_buffer_next = 0;
    return true;
}

void out_close()
{
    if (!g_audout) return;
    audoutStopAudioOut();
    audoutExit();
    g_audout = false;
}

void out_write(const int16_t *pcm)
{
    if (!g_audout) return;
    const size_t raw = BLOCK_FRAMES * CHANNELS * sizeof(int16_t);
    /* Round UP to a multiple of 0x1000, as the service demands. The padding is
     * already zero since we only write `raw` bytes into a static buffer: it
     * plays silence, not uninitialised memory. */
    const size_t aligned = (raw + 0xFFFu) & ~(size_t)0xFFF;

    int16_t *dst = g_buffer[g_buffer_next];
    AudioOutBuffer *aob = &g_aob[g_buffer_next];
    g_buffer_next = (g_buffer_next + 1) % NUM_BUFFERS;

    std::memcpy(dst, pcm, raw);
    aob->next = nullptr;
    aob->buffer = dst;
    aob->buffer_size = aligned;
    aob->data_size = raw;
    aob->data_offset = 0;

    audoutAppendAudioOutBuffer(aob);
    AudioOutBuffer *released = nullptr;
    u32 count = 0;
    /* Short and BOUNDED wait: this thread has to stay able to notice `g_stop`
     * within 100 ms, otherwise HOS kills it brutally on exit and leaks its
     * handles - and a leak costs a reboot of the console. */
    audoutWaitPlayFinish(&released, &count, 50000000ULL);
}

#else  /* neither ALSA nor audout: Windows */
bool out_open()  { return false; }
void out_close() { }
void out_write(const int16_t *) { }
#endif

/* --- The thread ------------------------------------------------------- */

void *thread_loop(void *)
{
    int16_t block[BLOCK_FRAMES * CHANNELS];
    bool opened = false;

    while (!g_stop) {
        pthread_mutex_lock(&g_lock);
        /* AUDC-1 - the claim is read under the same lock as the mix: an
         * iteration that mixed is one that saw no claim. */
        const bool suspended = g_suspended;
        const bool something = suspended ? false : mix_locked(block);
        g_playing = something;
        const double last = g_last;
        pthread_mutex_unlock(&g_lock);

        if (suspended) {
            /* AUDC-1 - the stream owns the output: close it now, never reopen
             * it, and only then acknowledge - even when nothing was open (no
             * device on Windows, no sound since the last close), so the claim
             * never waits for nothing. */
            if (opened) {
                out_close();
                opened = false;
            }
            pthread_mutex_lock(&g_lock);
            if (g_suspended) g_suspend_ack = true;
            pthread_mutex_unlock(&g_lock);
            /* 10 ms: the claim polls at that rate, and `g_stop` is still seen
             * well inside the 100 ms HOS demands. */
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, nullptr);
            continue;
        }

        if (something) {
            if (!opened) opened = out_open();
            /* The write happens OUTSIDE the lock: it blocks while the device
             * consumes the block, and holding the lock for that long would make
             * `play()` wait, hence the interface.
             * (`feedback_wss_mutex_deadlock`: never hold an application lock
             * across blocking I/O.) */
            if (opened) out_write(block);
            else {
                /* No device: consume the sound at real time anyway rather than
                 * spinning. Without this pause, a desktop with no audio server
                 * would run this thread flat out. */
                struct timespec ts = { 0, 10 * 1000 * 1000 };
                nanosleep(&ts, nullptr);
            }
        } else {
            if (opened && now() - last > IDLE_S) {
                out_close();
                opened = false;
            }
            /* 20 ms when idle: the thread notices `g_stop` well inside the
             * 100 ms HOS demands, and costs nothing. */
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, nullptr);
        }
    }

    if (opened) out_close();
    return nullptr;
}

/* Assumes `g_lock` is held. */
void start_thread_locked()
{
    if (g_thread_started) return;
    g_stop = false;
    if (pthread_create(&g_thread, nullptr, thread_loop, nullptr) == 0) g_thread_started = true;
}

}  // namespace

/* --- Public interface ------------------------------------------------- */

void init()
{
    pthread_mutex_lock(&g_lock);
    const bool handoff = handoff_locked();   /* AUDC-1: SHADOW_SFX_HANDOFF, read once */
    load_all_locked();
    pthread_mutex_unlock(&g_lock);
    /* Said once per process: the console A/B reads which arm ran from the log,
     * not from what env.txt was believed to hold. */
    if (!handoff)
        slog("[AUDC1] handing the audio output to the stream is disabled (SHADOW_SFX_HANDOFF=0)");
}

void play(Sound s)
{
    if ((int)s < 0 || s >= Sound::Count) return;

    pthread_mutex_lock(&g_lock);
    /* AUDC-1 - `g_suspended`: the stream owns the output (sfx.hpp promised this
     * silence long before any code enforced it). */
    if (!g_enabled || g_volume == 0 || g_suspended) { pthread_mutex_unlock(&g_lock); return; }
    load_all_locked();

    /* Navigation cycles through its variants. The counter advances even when the
     * voice does not land: two quick presses must still change variant,
     * otherwise the voice ceiling would bring back the very repetition we are
     * trying to avoid. */
    const Sample *e = nullptr;
    if (s == Sound::Navigation && !g_nav.empty()) {
        e = &g_nav[g_nav_next % g_nav.size()];
        g_nav_next++;
    } else {
        e = &g_sounds[(size_t)s];
    }
    if (!e || e->frames == 0) { pthread_mutex_unlock(&g_lock); return; }

    /* We take the first free voice. None is ever STOLEN: cutting a sound short
     * to make room for another is heard as a click, and a burst of navigation
     * would produce one at every step. Past the ceiling the sound is simply
     * dropped - which, at four voices, is inaudible. */
    /* S89 - pitch and gain are drawn NOW, once per playback, and not per block:
     * a pitch that moved during the sound would produce vibrato, not
     * variation. */
    const Variation var = VARIATION[(size_t)s];
    const float step = 1.0f + var.pitch * rand_signed();
    const float gain = (var.gain_db > 0.0f)
                     ? powf(10.0f, (var.gain_db * rand_signed()) / 20.0f)
                     : 1.0f;

    for (int v = 0; v < VOICE_MAX; v++) {
        if (g_voices[v].sample) continue;
        g_voices[v].sample = e;
        g_voices[v].position = 0.0;
        g_voices[v].step = step;
        g_voices[v].gain = gain;
        break;
    }
    g_last = now();
    start_thread_locked();
    pthread_mutex_unlock(&g_lock);
}

void setEnabled(bool on)
{
    pthread_mutex_lock(&g_lock);
    g_enabled = on;
    if (!on) for (int v = 0; v < VOICE_MAX; v++) g_voices[v].sample = nullptr;
    pthread_mutex_unlock(&g_lock);
}

void setVolume(uint32_t percent)
{
    pthread_mutex_lock(&g_lock);
    g_volume = percent > 100 ? 100 : percent;
    pthread_mutex_unlock(&g_lock);
}

bool enabled()
{
    pthread_mutex_lock(&g_lock);
    const bool a = g_enabled;
    pthread_mutex_unlock(&g_lock);
    return a;
}

void release()
{
    /* We push the idle timestamp into the past: the thread then closes the
     * output on its first pass with nothing left to mix.
     *
     * Closing it HERE would be more direct and WRONG: the device is opened by
     * the audio thread, and closing it from another thread while that one is
     * writing into it is exactly the race we avoid everywhere else.
     *
     * === AUDC-1 / OUT-2 2026-09-11 - THE VOICES ARE NO LONGER CUT ===
     * This call runs microseconds after play(Connected) - connecting_activity.cpp
     * puts only a queued Threading::sync between the two - and it used to clear
     * every voice: the chime never got out. 0 of its 34 464 frames in the AUDC-1
     * bench, 0 of 72 blocks in 12/12 real-time runs of the OUT-2 harness, ever
     * since S88. Now the sound in flight plays to its end and the thread closes
     * right after it; one buffer is in flight at a time, so that close cuts
     * nothing: 34 080 / 34 464 frames, bit-exact (the last partial 10 ms block
     * is dropped by mix_locked(), as for every sound). Handing the output to
     * the stream is suspend_for_stream()'s job, a second later. */
    pthread_mutex_lock(&g_lock);
    if (!handoff_locked())                   /* SHADOW_SFX_HANDOFF=0: the old cut */
        for (int v = 0; v < VOICE_MAX; v++) g_voices[v].sample = nullptr;
    g_last = now() - IDLE_S * 2.0;
    pthread_mutex_unlock(&g_lock);
}

/* === AUDC-1 / OUT-2 2026-09-11 - THE HANDOFF ===
 * libnx has ONE IAudioOut per process: we and media/audio.c call
 * audoutStartAudioOut() on the same output, with no handle. A reconnection
 * plays Failed, then opens the stream one second later - while this thread,
 * which closes only after IDLE_S, still holds the output started. HOS then
 * refuses the stream's Start (no sound for the whole session) or accepts it and
 * our idle close stops the shared output a second in: silent either way.
 * Offline bench (this file and audio.c built for __SWITCH__ against a mock of
 * the one IAudioOut, both models of a second Start, 430 runs): the reconnected
 * session heard 0.0 % (refused) or 3-33 % (accepted) at X <= 0.9 s before,
 * 100 % bit-exact at every X after, six materials including FLAC; UI sounds
 * over a live session: 48 refused Starts, or the stream stopped, before - 0
 * IPC after. A lazy Start retry inside audio.c (the finding's part 2) was
 * measured and not kept: useless when HOS accepts, a second late when it
 * refuses. SHADOW_SFX_HANDOFF=0 turns both functions into no-ops. */
bool suspend_for_stream()
{
    pthread_mutex_lock(&g_lock);
    if (!handoff_locked()) {                 /* SHADOW_SFX_HANDOFF=0: no claim */
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    g_suspended   = true;
    g_suspend_ack = false;
    /* Whatever still plays is cut, on purpose: the session is about to open the
     * output. The Connected chime is not concerned - release() lets it end,
     * and the session starts a second after it. */
    for (int v = 0; v < VOICE_MAX; v++) g_voices[v].sample = nullptr;
    const bool thread = g_thread_started;
    pthread_mutex_unlock(&g_lock);
    if (!thread) {                           /* no thread: nothing was ever opened */
        slog("[AUDC1] interface sounds suspended during the stream (no output open)");
        return true;
    }

    /* Wait for the THREAD's acknowledgement, in 10 ms slices, 100 ms at most
     * (KB §7.3 - and this runs on the connecting worker, never the UI thread).
     * Bench: median 10 ms, p99 20 ms, worst 69 ms over 3000 claims raced
     * against an open in progress. Measured in real time, not in slices: a
     * sleep rounds up to 15.6 ms on Windows. */
    const double t0 = now();
    for (;;) {
        pthread_mutex_lock(&g_lock);
        const bool done = g_suspend_ack;
        pthread_mutex_unlock(&g_lock);
        const int ms = (int)((now() - t0) * 1000.0 + 0.5);
        if (done) {
            slog("[AUDC1] interface sounds suspended during the stream (output handed back in %d ms)", ms);
            return true;
        }
        if (ms >= 100) break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }
    /* The claim stays in force - the thread closes as soon as it sees it - and
     * the session goes ahead: making it wait longer would cost more than a UI
     * sound could. */
    swarn("[AUDC1] interface sounds: output not handed back within 100 ms");
    return false;
}

void resume_after_stream()
{
    pthread_mutex_lock(&g_lock);
    g_suspended   = false;
    g_suspend_ack = false;
    pthread_mutex_unlock(&g_lock);
}

void close()
{
    if (g_thread_started) {
        g_stop = true;
        pthread_join(g_thread, nullptr);   /* clean exit before HOS kills it */
        g_thread_started = false;
    }
    pthread_mutex_lock(&g_lock);
    for (int v = 0; v < VOICE_MAX; v++) g_voices[v].sample = nullptr;
    pthread_mutex_unlock(&g_lock);
}

}  // namespace sfx
}  // namespace ui
