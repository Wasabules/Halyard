// Audio decode + Switch AudioOut. Cf. audio.h.
//
// Opus -> stereo s16 48 kHz -> audout buffers (4 buffers in rotation).
//
// Buffer size: 19200 bytes per buffer (= 1920 stereo s16 samples = 20 ms at
// 48 kHz, the typical size of a WebRTC Opus packet). Aligned on 0x1000 (4 KB, as
// audout requires).

#include "audio.h"
#include "../services/log.h"
#include "../protocol/audio_gain.h"
#include "../protocol/eq.h"
#include "../protocol/latency.h"   /* L5: depth of the output queue */
#include "../protocol/audio_route.h"   /* DEC-1: when a session may still become FLAC */
#include "../protocol/audio_gap.h"     /* OUT-1: output underruns, two-sided and confirmed */

#include <opus.h>

/* === K14 2026-08-27 — FLAC GOES THROUGH LIBAVCODEC ===
 * It is already linked for video on both platforms, and it carries the FLAC
 * decoder: no library to add. */
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>   /* OUT-1: EPIPE / ESTRPIPE from snd_pcm_writei */
#include <time.h>    /* OUT-1: the underrun classifier's monotonic clock */

/* === WHICH SOUND OUTPUT THIS BUILD HAS ===
 *
 * Each target NAMES its sink; nothing is inferred from "not the others". That
 * `#else` used to mean ALSA, which was true for exactly one platform and wrong
 * for every future one: a PS Vita build stopped here, on `alsa/asoundlib.h`,
 * with no way to say what it actually has. A target this ladder does not know
 * now gets NO output and still builds -- it decodes and discards, which is a
 * thing the code already knows how to do (`poussees=n/a` in the bilan) and is
 * far better than failing to compile.
 *
 * The capability macros below are what the rest of the file tests. Keep it that
 * way: `#ifdef __SWITCH__` inside the audio path is how this got tangled.
 */
#if defined(__SWITCH__)
#  include <switch.h>
#  define HAVE_AUDOUT 1

#elif defined(_WIN32)
/* === OUT-3 2026-09-11 - WINDOWS HAS AN OUTPUT: WASAPI BEHIND THE RING ===
 * This branch used to decode and throw the PCM away ("to be implemented
 * through WASAPI/XAudio2 if we ever want audio on Windows"): every Windows
 * user heard silence, and [L5] audio/file was never fed. It now reuses the
 * ALSA design - the ring and a dedicated playback thread - with WASAPI in
 * shared mode, event-driven, behind it (media/audio_out_win.c, where the
 * measured reasons are). SHADOW_WIN_AUDIO=0 keeps the decode-only path. Its
 * numbers speak for this backend only: never compare its audio/file with the
 * console's 27.6 ms, and never let it stand in for audout or ALSA. */
#  include "audio_out_win.h"
#  define HAVE_WASAPI 1

#elif defined(__vita__) || defined(__psp2__)
/* PS Vita: SceAudioOut. It BLOCKS until the buffer is consumed, so it has
 * ALSA's shape rather than WASAPI's - the device paces the play thread, no
 * period event, no room query. That is why it goes behind the same ring.
 * NOT VALIDATED ON HARDWARE: it compiles and its shape follows the SDK, and
 * nobody has heard it. See docs/PSVITA_PORT.md. */
#  include "audio_out_vita.h"
#  define HAVE_SCEAUDIO 1

#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#  include <alsa/asoundlib.h>
#  define HAVE_ALSA 1
#endif

/* Whatever the ladder did not claim is absent. Written out rather than left to
 * `#ifdef`, because every test below reads these as VALUES. */
#ifndef HAVE_AUDOUT
#  define HAVE_AUDOUT 0
#endif
#ifndef HAVE_ALSA
#  define HAVE_ALSA 0
#endif
#ifndef HAVE_WASAPI
#  define HAVE_WASAPI 0
#endif
#ifndef HAVE_SCEAUDIO
#  define HAVE_SCEAUDIO 0
#endif
/* OUT-3: the ring and its playback thread serve every blocking or paced sink
 * alike - ALSA, WASAPI and SceAudioOut; each device's own calls stay under its
 * own macro. HAVE_OUTPUT: the build has a sound output at all (the volume, the
 * bilan's `poussees`). */
#define HAVE_RING   (HAVE_ALSA || HAVE_WASAPI || HAVE_SCEAUDIO)
#define HAVE_OUTPUT (HAVE_AUDOUT || HAVE_RING)

/* S81 - the category is DECLARED here, not inferred from the message text.
 * `alog` stays at INFO: the existing calls do not disappear. `adbg` is there for
 * the bulky lines, which move over to it one at a time. */
#define alog(...) JOURNAL_INFO_(JOURNAL_CAT_AUDIO, __VA_ARGS__)
#define adbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_AUDIO, __VA_ARGS__)
/* Output volume, as a percentage (see streaming/audio_gain.h).
 * Written by the UI, read by the playback thread. An aligned `uint32_t` is
 * written in one go: no lock is needed, and a volume change missed by a few
 * milliseconds has no consequence. */
static volatile uint32_t g_volume = AUDIO_GAIN_DEFAUT;
/* === AUD-CFG-2 2026-09-11 - ONE READER OF `SHADOW_VOLUME`, ONE RULE ===
 * The variable used to be read in two places with two rules: the decoder's
 * open clamped a negative value to 0 (a silent session), while
 * audio_set_volume() took a negative value for "not forced" - so the first
 * menu press brought the saved volume back in the middle of a campaign that had
 * asked for silence. Resolved once, here: absent = -1 (not forced); present =
 * forced, clamped to 0..AUDIO_GAIN_MAX, a negative or unparsable value meaning
 * 0 (atoi's reading, as before). File scope rather than a function static, and
 * not session state: env.txt is applied before anything reads it, and
 * applyToggles() never writes this key. The pause menu's volume row now carries
 * the env.txt note, so the forced value is no longer invisible there. */
static int g_volume_env = -2;   /* -2 not read yet, -1 absent, else 0..AUDIO_GAIN_MAX */

static int audio_volume_forced(void)
{
    if (g_volume_env == -2) {
        const char *e = getenv("SHADOW_VOLUME");
        int v = -1;
        if (e) {
            v = atoi(e);
            if (v < 0) v = 0;
            if (v > (int)AUDIO_GAIN_MAX) v = (int)AUDIO_GAIN_MAX;
        }
        g_volume_env = v;
    }
    return g_volume_env;
}

/* Applied as soon as the decoder opens: a campaign must start silent, not
 * become silent at the first setting change. */
static void audio_volume_apply_env(void)
{
    const int v = audio_volume_forced();
    if (v >= 0) g_volume = (uint32_t)v;
}

void audio_set_volume(uint32_t pourcent)
{
    if (pourcent > AUDIO_GAIN_MAX) pourcent = AUDIO_GAIN_MAX;
    /* `SHADOW_VOLUME` always wins. The measurement campaigns chain dozens of
     * unattended sessions: without this precedence, the first setting read from
     * the parameters file would turn the sound back on, and a ten-minute
     * campaign would become unbearable next to the machine. When absent, the
     * variable changes nothing. AUD-CFG-2: the same rule as at the decoder's
     * open, see audio_volume_forced(). */
    const int v = audio_volume_forced();
    if (v >= 0) pourcent = (uint32_t)v;
    g_volume = pourcent;
}

uint32_t audio_get_volume(void) { return g_volume; }

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS    2

/* === S90 2026-08-29 — THE EQUALISER, AND WHY IT IS GLOBAL ===
 *
 * It lives beside the volume and for the same reason: it does not belong to ONE
 * session. It is set from the settings screen, outside any stream, and it must
 * hold for the next session without being set again. Tying it to the decoder -
 * which is born and dies with the session - would mean replaying it on every
 * connection, that is, losing it half the time.
 *
 * THE DELICATE PART IS THE THREADING. The coefficients are read by the audio
 * output thread while the settings screen rewrites them: writing straight into
 * the live `eq_t` would leave the filter running for an instant with half the
 * old coefficients and half the new - a state that is nobody's filter, and that
 * may be unstable.
 *
 * So we build the new filter ALONGSIDE, then swap it under a lock. The lock is
 * held only for the length of a structure copy: never during I/O, never during
 * processing (`feedback_wss_mutex_deadlock`). */
/* === EQV1(b) 2026-09-11 - A CHAIN, NOT A SINGLE FILTER ===
 * `g_eq` holds what is audible AND what replaces it: a genuine change warms
 * the new filter up for 20 ms beside the old one, then crossfades over 10 ms
 * (eq_chain_t, eq.h). Its 480-frame scratch lives inside it, so it is only
 * ever used under `g_eq_mtx`, like the rest - never on the stack of a libnx
 * thread. The initialiser is the neutral filter of eq_chain_init(c, NULL): a
 * zeroed eq_t is not neutral (bypass false, trim 0). */
static eq_chain_t       g_eq = { .live = { .trim = 1.0f, .bypass = true } };
static pthread_mutex_t  g_eq_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool             g_eq_armed = false;

/* SHADOW_EQ_CROSSFADE - EQV1(b), default 1. 0 installs a CHANGED setting at
 * once with its state zeroed, as before 2026-09-11, click included; identical
 * settings stay a no-op either way (EQV1(a) is bit-exact and untoggled).
 * Offline, 19440 cases (4 presets x every pause-menu notch x 7 signals x 9
 * switch instants) plus the 20 profile switches: the crossfade is never worse
 * than the instant swap; worst notch 639-769 LSB against 16552, worst profile
 * switch 150-266 against 15637; on gcc 16.1 and 14.2. Resolved once, on the
 * settings thread that calls audio_eq_configure. */
static int g_eq_crossfade = -1;
static bool eq_crossfade_on(void)
{
    if (g_eq_crossfade < 0) {
        const char *e = getenv("SHADOW_EQ_CROSSFADE");
        g_eq_crossfade = e ? (atoi(e) != 0) : 1;
    }
    return g_eq_crossfade != 0;
}

/* SHADOW_VOLUME_RAMP - EQV1(c), default 1. 0 applies a new volume from the
 * next sample, as before: a 7455 LSB step for 100 -> 0 % on Opus-decoded
 * music, 14715 for 100 -> 300 %. Resolved by audio_eq_session_start() on the
 * session thread, so the output threads normally only read the cached value. */
static int g_volume_ramp = -1;
static bool volume_ramp_on(void)
{
    if (g_volume_ramp < 0) {
        const char *e = getenv("SHADOW_VOLUME_RAMP");
        g_volume_ramp = e ? (atoi(e) != 0) : 1;
    }
    return g_volume_ramp != 0;
}

void audio_eq_configure(const eq_band_t *bands, int nb, bool auto_trim)
{
    eq_t fresh;
    eq_configure(&fresh, bands, nb, AUDIO_SAMPLE_RATE, auto_trim);
    const bool crossfade = eq_crossfade_on();     /* getenv: outside the lock */

    pthread_mutex_lock(&g_eq_mtx);
    /* === EQV1(a) 2026-09-11 - THE SAME SETTINGS LEAVE THE FILTER ALONE ===
     * The comment here said the old state must not be carried over, and the
     * code installed `fresh` - zeroed - on EVERY call. For a band whose
     * coefficients did not change, zeroing IS the click, and every pause-menu
     * row that goes through applyToggles (hardware Opus, audio quality)
     * re-applies unchanged EQ settings. Measured offline on the real eq.c,
     * HANDHELD, Opus-decoded music at 9 switch instants: median 3955 LSB,
     * worst 13100 (-8 dBFS), ~9 ms - and broadband: 90 % of it survives a 1 kHz
     * high-pass, so the built-in speakers do play it. Leaving the running
     * filter untouched is bit-exact (0 LSB), so this part has no toggle.
     *
     * === EQV1(b) 2026-09-11 - A GENUINE CHANGE IS WARMED UP, THEN FADED IN ===
     * Zeroing a changed filter is the click above, and carrying the old state
     * into it was measured WORSE (see eq.h). The chain runs the new filter
     * beside the old one for 20 ms, then fades it in over 10 ms - never worse
     * than zeroing in 19440 offline cases. Here we only copy structures (22 ns
     * under the lock, against 7 before); the processing happens in
     * eq_on_buffer. SHADOW_EQ_CROSSFADE=0 installs a changed filter at once,
     * zeroed, as before. */
    const int how = eq_chain_set(&g_eq, &fresh, crossfade);
    const bool unchanged = (how == EQ_CHAIN_SAME || how == EQ_CHAIN_CANCELLED);
    g_eq_armed = eq_chain_armed(&g_eq);
    pthread_mutex_unlock(&g_eq_mtx);

    /* === S90d — SAY WHAT IS ARMED ===
     *
     * This line would have saved an evening. The equaliser shipped wired into two
     * of the THREE output paths; the missing one was precisely the console's, and
     * the symptom was "I change preset and I hear no difference". With no trace,
     * the two hypotheses - "the filter is not armed" and "the filter is armed but
     * is not in the path" - are indistinguishable from the armchair.
     *
     * It states the STATE, not the intent: the number of bands REALLY active
     * after clamping, and the computed attenuation. A band refused for
     * instability therefore drops out of the count, which no settings display
     * would ever show. */
    {
        int active_count = 0;
        for (int i = 0; i < EQ_BANDS; i++) if (fresh.active[i]) active_count++;
        alog("[S90] egaliseur : %d/%d bande(s) active(s), attenuation %.1f dB%s%s",
             active_count, nb, (double)(20.0 * log10(fresh.trim > 0.0f ? fresh.trim : 1.0f)),
             fresh.bypass ? " - NEUTRAL, the sound is untouched" : "",
             unchanged                 ? " - unchanged"
             : how == EQ_CHAIN_SWAPPED ? " - switched immediately, the filter reset"
             :                           " - cross-faded (20 ms of warm-up, 10 ms of fade)");
    }
}

bool audio_eq_active(void)
{
    /* EQV1(b): the SETTING, not the processing - during the 30 ms of a fade to
     * neutral the filter still runs, while the settings already say neutral. */
    pthread_mutex_lock(&g_eq_mtx);
    const bool a = !eq_chain_target(&g_eq)->bypass;
    pthread_mutex_unlock(&g_eq_mtx);
    return a;
}

float audio_eq_reponse_db(float freq)
{
    pthread_mutex_lock(&g_eq_mtx);
    /* EQV1(b): the curve of the setting, even while its fade is in flight. */
    const float v = eq_response_db(eq_chain_target(&g_eq), freq, AUDIO_SAMPLE_RATE);
    pthread_mutex_unlock(&g_eq_mtx);
    return v;
}

/* === EQV2 2026-09-11 - EACH SESSION'S FILTER STARTS FROM REST ===
 *
 * The COEFFICIENTS are global on purpose (the S90 block above). The STATE is
 * not a setting, it is sound, and it belongs to the session that produced it.
 * eq.h said "call eq_reset when the source changes (new session)" and nothing
 * did: only audio_eq_configure zeroed the state, and that runs on a settings
 * save or a dock change, never at connect. A session that ended during sound
 * therefore handed its filter memory to the next, played out as a thump in the
 * first buffer. Measured on the REAL audio.c compiled for both output branches
 * (bench_dsp_EQV2, 3 paths x 5 presets x 5 materials x 10 cuts): HANDHELD
 * median -24 dBFS, worst -8.1 dBFS, 13 ms; HEADPHONES tail 68 ms. With this
 * reset before each session, 9000/9000 pairs are bit-identical to a clean
 * start, 0 LSB. Under FLAT (bypass) it changes nothing.
 *
 * Called by the session glue BEFORE the audio decoder exists, so no output
 * thread of the new session can be in eq_on_buffer yet - and the lock makes it
 * safe against anything else. No toggle, like the other session-start resets
 * in ctrl_session.c; and EQV1(a) needs it, since the accidental reset on every
 * settings save is gone.
 * NOT done when the channel resumes after silence (AUD16 revive): a reset under
 * a running stream is itself a transient (~-21 dBFS). That case waits until
 * AUD16b has been exercised. */
void audio_eq_session_start(void)
{
    (void)volume_ramp_on();   /* EQV1(c): resolved here, before any output thread */
    pthread_mutex_lock(&g_eq_mtx);
    /* EQV1(b): a change still warming up or fading is PROMOTED - the session
     * starts with the setting asked for last, from rest. */
    eq_chain_reset(&g_eq);
    g_eq_armed = eq_chain_armed(&g_eq);
    pthread_mutex_unlock(&g_eq_mtx);
}

/* Applies the equaliser to a buffer about to go out. Called from the OUTPUT
 * THREAD, hence under the lock - the operation is pure computation and lasts a
 * few microseconds for ten milliseconds of sound. */
static void eq_on_buffer(int16_t *pcm, size_t frames)
{
    if (!g_eq_armed) return;              /* unprotected read, deliberately: it
                                           * is one bool, and being wrong by a
                                           * frame costs nothing */
    pthread_mutex_lock(&g_eq_mtx);
    /* EQV1(b): one filter in steady state (13.7 us per 10 ms buffer, as
     * before), two for the 30 ms of a change (30 us). A fade that ends on a
     * neutral filter lets the fast path above skip the lock again. */
    eq_chain_process(&g_eq, pcm, frames, AUDIO_CHANNELS);
    g_eq_armed = eq_chain_armed(&g_eq);
    pthread_mutex_unlock(&g_eq_mtx);
}

/* Maximum size of an Opus frame: 120 ms at 48 kHz, i.e. 5760 samples per
 * channel. The buffer used to be sized for 40 ms, and `opus_decode` returned -2
 * ("buffer too small") on longer frames - that is what made Shadow's audio mute
 * while the frames were arriving and decrypting fine.
 * The cost is 23 KB per buffer: irrelevant, even on console. */
#define AUDIO_BUFFER_SAMPLES 5760   // 120 ms @ 48 kHz = the maximum Opus frame
#define AUDIO_BUFFER_BYTES (AUDIO_BUFFER_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))
// audout requires 0x1000 alignment; we pad up to the next boundary.
#define AUDIO_BUFFER_BYTES_ALIGNED ((AUDIO_BUFFER_BYTES + 0xFFF) & ~0xFFF)
#define AUDIO_NUM_BUFFERS  4

struct audio_decoder {
    OpusDecoder *opus;

    /* === K14 - THE CODEC IS NEGOTIATED, SO IT IS DETECTED ===
     * `:base+30`'s codec is negotiated when the session opens: Opus by default,
     * FLAC if "High fidelity" was requested. We could trust what we ASKED for,
     * but the server grants what it wants - and a misrouted frame decodes into
     * noise, not into an error. So we DETECT it, from the frame signature, and
     * stick to that for the session. */
    int          codec_detected;     /* 0 unknown, 1 Opus, 2 FLAC */
    /* DEC-1 2026-09-11 - SHADOW_FLAC_LATE_DETECT, resolved once per decoder
     * (so once per session) at its first packet: 0 unread, 1 off, 2 on. A
     * decoder field, never a static. */
    int          flac_late;
    /* W1 step 0 2026-09-11 - set as the last gesture of a successful
     * audio_decoder_create: only a whole decoder writes the session bilan. */
    bool         created;
    AVCodecContext *flac;
    AVPacket       *flac_pkt;
    AVFrame        *flac_frm;
    /* DEC-5 2026-09-11 - the FLAC decoder could not be built: decided once per
     * decoder, so once per session, never retried. See flac_ready. */
    bool            flac_failed;

#if HAVE_AUDOUT
    /* === A5 2026-08-25 — HARDWARE OPUS DECODER (the `hwopus` service) ===
     *
     * HOS exposes an Opus decoder on its audio DSP. It is OPT-IN here, and that
     * choice deserves explaining: the two reference clients that do hardware
     * VIDEO decoding on Switch - Moonlight-Switch and Switchfin - both decode
     * Opus in SOFTWARE (`opus_multistream_decode`) and never call `hwopus`.
     * Decoding stereo 48 kHz Opus costs a few percent of one core, and each
     * hardware packet costs an IPC round trip in return.
     *
     * We implement it anyway, but behind a toggle and with the measurement that
     * goes with it (`decode_us_total` / `frames_decoded`), so the question can be
     * settled on console instead of assumed. `SHADOW_HWOPUS=1` enables it.
     *
     * Fallback: if initialisation fails, `hwopus_ready` stays false and the
     * software path takes over without the caller knowing. */
    HwopusDecoder hwopus;
    bool       hwopus_ready;
    uint64_t   decode_ticks;      /* cumulative decode time, for the A/B */
    uint8_t    hwopus_in[4096];   /* [HwopusHeader 8 B][Opus packet] */

    bool       audout_init;
    bool       audout_started;
    uint8_t   *bufmem[AUDIO_NUM_BUFFERS];        // page-aligned PCM data
    AudioOutBuffer aob[AUDIO_NUM_BUFFERS];        // audout descriptors
    bool       buf_free[AUDIO_NUM_BUFFERS];       // true = available to write (not being played by audout)
    uint32_t   buf_drop_count;                    // how many times a packet had to be dropped (no free buffer)
    /* === L5 2026-08-29 — REAL DEPTH OF THE OUTPUT QUEUE ===
     * We did not know whether it was 10 or 40 ms: there was a counter of
     * REJECTIONS (`buf_drop_count`) and no counter of buffers IN FLIGHT. Yet it
     * is the depth that is latency, not the rejections.
     * `samples_in_flight` is exact and free: we add on submission and subtract
     * on release, in samples - so without assuming every frame lasts 10 ms (they
     * do, but assuming it would make the measurement wrong the day the server
     * changes its cadence).
     * `samples_submitted` serves the periodic report that queries the driver:
     * the difference with what it has PLAYED is the total depth, HOS driver
     * included - the only segment of the audio budget that cannot be deduced
     * from the code. */
    uint64_t   samples_submitted;
    uint32_t   samples_in_flight;
#endif

#if HAVE_RING
    /* AUD7 - playback ring.
     *
     * Decoding runs on the thread that receives the UDP, and the frames arrive
     * in bursts: writing straight into ALSA overflowed its buffer (795 frames
     * lost out of 1616 on the first measurement), then drained it - hence the
     * crackle. So we decouple: the receive thread drops PCM here, a dedicated
     * thread consumes it at the sound card's pace. It is the dedicated thread
     * that is allowed to block, not the one that receives. */
    int16_t    *ring;             /* interleaved PCM */
    size_t      ring_cap;         /* in samples per channel */
    /* Depth of the DEVICE's queue, published by the play loop so that L13 can
     * cap what the listener hears rather than the half it can see. See L13's
     * comment. One writer, one reader, and a value one iteration stale only
     * delays a trim by that much: no lock. */
    volatile size_t dev_queued;
    size_t      ring_head, ring_tail;
    pthread_mutex_t ring_mtx;
    pthread_cond_t  ring_cv;
    pthread_t   play_thread;
    bool        play_running;
    volatile bool play_abort;
#endif
#if HAVE_ALSA

    snd_pcm_t  *pcm;       // ALSA PCM playback handle
#endif
#if HAVE_SCEAUDIO
    audio_out_vita *vita;  // the Vita's SceAudioOut port, behind the same ring
#endif
#if HAVE_WASAPI
    /* OUT-3 2026-09-11 - the WASAPI side of the ring. `win_state` is written
     * by the playback thread, the device's owner, and read by the producer,
     * which feeds the ring only while it is WIN_OUT_READY: one aligned int,
     * like `play_abort`. The counters are read after the join, for the
     * session line. calloc starts the state at WIN_OUT_OPENING. */
    volatile int  win_state;
    volatile bool win_ever_ready;   /* a device could play this session's ring at least once */
    uint64_t      win_discarded;    /* samples decoded while no device could play them */
    uint32_t      win_reopens;      /* reopens after the endpoint went away */
#endif
#if HAVE_RING

    /* DEC-4 2026-09-11 - the state of the `taux` report (audio_decoder_feed).
     * OUT-3: under HAVE_RING, since the report now serves both ring outputs.
     * These were function statics while the decoder is built per session:
     * from the second session of a process on, its first line measured across
     * the gap between the sessions ("rate 0.11 over the last 45 s (silence or
     * loss IN PROGRESS)") and every cumulative figure mixed the two sessions
     * (0.62, 0.65, 0.68 where the audio ran at 1.00). calloc zeroes them with
     * the decoder; the arithmetic is unchanged. No toggle: log only, like
     * latency_reset_session and session_stats_reset. */
    int64_t     rate_t0_ms, rate_last_ms;
    uint64_t    rate_samples_acc, rate_samples_prev;
#endif

    /* === EQV1(c) 2026-09-11 - THE VOLUME THE LAST BUFFER WENT OUT AT ===
     * A volume change ramps across one buffer from this value (gain_on_buffer).
     * Per decoder and zeroed by calloc: a session's first buffer takes the
     * current volume directly, as before - never a ramp from the previous
     * session's level. ONE thread per platform touches it: the ALSA play
     * thread, or the thread that feeds audout. */
    uint32_t   gain_prev;
    bool       gain_primed;

    /* === OUT-1 2026-09-11 - UNDERRUNS: THE CLASSIFIER'S STATE ===
     * streaming/audio_gap.h, its line budget and the positive-control
     * diagnostic. Per decoder, so per session: calloc starts them fresh. In a
     * function static, the last frame of one session would stand as the
     * "audible frame before the gap" of the next session's first refill - dry,
     * since every output starts empty - and every session start would count
     * an underrun (tests/test_audio_gap.c, the two-session counter-case). ONE
     * writer: the thread that feeds the output - audio_decoder_feed's caller on
     * audout, the playback thread on ALSA and WASAPI; any other thread reads
     * through audio_gap_read(). */
    audio_gap_t gap;
    bool        gap_on;       /* SHADOW_AUD_SOUSFLUX, resolved at create, before any output thread */
    bool        play_ran;     /* ALSA/WASAPI: the playback thread ran this session (set at the join) */
    int64_t     gap_t_us;     /* audout: when the frame being submitted reached the output */
    uint32_t    gap_lines;    /* `audio: sous-flux #N` lines written this session */
    int64_t     diag_t0_ms;   /* SHADOW_DIAG_AUDIO_MUTE_*: this decoder's creation */
    int         diag_state;   /* 0 before the mute, 1 muting, 2 over */
    uint64_t    diag_muted;   /* decoded samples the diagnostic threw away */

    audio_stats_t stats;
};

/* === OUT-1 2026-09-11 - EVERY OUTPUT PATH WAS BLIND TO ITS OWN GAPS ===
 *
 * Nothing counted an underrun: a recovered ALSA -EPIPE leaves no trace (see
 * the L12 note), audout counted overflow only, and [L5] audio/file samples
 * the queue right after a write, at its fullest. streaming/audio_gap.h holds
 * the rule and why it is two-sided and confirmed: on the assessment's models
 * it counts 0 false underruns where the first draft counted 88 in 90 s of a
 * silent VM, and it finds every real one. This file feeds it, one hook per
 * output, on the thread that feeds that output:
 *   - audout, both submit paths (gap_refill_audout): after the reclaim loop,
 *     before the buffer counts in flight; ONE played-sample query, only when
 *     our in-flight count says AUDIO_GAP_QUERY_INFLIGHT buffers or fewer AND
 *     the answer can count;
 *   - ALSA (audio_play_thread): when the chunk comes back after a WAIT, dry if
 *     the device is in XRUN or snd_pcm_delay() says nothing is queued; a raw
 *     -EPIPE/-ESTRPIPE goes to xrun=, apart;
 *   - WASAPI (win_play_loop): dry is observed at the wake where the engine
 *     asked for data, the ring had none and the engine held less than one
 *     period, and handed over at the next refill.
 * Audibility is always the peak of the DECODED PCM, before the EQ and the gain
 * rewrite the buffer: under SHADOW_VOLUME=0 - every unattended campaign - a
 * peak read after the gain is 0 and would count nothing.
 *
 * Published as ` underrun=N (X ms) xrun=M` at the end of the session bilan, of
 * the FLAC 500-frame line and of `[L5] file audio`, where an output is
 * measured (gap_measured) - never a 0 where nothing is. Each counted underrun
 * also gets one `audio: sous-flux #N (X ms)` line, AUDIO_GAP_LOG_LINES per
 * session: a cumulative key on a 10 s line cannot be matched with a [D4]
 * freeze by its timestamp; this line can.
 *
 * SHADOW_AUD_SOUSFLUX - default 1. 0 turns it all off: no peak scan, no audout
 * query, no ALSA state or delay call, no WASAPI padding query, no key, no
 * line. Measured offline (tests/test_audio_gap.c, 158 checks, MinGW and glibc
 * with ASan+UBSan): 0 false counts and every real underrun found in every
 * scenario of the assessment's audout and ALSA models; the peak scan costs
 * 171.6 ns per 10 ms frame (i7-9750H). Resolved once per decoder at its
 * create, on the session thread, before any output thread exists. */
#define AUDIO_GAP_LOG_LINES 20

static int g_sousflux = -1;
static bool sousflux_on(void)
{
    if (g_sousflux < 0) {
        const char *e = getenv("SHADOW_AUD_SOUSFLUX");
        g_sousflux = e ? (atoi(e) != 0) : 1;
    }
    return g_sousflux != 0;
}

static int64_t gap_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* === AUD-INS-4 2026-09-11 - ONE SUM FOR `overflow=` ===
 * The frames the output could not take: stats.buffers_dropped on the ring
 * (ALSA, WASAPI), buf_drop_count on audout. audio_decoder_get_stats() made the
 * sum, but the FLAC 500-frame report read stats.buffers_dropped directly and
 * printed `overflow=0` forever on console, while the same log's `[L5] ...
 * dropped=` carried the true count. Both read this function now, so they cannot
 * drift apart again. A function rather than a sum written inside the log
 * call: alog is a macro, and a preprocessor directive among a macro's
 * arguments is undefined behaviour (C11 6.10.3p11). No toggle: an instrument. */
static uint32_t aud_drops(const audio_decoder *a)
{
#if HAVE_AUDOUT
    return a->stats.buffers_dropped + a->buf_drop_count;
#else
    return a->stats.buffers_dropped;
#endif
}

/* Whether this session's output is measured - so whether the keys print. */
static bool gap_measured(const audio_decoder *a)
{
    if (!a->gap_on) return false;
#if HAVE_AUDOUT
    return a->audout_started;
#elif HAVE_WASAPI
    return a->ring && a->win_ever_ready;
#elif HAVE_ALSA
    return a->play_running || a->play_ran;
#elif HAVE_SCEAUDIO
    /* The port opens at creation, so a running thread means a measured output -
     * there is no equivalent of WASAPI's "ready yet?" to wait for. */
    return a->vita && (a->play_running || a->play_ran);
#else
    return false;
#endif
}

/* The keys, for the end of a line: "" where nothing is measured. `xrun=` is
 * ALSA's raw -EPIPE/-ESTRPIPE count: audout and WASAPI have no such event (a
 * starved engine plays silence, which is what underrun= counts), so it reads
 * `n/a` there - never a 0 that would read as a measurement (the ING-1 and W1
 * precedent). */
static void gap_keys(const audio_decoder *a, char *buf, size_t n)
{
    buf[0] = '\0';
    if (!gap_measured(a)) return;
    uint32_t u = 0, ms = 0, xr = 0;
    audio_gap_read(&a->gap, &u, &ms, &xr);
#if HAVE_ALSA
    snprintf(buf, n, " underrun=%u (%u ms) xrun=%u", (unsigned)u, (unsigned)ms, (unsigned)xr);
#else
    (void)xr;
    snprintf(buf, n, " underrun=%u (%u ms) xrun=n/a", (unsigned)u, (unsigned)ms);
#endif
}

/* One frame reaches the output: the classifier, then - for a counted
 * underrun - its line, within the session's budget. The writer thread only. */
static void gap_refill(audio_decoder *a, int64_t now_us, bool dry, bool audible)
{
    const int64_t before = a->gap.gap_us;
    const unsigned ev = audio_gap_refill(&a->gap, now_us, dry, audible);
    if (!(ev & AUDIO_GAP_COUNTED) || a->gap_lines >= AUDIO_GAP_LOG_LINES) return;
    a->gap_lines++;
    uint32_t n = 0;
    audio_gap_read(&a->gap, &n, NULL, NULL);
    alog("audio: sous-flux #%u (%u ms)%s", (unsigned)n,
         (unsigned)((a->gap.gap_us - before) / 1000),
         a->gap_lines == AUDIO_GAP_LOG_LINES
             ? " - last line of the session, counting continues in the summary" : "");
}

/* === OUT-1 - SHADOW_DIAG_AUDIO_MUTE_AT_S / _MS: A GAP ON DEMAND ===
 * DIAGNOSTIC, OFF by default: the counter's positive control, in the VI1
 * pattern. AT seconds after this session's decoder was created, and for MS
 * milliseconds (default 300, the console's Wi-Fi freeze), decoded frames are
 * thrown away HERE, before any output sees them: the output runs dry exactly
 * as if the frames had stopped arriving. It removes frames, never adds one,
 * and emits nothing - not a byte on :base+30 (S57, KB §3.38). Once per
 * session. With sound playing, an injection adds one underrun of about MS -
 * 288-299 ms for 300 on the real WASAPI engine (W5's harness, digital
 * silence) - and in 3 injections of 4 a second one of 9-10 ms, 0-70 ms later,
 * as the queue restarts empty; on a silent VM it adds none. Process
 * configuration, read once, at the first create. */
static int g_diag_amute_at_s = -2;   /* -2 unread, <= 0 off */
static int g_diag_amute_ms   = 300;

static void diag_audio_mute_resolve(void)
{
    if (g_diag_amute_at_s != -2) return;
    const char *e = getenv("SHADOW_DIAG_AUDIO_MUTE_AT_S");
    g_diag_amute_at_s = e ? atoi(e) : 0;
    const char *m = getenv("SHADOW_DIAG_AUDIO_MUTE_MS");
    if (m && atoi(m) > 0) g_diag_amute_ms = atoi(m);
}

/* True when this decoded frame must be thrown away. Called on the thread
 * that calls audio_decoder_feed. */
static bool diag_audio_muted(audio_decoder *a, int samples)
{
    if (g_diag_amute_at_s <= 0 || a->diag_state == 2) return false;
    const int64_t el = gap_now_us() / 1000 - a->diag_t0_ms;
    const int64_t on = (int64_t)g_diag_amute_at_s * 1000;
    if (el < on) return false;
    if (el >= on + g_diag_amute_ms) {
        if (a->diag_state == 1)
            alog("[DIAG] audio retablie : %u echantillons decodes jetes (SHADOW_DIAG_AUDIO_MUTE_*)",
                 (unsigned)a->diag_muted);
        a->diag_state = 2;
        return false;
    }
    if (a->diag_state == 0) {
        a->diag_state = 1;
        alog("[DIAG] audio coupee volontairement pendant %d ms (SHADOW_DIAG_AUDIO_MUTE_*) — "
             "decoded frames dropped here, nothing is emitted", g_diag_amute_ms);
    }
    a->diag_muted += (uint64_t)samples;
    return true;
}

#if HAVE_OUTPUT
/* EQV1(c) - the volume, the last gesture before the hardware on EVERY output
 * path - ALSA, WASAPI since OUT-3, and audout's two (the S90d rule: one
 * sequence, written once). `g_volume` is read ONCE per buffer, so the ramp and
 * what it records agree. */
static void gain_on_buffer(audio_decoder *a, int16_t *pcm, size_t frames)
{
    const uint32_t to = g_volume;
    if (a->gain_primed && volume_ramp_on())
        audio_gain_ramp(pcm, frames, AUDIO_CHANNELS, a->gain_prev, to);
    else
        audio_gain_apply(pcm, frames * AUDIO_CHANNELS, to);
    a->gain_prev   = to;
    a->gain_primed = true;
}
#endif

#if HAVE_WASAPI
/* === OUT-3 2026-09-11 - THE WINDOWS PLAYBACK LOOP: THE ENGINE'S CLOCK ===
 *
 * The ALSA loop blocks in snd_pcm_writei, so the sound card paces it. WASAPI
 * in event mode is paced the same way by other means: the engine signals an
 * event once per device period (10 ms on the dev machine); the loop wakes,
 * asks how much room the engine buffer has, and moves whole 480-frame chunks
 * from the ring into it - each through eq_on_buffer then gain_on_buffer,
 * exactly the sequence of the other outputs (S90d). No process timer paces
 * anything here, so nothing needs timeBeginPeriod, and nothing calls it: that
 * setting is process-wide and would move the Windows baselines of KB
 * ING-1/ING-2 (WSAPoll(5) 15.4 -> 5.3 ms).
 *
 * THE DEVICE IS OPENED HERE, ON THIS THREAD, after CoInitializeEx(MTA) -
 * never in audio_decoder_create: an open took 83 ms to 1.2 s on the dev
 * machine, and create runs on the session thread. Until the device is ready
 * the producer discards what it decodes (audio_push_pcm), so the session never
 * waits for the device and the ring never fills behind a device that is not
 * there. If the open fails the decoder stays: decode-only for the session,
 * never NULL - exactly the Windows client of before.
 *
 * When the endpoint goes away mid-session (headphones unplugged, the device
 * disabled, the audio service restarted: AUDCLNT_E_DEVICE_INVALIDATED or
 * AUDCLNT_E_SERVICE_NOT_RUNNING) the loop closes it and reopens the default
 * device, at most once per second - local calls, nothing on the network, so
 * uncapped, but logged only #1-3 and then one in sixty. Any other failure
 * ends the output for the session, with one line.
 *
 * Teardown happens here too, since the objects belong to the thread that
 * created them: audio_decoder_destroy sets `play_abort`, the loop sees it
 * within one period (100 ms at most, the event wait's bound), closes the
 * device and leaves COM, and the join returns. An open in progress cannot be
 * interrupted: a session that ends during its first second may wait for it.
 *
 * MMCSS: the thread registers as a "Pro Audio" task before the open
 * (SHADOW_WIN_AUDIO_MMCSS, below) - its own scheduling class, nothing
 * process-wide. */
enum { WIN_OUT_OPENING = 0, WIN_OUT_READY, WIN_OUT_REOPENING, WIN_OUT_DOWN };

#define WIN_OUT_BUFFER_MS 20    /* the engine buffer asked for */
#define WIN_OUT_WAIT_MS   100   /* bound on one event wait: the abort poll */
#define WIN_OUT_RETRY_MS  1000  /* at most one open attempt per second */

/* SHADOW_WIN_AUDIO - OUT-3, default 1. 0 builds no ring and no playback
 * thread: the decode-only path of before 2026-09-11 (decoded and counted,
 * never played, [L5] audio/file never fed). Measured offline on the real
 * audio.c against this machine's default device, digital silence: see the
 * KB entry (OUT-3). Resolved once, on the session thread, at the first
 * decoder's create. */
static int g_win_audio = -1;
static bool win_audio_on(void)
{
    if (g_win_audio < 0) {
        const char *e = getenv("SHADOW_WIN_AUDIO");
        g_win_audio = e ? (atoi(e) != 0) : 1;
    }
    return g_win_audio != 0;
}

/* SHADOW_WIN_AUDIO_MMCSS - OUT-3, default 1. The playback thread registers
 * with MMCSS as a "Pro Audio" task: the scheduling class Windows reserves for
 * audio threads, for THIS thread only - nothing process-wide, unlike
 * timeBeginPeriod. The plan allowed it only if a load cell failed, and one did:
 * at normal priority, with 4 busy threads (burst arrivals), the engine starved
 * - L13 trims and overflows in 2 runs of 3, engine rate down to -4992 ppm - and
 * with 12 busy threads the output broke down (-84761 ppm, 57 trims).
 * 2026-09-11 A/B, EVIDENCE PARTIAL, and taken under a foreign real-time load:
 * a DAW playing through ASIO (Kontakt 8) and OBS recording ran on the same
 * machine throughout, and the default USB DAC refused every open (ERROR_BUSY,
 * probably held by that DAW - unverified), so the runs used the laptop's
 * Realtek endpoint. Real audio.c, the thread registered from the harness,
 * digital silence, arms interleaved, 30 s runs. Burst arrivals with 4 busy
 * threads, the cell that failed: 0 failing runs of 3 with MMCSS against 3 of
 * 6 without (1 of 3 in the interleaved window, 2 of 3 in the matrix). Steady
 * arrivals with 4 busy threads, and the idle control: no difference. With 12
 * busy threads (every core) the output breaks down either way and MMCSS is
 * within the spread (37/51/40 L13-or-overflow events against 51/38/47; engine
 * rate -54/-81/-46 against -89/-55/-54 per mille of real time). About half the
 * starved wakes there had an EMPTY ring: the producer was late, not this
 * thread - the session receive thread in the client, which no priority on
 * this thread can help. Kept on because the plan names it as the remedy for a
 * failing load cell, no cell measured worse with it, and it is per thread.
 * 0 leaves the thread at normal priority, as the first draft of OUT-3 did. */
static int g_win_mmcss = -1;
static bool win_mmcss_on(void)
{
    if (g_win_mmcss < 0) {
        const char *e = getenv("SHADOW_WIN_AUDIO_MMCSS");
        g_win_mmcss = e ? (atoi(e) != 0) : 1;
    }
    return g_win_mmcss != 0;
}

static int64_t win_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* An absolute CLOCK_REALTIME deadline `ms` from now, for pthread_cond_timedwait. */
static void win_deadline(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (long)ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* One open, logged with what the engine GRANTED (the L12b rule). Attempt 0
 * is the session's first; later ones are reopens, whose failures are logged
 * #1-3 and then one in sixty. OUT-1: `period` receives the engine's period -
 * what one of its passes takes (win_play_loop's dry observation). */
static audio_out_win *win_open(unsigned attempt, uint32_t *period)
{
    audio_out_win_info inf;
    long hr = 0;
    const int64_t t0 = win_now_ms();
    audio_out_win *o = audio_out_win_open(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                                          WIN_OUT_BUFFER_MS, &inf, &hr);
    const int dt = (int)(win_now_ms() - t0);
    if (!o) {
        if (attempt == 0)
            alog("audio: WASAPI indisponible (hr=0x%08lx, %d ms) — decodage seul "
                 "for this session", (unsigned long)hr, dt);
        else if (attempt <= 3 || attempt % 60 == 0)
            alog("audio: WASAPI reopen #%u failed (hr=0x%08lx)", attempt,
                 (unsigned long)hr);
        return NULL;
    }
    alog("audio: WASAPI opened in %d ms - buffer %.1f ms (%u frames), period %.1f ms "
         "(min %.1f), stream latency %.1f ms, mixer %u Hz %u channels",
         dt, (double)inf.buffer_frames * 1000.0 / AUDIO_SAMPLE_RATE,
         (unsigned)inf.buffer_frames,
         (double)inf.period_frames * 1000.0 / AUDIO_SAMPLE_RATE,
         (double)inf.min_period_frames * 1000.0 / AUDIO_SAMPLE_RATE,
         (double)inf.stream_latency_frames * 1000.0 / AUDIO_SAMPLE_RATE,
         (unsigned)inf.mix_rate, (unsigned)inf.mix_channels);
    if (period) *period = inf.period_frames;
    return o;
}

/* Before the first write the engine sends no event, so the loop waits for
 * the producer as the ALSA loop does: on the ring's condition, 10 ms at a
 * time, until one chunk is there. False when aborted. */
static bool win_wait_first_chunk(audio_decoder *a, size_t chunk)
{
    pthread_mutex_lock(&a->ring_mtx);
    while (!a->play_abort
           && (a->ring_head + a->ring_cap - a->ring_tail) % a->ring_cap < chunk) {
        struct timespec ts;
        win_deadline(&ts, 10);
        pthread_cond_timedwait(&a->ring_cv, &a->ring_mtx, &ts);
    }
    pthread_mutex_unlock(&a->ring_mtx);
    return !a->play_abort;
}

/* While reopening: sleep on the ring's condition, which destroy broadcasts. */
static void win_idle(audio_decoder *a, int ms)
{
    struct timespec ts;
    win_deadline(&ts, ms);
    pthread_mutex_lock(&a->ring_mtx);
    if (!a->play_abort) pthread_cond_timedwait(&a->ring_cv, &a->ring_mtx, &ts);
    pthread_mutex_unlock(&a->ring_mtx);
}

static void win_play_loop(audio_decoder *a, int16_t *out, size_t CHUNK)
{
    const int com = audio_out_win_thread_begin();
    /* Before the open, so the open itself and every wake run in the class. */
    void *mm = win_mmcss_on() ? audio_out_win_mmcss_begin() : NULL;
    if (win_mmcss_on())
        alog("audio: WASAPI playback thread %s", mm ? "en MMCSS Pro Audio"
                                                  : "outside MMCSS (refused by the service)");
    /* OUT-1: the engine's period, and the dry observation carried from a
     * starved wake to the next refill (see "WHEN THE ENGINE PLAYS SILENCE"). */
    uint32_t period = 0;
    bool gap_dry = false;
    audio_out_win *o = win_open(0, &period);
    int64_t last_try_ms = win_now_ms(), lost_ms = 0;
    unsigned tries = 0;
    bool started = false;

    if (o) {
        a->win_ever_ready = true;
        a->win_state = WIN_OUT_READY;
    } else {
        a->win_state = WIN_OUT_DOWN;   /* decode-only for the session */
    }
    while (!a->play_abort && (o || a->win_state == WIN_OUT_REOPENING)) {
        if (!o) {
            if (win_now_ms() - last_try_ms < WIN_OUT_RETRY_MS) {
                win_idle(a, WIN_OUT_WAIT_MS);
                continue;
            }
            last_try_ms = win_now_ms();
            o = win_open(++tries, &period);
            if (!o) continue;
            /* What the ring held when the device went away is a second old or
             * more: drop it rather than play it late. The producer has not
             * pushed since (the state was not READY). */
            pthread_mutex_lock(&a->ring_mtx);
            a->ring_tail = a->ring_head;
            pthread_mutex_unlock(&a->ring_mtx);
            a->win_reopens++;
            alog("audio: WASAPI reopened after %u attempt(s), %d ms with no output",
                 tries, (int)(win_now_ms() - lost_ms));
            tries = 0;
            started = false;
            gap_dry = false;   /* OUT-1: a device that went away is not an underrun */
            a->win_state = WIN_OUT_READY;
            continue;
        }

        if (!started) {
            if (!win_wait_first_chunk(a, CHUNK)) break;
        } else {
            /* THE ENGINE'S CLOCK: one event per device period. The bound is
             * only the abort poll; a timeout falls through to the same
             * room/write step. */
            audio_out_win_wait(o, WIN_OUT_WAIT_MS);
            if (a->play_abort) break;
        }

        uint32_t room = 0, wrote = 0;
        long hr = audio_out_win_room(o, &room);
        while (hr >= 0 && room >= CHUNK && !a->play_abort) {
            pthread_mutex_lock(&a->ring_mtx);
            const size_t avail = (a->ring_head + a->ring_cap - a->ring_tail) % a->ring_cap;
            if (avail < CHUNK) { pthread_mutex_unlock(&a->ring_mtx); break; }
            for (size_t i = 0; i < CHUNK; i++) {
                size_t src = ((a->ring_tail + i) % a->ring_cap) * AUDIO_CHANNELS;
                out[i * AUDIO_CHANNELS]     = a->ring[src];
                out[i * AUDIO_CHANNELS + 1] = a->ring[src + 1];
            }
            a->ring_tail = (a->ring_tail + CHUNK) % a->ring_cap;
            pthread_mutex_unlock(&a->ring_mtx);

            /* OUT-1 - the refill: the peak of the DECODED chunk, before the EQ
             * and the gain rewrite it, and - for the first chunk of this wake -
             * the dry observation carried from the starved wake (below). */
            int64_t gap_t = 0;
            if (a->gap_on) {
                gap_t = gap_now_us();
                gap_refill(a, gap_t, gap_dry,
                           audio_gap_audible(audio_gap_peak(out, CHUNK * AUDIO_CHANNELS)));
                gap_dry = false;
            }

            /* S90 then EQV1(c): the equaliser first, the volume last - the
             * sequence of every other output path. */
            eq_on_buffer(out, CHUNK);
            gain_on_buffer(a, out, CHUNK);

            /* === THE WASAPI WRITE SITE ===
             * One 10 ms chunk into the engine buffer, never more than room()
             * reported; the first success starts the stream. A failure here
             * loses this chunk, and the device is handled below. */
            hr = audio_out_win_write(o, out, (uint32_t)CHUNK);
            if (hr < 0) break;
            if (a->gap_on)   /* OUT-1: accepted - never for a failed write */
                audio_gap_played(&a->gap, gap_t, (int64_t)CHUNK * 1000000 / AUDIO_SAMPLE_RATE, -1);
            started = true;
            room  -= (uint32_t)CHUNK;
            wrote += (uint32_t)CHUNK;
        }

        /* === OUT-1 2026-09-11 - WHEN THE ENGINE PLAYS SILENCE ===
         * "GetCurrentPadding() == 0 at a wake" is NOT an underrun: in event
         * mode the engine signals right after taking a period, so a healthy
         * stream fed just in time reads 0 at most wakes (1674 of 2945 at rest,
         * W5). Nor is "padding 0 at a refill after the ring waited": at the
         * refill the padding reads 0 whether or not the engine starved. What
         * decides is THIS wake: the engine asked for data (room for a chunk),
         * the ring had none to give, and the engine holds less than one period
         * - its next pass takes what is left and plays silence for the rest.
         * That is the dry observation, carried to the next refill, where the
         * two-sided rule sorts sound from silence. One GetCurrentPadding, only
         * when it can count (audio_gap_query_useful) and only when this wake
         * wrote less than a period: never on a silent VM. Offline, a model of
         * this loop (the engine's passes, the ring, the producer on the
         * 15.6 ms timer quantum) with the real classifier and a ground truth,
         * 60 s per case: 0 false counts and every real gap found in all nine
         * cases; the "padding 0 at a refill" reading counted 1833 false
         * underruns in steady music (472 in pairs, 624 in bursts). On the REAL
         * engine (W5's harness on the Realtek endpoint, digital silence, 20 s
         * per cadence) that reading fired 449 / 185 / 63 times in steady /
         * pairs / bursts, and this rule counted only the 1-2 gaps of 2-21 ms in
         * the stream's first 90 ms - real: the stream starts on one chunk - and
         * each 300 ms mute injection (a 288-299 ms gap). */
        if (a->gap_on && started && hr >= 0 && room >= CHUNK && !a->play_abort) {
            const uint32_t per = period ? period : (uint32_t)CHUNK;
            if (wrote < per && audio_gap_query_useful(&a->gap, true)) {
                uint32_t pad = 0;
                if (audio_out_win_queued_frames(o, &pad) >= 0 && pad < per)
                    gap_dry = true;
            }
        }

        if (hr < 0) {
            audio_out_win_close(o);
            o = NULL;
            if (audio_out_win_lost(hr)) {
                lost_ms = win_now_ms();
                a->win_state = WIN_OUT_REOPENING;
                alog("audio: WASAPI lost (hr=0x%08lx: device removed or the service "
                     "stopped) - reopened at most once per second", (unsigned long)hr);
            } else {
                a->win_state = WIN_OUT_DOWN;
                alog("audio: WASAPI failed (hr=0x%08lx) - decode only until the "
                     "end of the session", (unsigned long)hr);
            }
            continue;
        }

        /* L5 - the queue as the listener has it: the ring plus what the engine
         * still holds, sampled after each wake that wrote (the ALSA rule: one
         * sample per write, none while starved). The same stage as L12's
         * snd_pcm_delay + ring - but of THIS backend only. */
        if (wrote > 0 && latency_enabled()) {
            uint32_t pad = 0;
            if (audio_out_win_queued_frames(o, &pad) >= 0) {
                pthread_mutex_lock(&a->ring_mtx);
                const size_t filled =
                    (a->ring_head + a->ring_cap - a->ring_tail) % a->ring_cap;
                pthread_mutex_unlock(&a->ring_mtx);
                latency_add(LAT_AUD_QUEUE,
                            ((int64_t)pad + (int64_t)filled) * 1000000LL / AUDIO_SAMPLE_RATE);
            }
        }
    }
    if (o) audio_out_win_close(o);
    if (mm) audio_out_win_mmcss_end(mm);
    audio_out_win_thread_end(com);
}

/* One line per session, written after the join, when the playback thread's
 * counters are final: what the output did. */
static void win_log_session(const audio_decoder *a)
{
    if (!a->created || !a->ring) return;
    const char *st = a->win_state == WIN_OUT_READY     ? "ouverte"
                   : a->win_state == WIN_OUT_REOPENING ? "lost, reopening"
                   : a->win_state == WIN_OUT_DOWN      ? (a->win_ever_ready ? "perdue"
                                                                            : "never opened")
                   :                                     "encore en ouverture";
    alog("audio: WASAPI output: %s, %u reopen(s), %u samples decoded with no "
         "output (%.2f s)", st, (unsigned)a->win_reopens, (unsigned)a->win_discarded,
         (double)a->win_discarded / AUDIO_SAMPLE_RATE);
}
#endif

#if HAVE_ALSA || HAVE_SCEAUDIO
/* === A SINK THAT PACES US ==================================================
 *
 * ALSA and the Vita's SceAudioOut share a shape WASAPI does not: their write
 * BLOCKS until the device has room, so the DEVICE sets the tempo and the loop
 * below needs no clock of its own. WASAPI is driven by a period event and keeps
 * its own `win_play_loop`.
 *
 * The four operations are the four the ALSA loop already performed -- this
 * interface was READ OFF the code, not invented for it, which is why adopting
 * it moved no logic. Before, ALSA sat inline in the play thread while WASAPI
 * had a function: ALSA looked like the default and every other output like an
 * exception. It is not the default. A fourth blocking sink is now a struct. */
typedef struct {
    const char *name;
    /* Is the device observably DRY? Asked only at a refill that came back
     * after a wait, and only when the answer can count (OUT-1). */
    bool (*dry)(audio_decoder *a);
    /* Writes `frames` interleaved frames, blocking. Frames written, or a
     * negative device error. */
    long (*write)(audio_decoder *a, const int16_t *pcm, size_t frames);
    /* What to do with a negative `write`: recover, count, log. */
    void (*on_error)(audio_decoder *a, long err);
    /* Frames the device still holds, or -1 when it cannot say (L12). */
    long (*queued)(audio_decoder *a);
} audio_sink;

#if HAVE_ALSA
/* ALSA. The four operations exactly as they were written inline, moved. */
static bool alsa_dry(audio_decoder *a)
{
    /* The device in XRUN (a hw device drains into it, its stop threshold being
     * the buffer size), or `snd_pcm_delay` saying nothing is queued, or failing
     * (plugins, which may never report XRUN). Both calls on the thread that
     * owns the handle. */
    snd_pcm_sframes_t d = 0;
    return snd_pcm_state(a->pcm) == SND_PCM_STATE_XRUN
        || snd_pcm_delay(a->pcm, &d) < 0 || d <= 0;
}
static long alsa_write(audio_decoder *a, const int16_t *pcm, size_t frames)
{
    return (long)snd_pcm_writei(a->pcm, pcm, frames);
}
static void alsa_on_error(audio_decoder *a, long w)
{
    if (a->gap_on && (w == -EPIPE || w == -ESTRPIPE)) audio_gap_xrun(&a->gap);
    int rec = snd_pcm_recover(a->pcm, (int)w, 1);
    if (rec < 0 && a->stats.decode_errors < 5)
        alog("audio: snd_pcm_writei FAIL: %s", snd_strerror((int)w));
}
static long alsa_queued(audio_decoder *a)
{
    snd_pcm_sframes_t d = 0;
    return (snd_pcm_delay(a->pcm, &d) == 0 && d >= 0) ? (long)d : -1;
}
static const audio_sink ALSA_SINK = { "ALSA", alsa_dry, alsa_write, alsa_on_error, alsa_queued };
#endif /* HAVE_ALSA */

#if HAVE_SCEAUDIO
/* PS Vita, SceAudioOut. NOT VALIDATED ON HARDWARE - it compiles and follows the
 * SDK; nobody has heard it. See docs/PSVITA_PORT.md.
 *
 * The device has no XRUN notion to ask about, so `dry` reads the queue instead:
 * zero frames left at a refill that came back after a wait is the same
 * observation ALSA makes through its state machine, taken the only way this
 * device offers. */
static bool vita_dry(audio_decoder *a)
{
    /* -1 means the device could not say, and "cannot say" is not "dry": OUT-1
     * counts an underrun only on an OBSERVATION, never on the absence of one. */
    return audio_out_vita_queued(a->vita) == 0;
}
static long vita_write(audio_decoder *a, const int16_t *pcm, size_t frames)
{
    return audio_out_vita_write(a->vita, pcm, (int)frames);
}
static void vita_on_error(audio_decoder *a, long w)
{
    /* No recovery call exists: `sceAudioOutOutput` either accepts the buffer or
     * the port is gone. Counted like an xrun so the bilan says something rather
     * than nothing, and logged under the same five-line budget. */
    if (a->gap_on) audio_gap_xrun(&a->gap);
    if (a->stats.decode_errors < 5)
        alog("audio: sceAudioOutOutput FAIL: %ld", w);
}
static long vita_queued(audio_decoder *a)
{
    return audio_out_vita_queued(a->vita);
}
static const audio_sink VITA_SINK = { "SceAudioOut", vita_dry, vita_write, vita_on_error, vita_queued };
#endif /* HAVE_SCEAUDIO */

static void blocking_play_loop(audio_decoder *a, int16_t *out, const size_t CHUNK,
                               const audio_sink *snk)
{
    while (!a->play_abort) {
        size_t avail;
        bool waited = false;   /* OUT-1: this chunk came back after a wait */
        pthread_mutex_lock(&a->ring_mtx);
        /* THE WAIT SITE: the ring holds less than one chunk, so the
         * thread waits for the producer, 10 ms at a time, while the sound card
         * plays what it already holds. */
        while (!a->play_abort) {
            avail = (a->ring_head + a->ring_cap - a->ring_tail) % a->ring_cap;
            if (avail >= CHUNK) break;
            waited = true;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&a->ring_cv, &a->ring_mtx, &ts);
        }
        if (a->play_abort) { pthread_mutex_unlock(&a->ring_mtx); break; }
        for (size_t i = 0; i < CHUNK; i++) {
            size_t src = ((a->ring_tail + i) % a->ring_cap) * AUDIO_CHANNELS;
            out[i * AUDIO_CHANNELS]     = a->ring[src];
            out[i * AUDIO_CHANNELS + 1] = a->ring[src + 1];
        }
        a->ring_tail = (a->ring_tail + CHUNK) % a->ring_cap;
        pthread_mutex_unlock(&a->ring_mtx);

        /* === OUT-1 2026-09-11 - THE REFILL ===
         * The peak of the DECODED chunk, before the EQ and the gain rewrite
         * it. Dry only when the chunk came back after a WAIT - the one moment
         * the device may have drained - and only when the answer can count
         * (audio_gap_query_useful: once per episode, never on silence): the
         * device in XRUN (a hw device drains into it, its stop threshold being
         * the buffer size); the sink answers it the way its device allows -
         * see `alsa_dry` and `vita_dry`, which observe the same thing through
         * different windows. Always on the thread that owns the handle. */
        int64_t gap_t = 0;
        if (a->gap_on) {
            const bool aud = audio_gap_audible(audio_gap_peak(out, CHUNK * AUDIO_CHANNELS));
            bool dry = false;
            if (waited && audio_gap_query_useful(&a->gap, aud)) {
                dry = snk->dry(a);
            }
            gap_t = gap_now_us();
            gap_refill(a, gap_t, dry, aud);
        }

        /* S90 - the equaliser BEFORE the volume. Order matters: the volume
         * clamps on output, so it must be the last to write. Equalising after it
         * could push back past the ceiling it just imposed. */
        eq_on_buffer(out, CHUNK);

        /* The gain is applied AT THE LAST MOMENT: it therefore covers
         * everything that goes out, whatever decode path came before. */
        gain_on_buffer(a, out, CHUNK);   /* EQV1(c): a volume change ramps */

        const long w = snk->write(a, out, CHUNK);
        if (w < 0) {
            /* OUT-1 - a raw xrun is counted APART (`xrun=`) by the sink, and
             * never added into `underrun=`: on a hw device it includes every
             * idle gap of a silent VM (88 in 90 s of silence in the
             * assessment's model). The chunk is lost and not played(): the next
             * refill finds the device empty, the same episode. Retrying the
             * write after a recovery is NOT done here - it changes what is
             * heard and needs its own A/B. */
            snk->on_error(a, w);
        } else if (a->gap_on && w > 0) {
            audio_gap_played(&a->gap, gap_t, (int64_t)w * 1000000 / AUDIO_SAMPLE_RATE, -1);
        }

        /* === L12 2026-08-29 — WHAT THE DRIVER HOLDS, AND NOBODY WATCHED ===
         *
         * `audio/file` was read from the ring's fill level alone, on the
         * producer's side. But the ring is not the queue: the DEVICE holds
         * audio too - ALSA is opened with 100 ms of requested latency
         * (`snd_pcm_set_params`, chosen because it was the simplest function,
         * never re-examined) - and that sits in front of the listener with no
         * counter showing it. The measurement
         * therefore published 10.2 ms at p50 for a path that holds ten times
         * that. The same kind of blind spot as the video hold, and it survived
         * the L5 instrumentation for the same reason: two instruments bracketed
         * the gap without covering it.
         *
         * The sink's `queued` returns the frames between what we have just
         * written and what is being heard. The full queue is that PLUS whatever
         * still sleeps in the ring. It is that sum which compares with the
         * console measurement (L11, `samples_submitted - played`): on both sides,
         * "how much audio is already decided but not yet heard".
         *
         * One read per 10 ms frame, on the thread that already owns the device
         * handle - querying it from the producer would put two threads on the
         * same handle. */
        if (latency_enabled()) {
            const long d = snk->queued(a);
            if (d >= 0) a->dev_queued = (size_t)d;   /* pour L13 */
            if (d >= 0) {
                pthread_mutex_lock(&a->ring_mtx);
                const size_t filled =
                    (a->ring_head + a->ring_cap - a->ring_tail) % a->ring_cap;
                pthread_mutex_unlock(&a->ring_mtx);
                latency_add(LAT_AUD_QUEUE,
                                ((int64_t)d + (int64_t)filled) * 1000000LL
                                    / AUDIO_SAMPLE_RATE);
            }
        }
    }
}

#endif

#if HAVE_RING
/* Consumes the ring and writes to ALSA - or, since OUT-3, to WASAPI. Only this
 * thread is allowed to block: it is responsible for audio output alone, and
 * its lateness penalises nobody else. It exits on `play_abort`, checked every
 * turn (~10 ms). */
static void *audio_play_thread(void *arg) {
    audio_decoder *a = (audio_decoder *)arg;
    const size_t CHUNK = 480;              /* 10 ms */
    int16_t out[480 * AUDIO_CHANNELS];

#if HAVE_WASAPI
    /* OUT-3: the same ring, the same 480-frame chunks, the same EQ-then-gain
     * sequence; only the clock differs - the engine's period event instead of
     * a blocking write. */
    win_play_loop(a, out, CHUNK);
#elif HAVE_ALSA
    /* The blocking-write shape: the device paces us. */
    blocking_play_loop(a, out, CHUNK, &ALSA_SINK);
#elif HAVE_SCEAUDIO
    blocking_play_loop(a, out, CHUNK, &VITA_SINK);
#endif
    return NULL;
}
#endif

#if HAVE_ALSA
/* === L12 2026-08-29 — THE ALSA LATENCY, MEASURED RATHER THAN COPIED ===
 *
 * 100 ms was never a choice: it is `snd_pcm_set_params`'s example value, set
 * with a comment saying it was the simplest function. Sweep of the COMPLETE
 * queue (driver included, `snd_pcm_delay` + ring), 30 s sessions against a real
 * VM:
 *
 *     asked     buffer obtained   period    queue p50 / p99
 *       5 ms         5,0 ms        1,2 ms     86,0 / 106,5
 *      10 ms        10,0 ms        2,5 ms     94,2 / 114,7
 *      20 ms        20,0 ms        5,0 ms     63,5 /  73,7   <-- minimum
 *      40 ms        40,0 ms       10,0 ms     90,1 / 127,0
 *     100 ms       100,0 ms       25,0 ms    131,1 / 155,6
 *
 * The curve is NOT monotonic, and that is the interesting fact: going below
 * 20 ms makes things worse. The playback thread writes in blocks of 480 frames
 * (10 ms); below two blocks of buffer, every write waits and the ring absorbs
 * the difference - the latency does not disappear, it MOVES upstream, where
 * nothing bounded it. The minimum therefore sits at twice the write block, and
 * would move with it.
 *
 * 20 ms reproduced identically over twelve sessions (63.5 / 73.7 to the tenth).
 * It is the default. Gain over the old value: -76 ms.
 *
 * OUT-1 2026-09-11 - SUPERSEDED as a measurement: this paragraph also said
 * 20 ms "produced NO underrun (`writei FAIL` = 0), no more than any other
 * point". That line cannot fire for an underrun: snd_pcm_recover returns 0 for
 * -EPIPE, -ESTRPIPE and -EINTR and, with silent=1, prints nothing (checked on
 * libasound 1.2.14), so `writei FAIL` only ever meant that the RECOVERY failed.
 * Whether 20 ms underruns was never measured; `underrun=` and `xrun=` (the
 * session bilan, audio_play_thread) answer it now. Do not require a nonzero
 * count at 5 ms either: the table shows a DEEPER queue there (86.0 ms p50).
 *
 * DESKTOP path only - the console goes out through `audout` and is not
 * concerned. */
static unsigned audio_alsa_latency_us(void)
{
    static int g_us = -1;
    if (g_us < 0) {
        const char *e = getenv("SHADOW_ALSA_LATENCE_MS");
        int ms = e ? atoi(e) : 20;
        if (ms < 5)   ms = 5;
        if (ms > 500) ms = 500;
        g_us = ms * 1000;
    }
    return (unsigned)g_us;
}
#endif

audio_decoder *audio_decoder_create(void) {
    audio_volume_apply_env();
    int rc = 0;
    audio_decoder *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->opus = opus_decoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, &rc);
    if (rc != OPUS_OK || !a->opus) {
        alog("audio: opus_decoder_create FAIL rc=%d (%s)", rc,
             opus_strerror(rc));
        free(a); return NULL;
    }
    /* The Opus decoder is always created - it is the default codec. The message
     * said so without nuance, which suggested the session WAS in Opus when the
     * codec is negotiated and may be FLAC: we
     * dit maintenant. */
    alog("audio: Opus decoder ready (sr=%d ch=%d) - the real codec is negotiated, "
         "FLAC is recognised on the first frame", AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);

    /* OUT-1 - resolved here, on the session thread, before any output thread
     * exists: the playback threads only read them. */
    a->gap_on = sousflux_on();
    diag_audio_mute_resolve();
    a->diag_t0_ms = gap_now_us() / 1000;

#if HAVE_AUDOUT
    /* A5: the hardware decoder is opt-in. The software decoder is still built
     * whatever happens - it is the fallback, and it only costs one allocation. */
    {
        const char *e = getenv("SHADOW_HWOPUS");
        if (e && atoi(e)) {
            Result hr = hwopusDecoderInitialize(&a->hwopus,
                                                AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);
            if (R_SUCCEEDED(hr)) {
                a->hwopus_ready = true;
                alog("audio: HARDWARE Opus decoder active (hwopus)");
            } else {
                alog("audio: hwopusDecoderInitialize rc=0x%x - staying in software", hr);
            }
        }
    }

    Result r = audoutInitialize();
    if (R_FAILED(r)) {
        alog("audio: audoutInitialize FAIL 0x%x", r);
        /* === AUDC-1 2026-09-11 - THROUGH THE DESTRUCTOR, NOT free() ===
         * With SHADOW_HWOPUS=1 the hwopus session opened just above leaked on
         * this path and the next one - its service session and its transfer
         * memory, one per failed create, the handle leak KB §7 forbids; the
         * reconnect collision made this path a routine one. The destructor
         * tolerates a partial decoder: audout_init and audout_started are set
         * only after success, and a failed audoutInitialize has already been
         * undone by libnx's guard. Bench (this file built for __SWITCH__
         * against a mock libnx, three failure causes): 1 hwopus session left
         * open per failed create before, 0 after. */
        audio_decoder_destroy(a); return NULL;
    }
    a->audout_init = true;
    r = audoutStartAudioOut();
    if (R_FAILED(r)) {
        alog("audio: audoutStartAudioOut FAIL 0x%x", r);
        audio_decoder_destroy(a); return NULL;   /* AUDC-1: exits audout and hwopus */
    }
    a->audout_started = true;
    u32 sr = audoutGetSampleRate();
    u32 ch = audoutGetChannelCount();
    alog("audio: audout sr=%u ch=%u", sr, ch);
    // Allocate the PCM buffers (page-aligned). All initially "free".
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        a->bufmem[i] = aligned_alloc(0x1000, AUDIO_BUFFER_BYTES_ALIGNED);
        if (!a->bufmem[i]) {
            alog("audio: aligned_alloc FAIL i=%d", i);
            audio_decoder_destroy(a); return NULL;
        }
        memset(a->bufmem[i], 0, AUDIO_BUFFER_BYTES_ALIGNED);
        a->aob[i].next = NULL;
        a->aob[i].buffer = a->bufmem[i];
        a->aob[i].buffer_size = AUDIO_BUFFER_BYTES_ALIGNED;
        a->aob[i].data_size = 0;
        a->aob[i].data_offset = 0;
        a->buf_free[i] = true;
    }
    alog("audio: %d buffers x %u bytes allocated", AUDIO_NUM_BUFFERS,
         (unsigned)AUDIO_BUFFER_BYTES_ALIGNED);
#endif

#if HAVE_ALSA
    /* Open the default ALSA device for playback. "default" = pulseaudio on most
     * modern distributions, falling back to hw:0 otherwise. */
    /* Blocking open: it is the dedicated playback thread that will wait on the
     * sound card, never the receive thread (see the ring, above). */
    int err = snd_pcm_open(&a->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        alog("audio: snd_pcm_open FAIL: %s", snd_strerror(err));
        opus_decoder_destroy(a->opus); free(a); return NULL;
    }
    /* Set the hardware params: 48 kHz s16le interleaved stereo, 100 ms latency.
     * snd_pcm_set_params is the simplest one-shot helper, and enough for live
     * streaming (no need for the full HW config tuning). */
    err = snd_pcm_set_params(a->pcm,
                              SND_PCM_FORMAT_S16_LE,
                              SND_PCM_ACCESS_RW_INTERLEAVED,
                              AUDIO_CHANNELS,
                              AUDIO_SAMPLE_RATE,
                              1,                    /* allow soft resample */
                              (unsigned)audio_alsa_latency_us());
    if (err < 0) {
        alog("audio: snd_pcm_set_params FAIL: %s", snd_strerror(err));
        snd_pcm_close(a->pcm); a->pcm = NULL;
        opus_decoder_destroy(a->opus); free(a); return NULL;
    }
    /* === L12b — SAY WHAT WE GOT, NOT WHAT WE ASKED FOR ===
     * This line used to hard-code "100ms latency". It stayed true only as long as
     * the value did; as soon as the latency became configurable, it started
     * lying - and it is precisely the kind of line one then reasons from. Above
     * all: `snd_pcm_set_params` ROUNDS to the hardware's granularity. Asking for
     * 20 ms and getting 60 is normal, invisible, and would be enough to make a
     * whole campaign conclude the wrong way. So we read back the parameters
     * actually set. */
    {
        snd_pcm_uframes_t buf = 0, per = 0;
        if (snd_pcm_get_params(a->pcm, &buf, &per) == 0)
            alog("audio: ALSA opened - asked %u ms, got a %.1f ms buffer "
                 "(%lu frames), period %.1f ms (%lu frames)",
                 audio_alsa_latency_us() / 1000,
                 (double)buf * 1000.0 / AUDIO_SAMPLE_RATE, (unsigned long)buf,
                 (double)per * 1000.0 / AUDIO_SAMPLE_RATE, (unsigned long)per);
        else
            alog("audio: ALSA opened - asked %u ms, real parameters unreadable",
                 audio_alsa_latency_us() / 1000);
    }

    /* A half-second ring: enough to absorb the UDP bursts without adding
     * perceptible latency (we never play more than what has arrived). */
    a->ring_cap = AUDIO_SAMPLE_RATE / 2;
    a->ring = (int16_t *)calloc(a->ring_cap * AUDIO_CHANNELS, sizeof(int16_t));
    if (!a->ring) {
        alog("audio: allocation de l'anneau FAIL");
        snd_pcm_close(a->pcm); a->pcm = NULL;
        opus_decoder_destroy(a->opus); free(a); return NULL;
    }
    pthread_mutex_init(&a->ring_mtx, NULL);
    pthread_cond_init(&a->ring_cv, NULL);
    if (pthread_create(&a->play_thread, NULL, audio_play_thread, a) == 0) {
        a->play_running = true;
    } else {
        alog("audio: playback thread creation FAILED - audio output disabled");
    }
#endif
#if HAVE_WASAPI
    /* === OUT-3 2026-09-11 - THE RING HERE, THE DEVICE ON ITS OWN THREAD ===
     * The same half-second ring and playback thread as ALSA, but NO device
     * here: the thread opens it (win_play_loop), because an open took 83 ms to
     * 1.2 s on the dev machine and this runs on the session thread. Nothing
     * here can make create fail: without a ring or a thread the decoder is
     * decode-only, as the Windows client always was. */
    if (win_audio_on()) {
        a->ring_cap = AUDIO_SAMPLE_RATE / 2;
        a->ring = (int16_t *)calloc(a->ring_cap * AUDIO_CHANNELS, sizeof(int16_t));
        if (!a->ring) {
            alog("audio: allocation de l'anneau FAIL — decodage seul");
        } else {
            pthread_mutex_init(&a->ring_mtx, NULL);
            pthread_cond_init(&a->ring_cv, NULL);
            if (pthread_create(&a->play_thread, NULL, audio_play_thread, a) == 0) {
                a->play_running = true;
            } else {
                alog("audio: playback thread creation FAILED - audio output disabled");
                pthread_mutex_destroy(&a->ring_mtx);
                pthread_cond_destroy(&a->ring_cv);
                free(a->ring);
                a->ring = NULL;
            }
        }
    } else {
        alog("audio: Windows output disabled (SHADOW_WIN_AUDIO=0) - decode only, "
             "as before 2026-09-11");
    }
#endif
#if HAVE_SCEAUDIO
    /* PS Vita. Opened HERE and not on the play thread, unlike WASAPI: there is
     * no COM apartment and no endpoint enumeration behind
     * `sceAudioOutOpenPort`, so it cannot cost the session thread the 83 ms to
     * 1.2 s that justified deferring the Windows open. Opening here also means
     * a failure is reported at creation rather than discovered later.
     *
     * A failure is NOT fatal: without a port the decoder stays decode-only,
     * exactly as Windows was before OUT-3 and as an ALSA-less build is now. */
    {
        audio_out_vita_info vinf;
        a->vita = audio_out_vita_open(AUDIO_OUT_VITA_MAIN, AUDIO_SAMPLE_RATE,
                                          AUDIO_CHANNELS, &vinf);
        if (!a->vita) {
            alog("audio: SceAudioOut unavailable - decode only");
        } else {
            alog("audio: SceAudioOut opened - %d Hz, %d ch, grain %d frames (%.2f ms)",
                 vinf.rate, vinf.channels, vinf.grain, vinf.grain_us / 1000.0);
            a->ring_cap = AUDIO_SAMPLE_RATE / 2;
            a->ring = (int16_t *)calloc(a->ring_cap * AUDIO_CHANNELS, sizeof(int16_t));
            if (!a->ring) {
                alog("audio: ring allocation FAILED - decode only");
                audio_out_vita_close(a->vita); a->vita = NULL;
            } else {
                pthread_mutex_init(&a->ring_mtx, NULL);
                pthread_cond_init(&a->ring_cv, NULL);
                if (pthread_create(&a->play_thread, NULL, audio_play_thread, a) == 0) {
                    a->play_running = true;
                } else {
                    alog("audio: playback thread creation FAILED - audio output disabled");
                    pthread_mutex_destroy(&a->ring_mtx);
                    pthread_cond_destroy(&a->ring_cv);
                    free(a->ring); a->ring = NULL;
                    audio_out_vita_close(a->vita); a->vita = NULL;
                }
            }
        }
    }
#endif

    a->created = true;   /* W1 step 0: a whole decoder - see audio_log_session_summary */
    return a;
}

/* === W1 step 0 2026-09-11 - ONE AUDIO LINE PER SESSION ===
 * Every audio A/B of the W1-W2 waves reads its verdict here. The counters
 * existed, but only the 500-frame reports and the panel showed them: a session
 * shorter than 500 frames - or one that decoded nothing, which is exactly the
 * FLAC silence of DEC-1 - left no trace of what it received, decoded, refused
 * or failed. Emitted once per decoder (the glue creates and destroys one per
 * session), before anything is freed; the session thread has stopped feeding
 * the decoder by then, so nothing races. `poussees` and `deborde` describe an
 * output only audout and ALSA have: `n/a` elsewhere (the ING-1 precedent),
 * never a zero that would read as a measurement. Under audout `deborde`
 * includes `buf_drop_count`: the same sum as audio_decoder_get_stats(), the
 * panel's. New line, new keys; the existing lines are unchanged.
 *
 * OUT-1 2026-09-11 - ` underrun=N (X ms) xrun=M` ends the line where this
 * session's output was measured (gap_measured): audout, ALSA, and WASAPI once
 * a device played. Absent - never 0 - elsewhere, and under
 * SHADOW_AUD_SOUSFLUX=0. The playback thread is joined before this runs
 * (audio_decoder_destroy), so its counters are final here. */
static void audio_log_session_summary(audio_decoder *a)
{
    /* Only a decoder that finished its create had a session: the failure
     * paths of audio_decoder_create may free a half-built one through
     * audio_decoder_destroy, and it has nothing to report. A whole decoder
     * that received nothing IS reported - `recues=0` is exactly the session
     * worth a line - so the guard is `created`, never a counter. */
    if (!a->created) return;
    audio_stats_t st;
    audio_decoder_get_stats(a, &st);
    char pushed[16], over[16];
#if HAVE_AUDOUT || HAVE_ALSA
    snprintf(pushed, sizeof pushed, "%u", (unsigned)st.buffers_pushed);
    snprintf(over,   sizeof over,   "%u", (unsigned)st.buffers_dropped);
#elif HAVE_WASAPI
    /* OUT-3: the widened guard. The ring is an output only in a session whose
     * device became ready at least once; SHADOW_WIN_AUDIO=0, or a device that
     * never opened, is still decode-only - `n/a`, never a zero that would read
     * as a measurement. */
    if (a->ring && a->win_ever_ready) {
        snprintf(pushed, sizeof pushed, "%u", (unsigned)st.buffers_pushed);
        snprintf(over,   sizeof over,   "%u", (unsigned)st.buffers_dropped);
    } else {
        snprintf(pushed, sizeof pushed, "n/a");
        snprintf(over,   sizeof over,   "n/a");
    }
#else
    snprintf(pushed, sizeof pushed, "n/a");
    snprintf(over,   sizeof over,   "n/a");
#endif
    char gk[64];
    gap_keys(a, gk, sizeof gk);   /* OUT-1: "" where no output was measured */
    alog("audio: session summary: codec=%s received=%u decoded=%u dropped=%u "
         "erreurs=%u poussees=%s overflow=%s%s",
         audio_decoder_codec_name(a), (unsigned)st.packets_received,
         (unsigned)st.frames_decoded, (unsigned)st.invalid_packets,
         (unsigned)st.decode_errors, pushed, over, gk);
}

void audio_decoder_destroy(audio_decoder *a) {
    if (!a) return;
#if HAVE_RING
    /* The playback thread first: it touches the device and the ring. OUT-3:
     * on Windows it also closes the device itself - WASAPI objects belong to
     * the thread that opened them - so the join covers the teardown.
     * OUT-1 2026-09-11: joined BEFORE the session bilan, which reads the
     * underrun counters this thread writes, so they are final there. The
     * bilan reads nothing the join frees. */
    if (a->play_running) {
        a->play_abort = true;
        pthread_mutex_lock(&a->ring_mtx);
        pthread_cond_broadcast(&a->ring_cv);
        pthread_mutex_unlock(&a->ring_mtx);
        pthread_join(a->play_thread, NULL);
        a->play_running = false;
        a->play_ran = true;   /* OUT-1: this session had an output thread */
        pthread_mutex_destroy(&a->ring_mtx);
        pthread_cond_destroy(&a->ring_cv);
    }
#endif
    audio_log_session_summary(a);   /* W1 step 0 - before anything is freed */
#if HAVE_AUDOUT
    if (a->hwopus_ready) { hwopusDecoderExit(&a->hwopus); a->hwopus_ready = false; }
#endif
    if (a->opus) opus_decoder_destroy(a->opus);
#if HAVE_AUDOUT
    if (a->audout_started) audoutStopAudioOut();
    if (a->audout_init)    audoutExit();
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        free(a->bufmem[i]);
    }
#endif
#if HAVE_RING
    /* The playback thread was joined first thing (OUT-1, above). */
#if HAVE_WASAPI
    win_log_session(a);   /* OUT-3 - after the join: the counters are final */
#endif
    free(a->ring);
#endif
#if HAVE_ALSA
    if (a->pcm) {
        snd_pcm_drain(a->pcm);
        snd_pcm_close(a->pcm);
    }
#endif
#if HAVE_SCEAUDIO
    /* After the play thread has been joined above: `sceAudioOutOutput` blocks,
     * so releasing the port under a thread still inside it would be a use after
     * free. There is no drain to ask for - the device plays out what it holds
     * and the wrapper drops at most one grain, which the header explains. */
    if (a->vita) { audio_out_vita_close(a->vita); a->vita = NULL; }
#endif
    if (a->flac)     avcodec_free_context(&a->flac);
    if (a->flac_pkt) av_packet_free(&a->flac_pkt);
    if (a->flac_frm) av_frame_free(&a->flac_frm);
    free(a);
}

/* AUD3 2026-08-21 - the header offset before the Opus data.
 *
 * The `:base+30` channel's frames start with a small header (the same leading
 * byte as the cursor positions). We do not yet know whether it is 0 or 8 bytes,
 * and guessing would cost a run per mistake: we try both on the first frames,
 * keep the one that decodes, and say so. `SHADOW_AUDIO_HDR=<n>` forces the
 * value. */
static int audio_payload_offset(audio_decoder *a, const uint8_t *payload, size_t len);

/* Drops interleaved 16-bit PCM into the playback ring. Extracted from
 * `audio_decoder_feed`'s body on 2026-08-27: the FLAC path needs it, and copying a
 * mutex-protected insertion would let the two diverge the day one of them
 * changes. */
#if HAVE_AUDOUT
/* === L5 - THE TWO audout QUEUE MILESTONES, WRITTEN ONCE ===
 *
 * TWO places submit a buffer to audout and two release one: the Opus path, which
 * decodes straight into the buffer, and the `audio_push_pcm` path (FLAC, and the
 * deposit into the desktop's ring). S90d cost an inaudible fix on console
 * because a truncated report had shown only two of the three output paths; the
 * remedy adopted was that the paths run the SAME sequence, written in one place.
 * These two functions are that place, for the measurement.
 *
 * Cost: two additions and one `clock_gettime` per 10 ms frame. */
static void aud_note_released(audio_decoder *a, const AudioOutBuffer *b)
{
    uint32_t samples = (uint32_t)(b->data_size / (AUDIO_CHANNELS * sizeof(int16_t)));
    a->samples_in_flight = (a->samples_in_flight > samples) ? (a->samples_in_flight - samples) : 0;
}

static void aud_note_submitted(audio_decoder *a, int samples)
{
    a->samples_in_flight += (uint32_t)samples;
    a->samples_submitted += (uint64_t)samples;
    if (!latency_enabled()) return;

    /* === L11 2026-08-29 — MEASURE THE QUEUE, NOT OUR OWN BOOKKEEPING ===
     *
     * This stage read from `samples_in_flight`, our own count of buffers not yet
     * returned. It published 41.0 ms at p50, at p90 AND at p99 - a perfect flat
     * line, which does not describe a queue but a STRUCTURAL COUNTER: four
     * buffers of 10 ms, always. The driver, queried in parallel, said 27.7 to
     * 33 ms on the same sessions.
     *
     * The gap is not an arithmetic error, it is `audout`'s asynchronous return
     * delay: we still count a buffer the hardware has already played. In other
     * words the stage overestimated by about one buffer, and a fix sized on it
     * would have targeted ten milliseconds that do not exist.
     *
     * So we read the driver, which alone knows. Every TEN frames rather than
     * every 500: ~100 samples per report window instead of two, enough to
     * sustain percentiles, for ten IPC round trips per second - the path already
     * makes a hundred just to submit the buffers.
     *
     * `SHADOW_AUD_FILE_PILOTE=0` returns to the old measurement, the one that
     * read from ourselves. */
    static int g_queue_from_driver = -1;
    if (g_queue_from_driver < 0) {
        const char *e = getenv("SHADOW_AUD_FILE_PILOTE");
        g_queue_from_driver = e ? atoi(e) : 1;
    }
    if (!g_queue_from_driver) {
        latency_add(LAT_AUD_QUEUE,
                        (int64_t)a->samples_in_flight * 1000000LL / AUDIO_SAMPLE_RATE);
    } else if ((a->stats.buffers_pushed % 10) == 0) {
        u64 played = 0;
        if (R_SUCCEEDED(audoutGetAudioOutPlayedSampleCount(&played))
            && a->samples_submitted >= played) {
            latency_add(LAT_AUD_QUEUE,
                            (int64_t)(a->samples_submitted - played) * 1000000LL
                                / AUDIO_SAMPLE_RATE);
        }
    }

    /* The plain-text report keeps its 500-frame cadence: it exists to compare
     * the TWO counts, and it was that comparison which revealed the bias.
     * Keeping it means being able to redo the observation rather than believe
     * it. */
    if ((a->stats.buffers_pushed % 500) == 0) {
        u64 played = 0;
        if (R_SUCCEEDED(audoutGetAudioOutPlayedSampleCount(&played))
            && a->samples_submitted >= played) {
            char gk[64];
            gap_keys(a, gk, sizeof gk);   /* OUT-1: the underrun keys at the end */
            alog("[L5] audio queue: %u samples ours (%.1f ms), %llu samples in total "
                 "including the driver (%.1f ms), dropped=%u%s",
                 a->samples_in_flight, (double)a->samples_in_flight * 1000.0 / AUDIO_SAMPLE_RATE,
                 (unsigned long long)(a->samples_submitted - played),
                 (double)(a->samples_submitted - played) * 1000.0 / AUDIO_SAMPLE_RATE,
                 a->buf_drop_count, gk);
        }
    }
}

/* === OUT-1 2026-09-11 - THE audout REFILL: ONE QUESTION TO THE DRIVER ===
 * Shared by both submit paths (the Opus decode into the buffer, and
 * audio_push_pcm), called after the reclaim loop and before the buffer counts
 * in flight. `samples_in_flight == 0` alone misses most short gaps: releases
 * are seen late (L11), our count runs about one buffer ahead of the driver,
 * and the assessment's model saw only 1 of the 4 dry episodes of an 80 ms
 * stall that way (185 of 469 under heavy jitter). So when our count says
 * AUDIO_GAP_QUERY_INFLIGHT buffers or fewer - and only when the answer can
 * count - the driver is asked once: dry if it has played everything we
 * submitted. EQUALITY, not >=: a count polluted by another user of audout
 * would read played > submitted (the L11 guard's case) and must blind the
 * counter rather than invent underruns. In the model this found every dry
 * episode, at 3 to 56 queries a second during sound and none on a silent VM;
 * the path already makes a hundred IPCs a second to submit. */
static void gap_refill_audout(audio_decoder *a, const int16_t *pcm, int samples)
{
    if (!a->gap_on || samples <= 0) return;
    const bool aud = audio_gap_audible(audio_gap_peak(pcm, (size_t)samples * AUDIO_CHANNELS));
    int in_flight = 0;
    for (int j = 0; j < AUDIO_NUM_BUFFERS; j++) if (!a->buf_free[j]) in_flight++;
    bool dry = false;
    if (in_flight <= AUDIO_GAP_QUERY_INFLIGHT && audio_gap_query_useful(&a->gap, aud)) {
        u64 played = 0;
        dry = R_SUCCEEDED(audoutGetAudioOutPlayedSampleCount(&played))
              && played == a->samples_submitted;
    }
    a->gap_t_us = gap_now_us();
    gap_refill(a, a->gap_t_us, dry, aud);
}

/* The buffer was appended: the output accepted this frame. */
static void gap_played_audout(audio_decoder *a, int samples)
{
    if (a->gap_on)
        audio_gap_played(&a->gap, a->gap_t_us, (int64_t)samples * 1000000 / AUDIO_SAMPLE_RATE, -1);
}
#endif

#if HAVE_RING
/* === OUT-5 2026-09-11 - A TRIM OF THE RING IS A SPLICE: FADE ACROSS IT ===
 *
 * Both places that move `ring_tail` forward - the ring-full drop and the L13
 * catch-up - make the playback thread's next chunk start at an unrelated
 * sample: a step in the waveform, heard as a click. Offline (verify-next
 * out5_click.c: the ring and L13 arithmetic, a 100 ms hole then a 10-frame
 * burst), the hard cut jumps 17.5 times the largest natural step of a 440 Hz
 * tone (+38.5 dB of click energy), 57 times on an 80 Hz bass (+63.5 dB) and
 * 17.8 times on music-like material (+49.3 dB); a 5 ms crossfade brings them
 * to x1.0 (+0.5 dB), x1.6 (+21.5 dB) and +2.2 dB. A cut is clean only by
 * phase coincidence (1 kHz: the 1920 dropped samples are exactly 40 periods).
 *
 * The first N samples after the new tail are blended with the N samples that
 * WOULD have played next (from the old tail), weight (i+1)/(N+1) on the new
 * side: the output continues what was just played and slides into the new
 * position. The latency is unchanged - the same new tail, the same amount
 * dropped - for 2N multiply-adds per channel and per event. Called under
 * `ring_mtx`, once the tail has moved and before the dropped region can be
 * rewritten. Walked from the END: a ring-full drop can be shorter than N, and
 * then the old and new regions overlap; backwards, every old sample is read
 * before it is rewritten. Integer arithmetic: equal inputs give the input.
 *
 * SHADOW_AUDIO_XFADE_MS - default 5 (240 samples; L13's 30 ms target always
 * leaves 1440 after the cut); 0 restores the hard cut exactly; clamped to 20.
 * Resolved at the first trim (under the lock, as L13's own toggle is). The
 * ring serves ALSA and, since OUT-3, WASAPI. */
static int g_xfade_ms = -1;

static void ring_splice_xfade(audio_decoder *a, size_t old_tail, size_t new_tail)
{
    if (g_xfade_ms < 0) {
        const char *e = getenv("SHADOW_AUDIO_XFADE_MS");
        int ms = e ? atoi(e) : 5;
        if (ms < 0)  ms = 0;
        if (ms > 20) ms = 20;
        g_xfade_ms = ms;
    }
    size_t n = (size_t)g_xfade_ms * AUDIO_SAMPLE_RATE / 1000;
    const size_t left = (a->ring_head + a->ring_cap - new_tail) % a->ring_cap;
    if (n > left) n = left;
    for (size_t i = n; i-- > 0; ) {
        const size_t so = ((old_tail + i) % a->ring_cap) * AUDIO_CHANNELS;
        const size_t sn = ((new_tail + i) % a->ring_cap) * AUDIO_CHANNELS;
        const int32_t w_new = (int32_t)(i + 1), w_old = (int32_t)(n - i);
        for (int c = 0; c < AUDIO_CHANNELS; c++)
            a->ring[sn + c] = (int16_t)(((int32_t)a->ring[so + c] * w_old
                                         + (int32_t)a->ring[sn + c] * w_new)
                                        / (int32_t)(n + 1));
    }
}
#endif

static void audio_push_pcm(audio_decoder *a, const int16_t *pcm, int samples)
{
    /* OUT-1 - SHADOW_DIAG_AUDIO_MUTE_*: a decoded frame thrown away here
     * reaches no output, on any platform. */
    if (samples > 0 && diag_audio_muted(a, samples)) return;
#if HAVE_AUDOUT
    /* === K14b 2026-08-28 — THE CONSOLE DOES NOT GO THROUGH ALSA ===
     *
     * This function only had an ALSA branch. On Switch the output goes through
     * `audout`, and the FLAC path - which hands over here - therefore pushed
     * NOTHING: the panel showed "FLAC 48 kHz" and 100 frames/s, everything was
     * decoded, and there was no sound at all. A silence with not a single counter
     * in error, once again.
     *
     * The Opus path, by contrast, decodes DIRECTLY into audout's buffer and does
     * not come through here. So we repeat the same buffer handling, with one
     * extra copy - two kilobytes per 10 ms frame, negligible against an IPC round
     * trip. */
    if (!a->audout_started || samples <= 0) return;

    /* Reclaim ALL the returned buffers: without this loop none is ever freed and
     * everything ends up dropped. */
    AudioOutBuffer *released = NULL;
    u32 released_count = 0;
    while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &released_count))
           && released_count > 0) {
        for (int j = 0; j < AUDIO_NUM_BUFFERS; j++) {
            if (released == &a->aob[j]) { a->buf_free[j] = true;
                                          aud_note_released(a, released); break; }   /* L5 */
        }
        released = NULL; released_count = 0;
    }

    int idx = -1;
    for (int j = 0; j < AUDIO_NUM_BUFFERS; j++)
        if (a->buf_free[j]) { idx = j; break; }
    if (idx < 0) {
        /* All busy: we DROP rather than overwrite a buffer being played - that
         * is what produced the crackle. */
        a->buf_drop_count++;
        if (a->buf_drop_count <= 5 || (a->buf_drop_count % 100 == 0))
            alog("audio: frame dropped - no free buffer (%u)", a->buf_drop_count);
        return;
    }

    const size_t nbytes = (size_t)samples * AUDIO_CHANNELS * sizeof(int16_t);
    if (nbytes > AUDIO_BUFFER_BYTES) return;
    gap_refill_audout(a, pcm, samples);   /* OUT-1: on `pcm`, before the copy, the EQ and the gain */
    memcpy(a->bufmem[idx], pcm, nbytes);
    /* S90 - equaliser then volume, in that order: see the ALSA path. */
    eq_on_buffer((int16_t *)a->bufmem[idx], (size_t)samples);

    /* Same rule as on the ALSA path: the volume is the last gesture before the
     * buffer is handed to the hardware. */
    gain_on_buffer(a, (int16_t *)a->bufmem[idx], (size_t)samples);   /* EQV1(c) */
    a->aob[idx].data_size = (u64)nbytes;
    a->aob[idx].data_offset = 0;
    a->buf_free[idx] = false;   /* busy BEFORE the send, released by audout */
    Result r = audoutAppendAudioOutBuffer(&a->aob[idx]);
    if (R_FAILED(r)) {
        a->stats.decode_errors++;
        a->buf_free[idx] = true;
        if (a->stats.decode_errors < 5)
            alog("audio: audoutAppendAudioOutBuffer rc=0x%x", r);
        return;
    }
    a->stats.buffers_pushed++;
    aud_note_submitted(a, samples);   /* L5 */
    gap_played_audout(a, samples);    /* OUT-1 */
    return;
#endif
#if HAVE_RING
    if (!a->ring || samples <= 0) return;
#if HAVE_WASAPI
    /* OUT-3 - the ring is fed only while a device can drain it. While the
     * playback thread opens the device, reopens it, or after it failed, a push
     * would only fill the ring and come out as L13 trims and overflows; those
     * samples are decode-only, counted for the session line. */
    if (a->win_state != WIN_OUT_READY) { a->win_discarded += (uint64_t)samples; return; }
#endif
    pthread_mutex_lock(&a->ring_mtx);
    size_t free_room = (a->ring_tail + a->ring_cap - a->ring_head - 1) % a->ring_cap;
    if ((size_t)samples > free_room) {
        /* Ring full: we drop the OLDEST data rather than the newest. A listener
         * prefers a brief jump to the present over a delay that accumulates and
         * never catches up. OUT-5: the jump is faded, see ring_splice_xfade. */
        size_t drop = (size_t)samples - free_room;
        const size_t old_tail = a->ring_tail;
        a->ring_tail = (a->ring_tail + drop) % a->ring_cap;
        ring_splice_xfade(a, old_tail, a->ring_tail);
        a->stats.buffers_dropped++;
    }
    for (int i = 0; i < samples; i++) {
        size_t dst = ((a->ring_head + (size_t)i) % a->ring_cap) * AUDIO_CHANNELS;
        a->ring[dst]     = pcm[i * AUDIO_CHANNELS];
        a->ring[dst + 1] = pcm[i * AUDIO_CHANNELS + 1];
    }
    a->ring_head = (a->ring_head + (size_t)samples) % a->ring_cap;

    /* === L13 2026-08-29 — THE RING NEVER COMES BACK DOWN ON ITS OWN ===
     *
     * The observation L5 recorded here - "nothing brings it back down, so its
     * fill level climbs and stays high" - was right, and it was treated as a
     * desktop quirk worth mentioning. It is a DEFECT, and the L12 measurement
     * quantified it.
     *
     * The playback thread consumes 10 ms per turn and blocks in
     * `snd_pcm_writei`: it is paced by REAL TIME, no faster and no slower. If the
     * producer gets ahead just once - a burst after a network hiccup, the server
     * catching up - the ring climbs by that much and never comes back down:
     * nothing consumes faster than time. The lead taken in three seconds is paid
     * for until the end of the session.
     *
     * That explains the campaign's variance: four sessions of the same binary
     * returned 96, 139, 152 and 310 ms of queue. The 310 ms run had no dropped
     * frame - nothing was trimmed, the ring had climbed early and stayed high.
     * The only existing guard fires at 500 ms: set so far out that it guards
     * nothing. (OUT-1 2026-09-11: this paragraph also said that run had no
     * ALSA underrun, "`writei FAIL` = 0" - a line that cannot fire for a
     * recovered one. The claim is withdrawn, not refuted: L13 rests on the
     * ring level, not on it. `underrun=` and `xrun=` measure underruns now.)
     *
     * So we bring the queue back towards a target as soon as it exceeds a
     * ceiling, by dropping the OLDEST samples. The jolt is paid once, in a few
     * milliseconds of sound, instead of being carried by the whole session. For
     * gaming, audio a third of a second late is worse than a glitch.
     *
     * `SHADOW_AUDIO_MAX_MS=0` disables the catch-up.
     *
     * OUT-3: the WASAPI loop is paced by the engine the same way, so the same
     * catch-up applies there. OUT-5: the trim is faded, not cut. */
    {
        static int g_max_ms = -1, g_target_ms = -1;
        if (g_max_ms < 0) {
            const char *e = getenv("SHADOW_AUDIO_MAX_MS");
            g_max_ms = e ? atoi(e) : 60;
            if (g_max_ms > 0 && g_max_ms < 20) g_max_ms = 20;
            g_target_ms = g_max_ms / 2;
        }
        if (g_max_ms > 0) {
            const size_t cap_samples = (size_t)g_max_ms * AUDIO_SAMPLE_RATE / 1000;
            const size_t filled =
                (a->ring_head + a->ring_cap - a->ring_tail) % a->ring_cap;
            /* === THE CEILING COVERS WHAT THE LISTENER HEARS =============
             *
             * It counted only the RING. But `[L5] audio/file` -- the
             * measurement that states the real latency -- is ring PLUS device
             * queue, and on PS Vita that second half weighs: 56 ms measured
             * against 27.6 on Switch, for a ring already capped at 60. A
             * ceiling that ignores half the queue does not cap the latency, it
             * caps a number.
             *
             * `dev_queued` comes from the play loop, which asks the sink on
             * every turn. At zero -- no sink, or a sink that cannot answer --
             * the behaviour is exactly what it was before. */
            const size_t total = filled + a->dev_queued;
            /* The SPLIT, not just the total: the ring is the only half that can
             * be trimmed, so setting the ceiling without knowing what the
             * device holds means aiming at a target that is sometimes
             * unreachable. One line every 500 frames is enough to
             * characterise it. */
            {
                static unsigned n_split = 0;
                if ((n_split++ % 500) == 0)
                    alog("[L13] repartition : anneau %u ms + peripherique %u ms = %u ms",
                         (unsigned)(filled * 1000 / AUDIO_SAMPLE_RATE),
                         (unsigned)(a->dev_queued * 1000 / AUDIO_SAMPLE_RATE),
                         (unsigned)(total * 1000 / AUDIO_SAMPLE_RATE));
            }
            if (total > cap_samples) {
                const size_t target = (size_t)g_target_ms * AUDIO_SAMPLE_RATE / 1000;
                /* Only the ring can be trimmed: samples already handed to the
                 * device are gone. If the hardware queue alone exceeds the
                 * target, empty the ring as far as it goes and do not
                 * manufacture a negative trim. */
                size_t want_ring = (total - target > filled) ? filled
                                                               : (total - target);
                /* === A FLOOR, BECAUSE THE FIRST ATTEMPT STARVED ==============
                 *
                 * Counting the device queue in the ceiling did drop
                 * `audio/file` from 56.5 to 38-42 ms -- but L13 started
                 * trimming 50 times a minute and two 10 ms underruns showed
                 * up. The cause is arithmetic: the device already holds 10 to
                 * 20 ms, so aiming at a 30 ms TOTAL left almost nothing in the
                 * ring, and the slightest irregularity from the producer
                 * emptied it.
                 *
                 * So the ring keeps enough to cover a late producer.
                 * `SHADOW_AUDIO_FLOOR_MS` sets it.
                 *
                 * 20 to begin with, measured at 333 MHz. Now that the console
                 * runs at its real clocks (444 MHz, see power_profile.c) the
                 * producer is late less often, and 20 was costing latency for
                 * nothing. A/B on console, `[L5] audio/file`:
                 *
                 *     floor 20 -> 33-48 ms
                 *     floor 16 -> 25-29 ms   <- the default
                 *     floor 12 -> 21-24 ms
                 *
                 * 12 works and goes below the Switch (27.6 ms). We keep 16
                 * because the underrun counts across the three arms are
                 * dominated by NETWORK variance rather than by the floor -- 12
                 * showed FEWER of them than 16 on those samples, which says
                 * the measurement does not yet separate the two. A margin you
                 * cannot justify removing is a margin you keep. `=12` to take
                 * it back.
                 *
                 * At 0 the starving behaviour returns: a revert path, not a
                 * suggestion. */
                static int g_floor_ms = -1;
                if (g_floor_ms < 0) {
                    const char *fe = getenv("SHADOW_AUDIO_FLOOR_MS");
                    g_floor_ms = fe ? atoi(fe) : 16;
                }
                const size_t floor_s = (size_t)g_floor_ms * AUDIO_SAMPLE_RATE / 1000;
                if (filled > floor_s && want_ring > filled - floor_s)
                    want_ring = filled - floor_s;
                else if (filled <= floor_s)
                    want_ring = 0;
                const size_t dropped = want_ring;
                if (dropped == 0) goto l13_done;
                const size_t old_tail = a->ring_tail;
                a->ring_tail = (a->ring_tail + dropped) % a->ring_cap;
                ring_splice_xfade(a, old_tail, a->ring_tail);   /* OUT-5 */
                a->stats.buffers_dropped++;
                /* %u and a cast rather than the size_t length modifier: since
                 * OUT-3 this line also compiles on Windows, whose format check
                 * does not know that modifier (CLAUDE.md, MinGW). The text is
                 * unchanged. */
                if (a->stats.buffers_dropped <= 5
                    || (a->stats.buffers_dropped % 50) == 0)
                    alog("[L13] rattrapage audio : file a %u ms, ramenee a %d ms "
                         "(%u echantillons jetes, %u fois)",
                         (unsigned)(total * 1000 / AUDIO_SAMPLE_RATE), g_target_ms,
                         (unsigned)dropped, a->stats.buffers_dropped);
            }
l13_done: ;
        }
    }

    /* L12 - the queue measurement moved into the playback thread, the only
     * place from which the driver can also be queried. Measuring here only saw
     * the ring, i.e. a tenth of the real queue. */
    pthread_cond_signal(&a->ring_cv);
    pthread_mutex_unlock(&a->ring_mtx);
    a->stats.buffers_pushed++;
#else
    /* With no ring, audio output goes through `audout` (Switch), which
     * returned above. The decoding still happened, which keeps the counters
     * comparable across platforms. */
    (void)a; (void)pcm; (void)samples;
#endif
}

/* Creates the FLAC decoder on the first frame. Lazy and non-fatal: if it
 * cannot be built we say so ONCE and the sound stays mute rather than bringing
 * the session down.
 *
 * DEC-5 2026-09-11 - "ONCE" WAS NOT TRUE, AND A PARTIAL ALLOCATION CRASHED.
 * After a failed avcodec_open2 the context was freed but the packet and the
 * frame were not, and nothing remembered the failure: the next frame looked
 * the decoder up again, overwrote both pointers (a leak) and logged again - at
 * 100 frames/s. A failed av_packet_alloc returned with `flac` set, so the next
 * call answered "ready" and flac_decode dereferenced a NULL packet. Harness
 * (this function and flac_decode verbatim, libavcodec behind counting
 * wrappers, 1000 frames = 10 s of FLAC): open failing, 1000 open attempts,
 * 1000 log lines, 1000 AVPacket + 1000 AVFrame leaked -> 1, 1, 0; one failed
 * packet allocation, a NULL dereference -> one line, nothing leaked. Any
 * failure now frees all three objects and sets `flac_failed`, a decoder field:
 * the "no FLAC decoder" line was guarded by a per-process static, so only the
 * first session of a process could say it. The allocation failure, which
 * logged nothing, gets its own line; the three existing lines are unchanged.
 * The success path is unchanged (1000 of 1000 frames decoded, one open). No
 * toggle: only the failure branches change. */
static int flac_ready(audio_decoder *a)
{
    if (a->flac) return 1;
    if (a->flac_failed) return 0;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_FLAC);
    if (!c) {
        alog("audio: libavcodec has no FLAC decoder");
        a->flac_failed = true;
        return 0;
    }
    a->flac = avcodec_alloc_context3(c);
    a->flac_pkt = av_packet_alloc();
    a->flac_frm = av_frame_alloc();
    /* The server announces 48 kHz / 16 bits / stereo in the channel
     * announcement, and the FLAC frame repeats them in its own header anyway: we
     * set nothing here, the decoder reads the frame. */
    if (!a->flac || !a->flac_pkt || !a->flac_frm) {
        alog("audio: could not allocate the FLAC decoder");
    } else if (avcodec_open2(a->flac, c, NULL) < 0) {
        alog("audio: avcodec_open2(FLAC) a echoue");
    } else {
        alog("audio: FLAC decoder ready (high fidelity)");
        return 1;
    }
    /* All three, whichever failed: each free takes a NULL and clears the
     * pointer, so audio_decoder_destroy finds nothing left to free. */
    avcodec_free_context(&a->flac);
    av_packet_free(&a->flac_pkt);
    av_frame_free(&a->flac_frm);
    a->flac_failed = true;
    return 0;
}

/* Decodes a FLAC frame into interleaved 16-bit PCM. Returns the number of
 * samples PER CHANNEL, or -1. */
static int flac_decode(audio_decoder *a, const uint8_t *payload, size_t len,
                        int16_t *pcm, int pcm_cap_samples)
{
    if (!flac_ready(a)) return -1;
    av_packet_unref(a->flac_pkt);
    /* `av_packet_from_data` would take ownership of the buffer; here the bytes
     * belong to the caller and vanish on return. We point at them for the
     * duration of the call, which `avcodec_send_packet` allows since it copies
     * what it needs. */
    a->flac_pkt->data = (uint8_t *)payload;
    a->flac_pkt->size = (int)len;
    if (avcodec_send_packet(a->flac, a->flac_pkt) < 0) return -1;

    /* WE DRAIN THE DECODER. One packet may return SEVERAL frames, or none:
     * reading only one leaves the rest inside, from where they will come out one
     * packet late. That produces no error - just gaps, exactly the symptom we go
     * hunting for. */
    int total = 0;
    while (avcodec_receive_frame(a->flac, a->flac_frm) >= 0) {
        const AVFrame *f = a->flac_frm;
        const int n  = f->nb_samples;
        const int ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 1;
        if (n <= 0 || total + n > pcm_cap_samples) {
            av_frame_unref(a->flac_frm);
            break;   /* keep what is already decoded rather than lose it all */
        }
        /* The FLAC decoder returns S16 for 16 bits and S32 for 24 or 32; both
         * exist in a PLANAR variant. Handling all four cases costs ten lines and
         * avoids an inexplicable silence the day the server changes bit depth. */
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                const int sc = (c < ch) ? c : ch - 1;   /* mono -> duplicated */
                int32_t v = 0;
                switch (f->format) {
                    case AV_SAMPLE_FMT_S16:
                        v = ((const int16_t *)f->data[0])[i * ch + sc]; break;
                    case AV_SAMPLE_FMT_S16P:
                        v = ((const int16_t *)f->data[sc])[i]; break;
                    case AV_SAMPLE_FMT_S32:
                        v = ((const int32_t *)f->data[0])[i * ch + sc] >> 16; break;
                    case AV_SAMPLE_FMT_S32P:
                        v = ((const int32_t *)f->data[sc])[i] >> 16; break;
                    default:
                        av_frame_unref(a->flac_frm);
                        return total > 0 ? total : -1;
                }
                pcm[(total + i) * AUDIO_CHANNELS + c] = (int16_t)v;
            }
        }
        total += n;
        av_frame_unref(a->flac_frm);
    }
    return total > 0 ? total : -1;
}

const char *audio_decoder_codec_name(const audio_decoder *a)
{
    if (!a) return "?";
    return (a->codec_detected == 2) ? "FLAC" : (a->codec_detected == 1 ? "Opus" : "?");
}

void audio_decoder_feed(audio_decoder *a, const uint8_t *payload, size_t len,
                        uint32_t ts) {
    (void)ts;
    if (!a || !a->opus || !payload || len == 0) return;
    a->stats.packets_received++;

    /* === K14 — RECOGNISE FLAC BEFORE LOOKING FOR OPUS FRAMING ===
     * `audio_payload_offset` VALIDATES as Opus: it therefore fails on EVERY FLAC
     * frame, which went to the `invalid_packets` counter - the sound went mute
     * without a single error. The prefix is the same (`[0x12][seq u32 LE]`), only
     * the payload differs: a native FLAC frame starts with the `ff f8` sync.
     *
     * DEC-1 2026-09-11 - A ONE-WAY PROMOTION, NOT A DECISION TAKEN ONCE. This
     * used to decide once for the session, on the first packet. Half of that
     * stays: a router that hesitates frame by frame would eventually hand FLAC
     * to the Opus decoder, which returns NOISE, not an error - so FLAC is never
     * demoted. But deciding on the first packet let whatever reached the Opus
     * path first - the stream descriptor, from the second session of a process
     * on - lock a FLAC session to Opus for good: 0 of 200 frames decoded (see
     * streaming/audio_route.h and the S53 paragraph of audio_payload_offset).
     * A FLAC frame now promotes a session classified Opus. The sync also
     * requires the 0x12 prefix, in both arms: the legacy DC path feeds raw RTP
     * Opus here, and without the gate 3 of its 14 bench sessions were promoted
     * to FLAC and went silent (0 with it). Bench (variant Bg, with the
     * ctrl_session.c ingress filter): 100 % bit-exact FLAC in every order and
     * at every session count, 0 of 294 000 false promotions on the DC path,
     * +0.39 ns per packet. SHADOW_FLAC_LATE_DETECT=0 recognises FLAC only while
     * the codec is unknown, as before. */
    if (a->flac_late == 0) {
        const char *e = getenv("SHADOW_FLAC_LATE_DETECT");
        a->flac_late = (e ? atoi(e) : 1) ? 2 : 1;
    }
    if (aud_flac_promote(a->codec_detected, a->flac_late == 2, payload, len)) {
        const int from_opus = (a->codec_detected == 1);
        a->codec_detected = 2;
        alog("audio: FLAC detected on :base+30 - high fidelity, 48 kHz stereo");
        if (from_opus)
            alog("[DEC1] session reclassified Opus -> FLAC after %u packet(s)",
                 (unsigned)(a->stats.packets_received - 1));
    }

    if (a->codec_detected == 2) {
        if (len <= 5) { a->stats.invalid_packets++; return; }
        /* `static`, like the Opus path: 23 KB on the stack for every packet
         * would pass on a desktop and overflow a console thread, where the stacks
         * are small. This is NOT session state - the buffer is entirely rewritten
         * on every call. */
        static int16_t pcm[AUDIO_BUFFER_SAMPLES * AUDIO_CHANNELS];
        const int samples = flac_decode(a, payload + 5, len - 5,
                                         pcm, AUDIO_BUFFER_SAMPLES);
        if (samples <= 0) {
            a->stats.decode_errors++;
            if (a->stats.decode_errors <= 5)
                alog("audio: FLAC decoding failed, %zu-byte frame", len - 5);
            return;
        }
        a->stats.frames_decoded++;
        audio_push_pcm(a, pcm, samples);
        /* The Opus path logs its first five frames; the FLAC path did not, and
         * its silence made it impossible to tell "it is decoding" from "it is not
         * decoding" - exactly the question you ask when validating a new
         * feature. */
        if (a->stats.frames_decoded <= 5)
            alog("audio: FLAC frame #%u - %d samples (%zu compressed bytes)",
                 a->stats.frames_decoded, samples, len - 5);
        /* A regular report: FLAC is LOSSLESS, so its bitrate is far higher than
         * Opus's (~800-900 kbit/s against ~100). Seeing that figure climb is
         * NORMAL and not a defect; showing it stops it being read as a leak. */
        /* The report carries the three counters that say WHERE the sound is lost,
         * and it was missing: without them, "it crackles" has no lead.
         *   ecartes = frames refused before decoding (bad framing)
         *   erreurs = decoding failed
         *   deborde = ring full, samples DROPPED -> audible gaps
         * The keys stay French: they are emitted, and KB.md quotes them. */
        if ((a->stats.frames_decoded % 500) == 0) {
            /* AUD-INS-4 2026-09-11 - `overflow=` reads aud_drops(): on console
             * it read stats.buffers_dropped, which only the ring counts, and
             * printed 0 forever while the same log's [L5] dropped= carried the
             * true count. OUT-1: the underrun keys end the line where an
             * output is measured. %u and a cast for the length: this line
             * compiles on Windows too, whose format check does not know the
             * size_t modifier; the text is unchanged. */
            char gk[64];
            gap_keys(a, gk, sizeof gk);
            alog("audio: FLAC %u frames, last %d samples for %u B (ratio %.0f %%) "
                 "| ecartes=%u erreurs=%u overflow=%u%s",
                 a->stats.frames_decoded, samples, (unsigned)(len - 5),
                 100.0 * (double)(len - 5)
                       / (double)(samples * AUDIO_CHANNELS * 2),
                 a->stats.invalid_packets, a->stats.decode_errors,
                 aud_drops(a), gk);
        }
        return;
    }

    {
        int off = audio_payload_offset(a, payload, len);
        if (off < 0 || (size_t)off >= len) {
            /* === S53 2026-08-26 — A SILENCE THAT DOES NOT SAY ITS NAME ===
             *
             * We returned here without counting anything: `packets_received`
             * climbed while `frames_decoded`, `invalid_packets` and
             * `decode_errors` stayed at zero. The panel therefore showed "packets
             * are arriving" and "nothing is decoded", with no counter naming the
             * culprit - an anonymous silence, impossible to diagnose live.
             *
             * The known case that leads here: `:base+30`'s codec is NEGOTIATED,
             * not fixed (Opus on 2026-08-21, FLAC on 2026-08-26 on the same
             * channel, official telemetry "audio session granted : Flac"). A FLAC
             * frame carries the same `[0x12][seq u32 LE]` prefix as an Opus
             * frame: the offset detection fails on all of them, and the sound
             * goes mute without an error. The `opus_packet_get_nb_samples` guard
             * (AUD9) prevents the NOISE, it does not explain the silence. */
            a->stats.invalid_packets++;
            if (len >= 7) {
                static int said_flac = 0;
                if (!said_flac && payload[5] == 0xff && (payload[6] & 0xfc) == 0xf8) {
                    said_flac = 1;
                    alog("audio: the server sends FLAC, not Opus - "
                         "stream ignored (KB §3.37). The codec is negotiated at "
                         "l'ouverture de session.");
                }
            }
            return;
        }
        payload += off;
        len     -= (size_t)off;
    }

    /* === AUD9 — VALIDATE BEFORE DECODING ===
     *
     * `opus_decode` accepts badly framed packets and returns noise rather than an
     * error: that is what made us hear crackle while decoding at the wrong
     * offset, with no counter flinching. `opus_packet_get_nb_samples` is a
     * weaker test than this comment used to say (DEC-1, 2026-09-11): it reads
     * only the TOC byte, and a code-3 packet's frame count, never the frame
     * lengths - the 271-byte stream descriptor passed it at offset 5 (nb=960,
     * where `opus_packet_parse` returns -4). What it does drop is a packet whose
     * announced duration is impossible, FLAC frames read as Opus included
     * (nb=-4), before they dirty the decoder's state for the following frames. */
    {
        /* K18 - the Opus path declares itself too: without this the panel would
         * stay on "?" for Opus, and there would be no way to tell "this is Opus"
         * from "nothing has been classified yet". */
        if (a->codec_detected == 0) a->codec_detected = 1;
        int nb = opus_packet_get_nb_samples(payload, (opus_int32)len,
                                            AUDIO_SAMPLE_RATE);
        if (nb <= 0 || nb > AUDIO_BUFFER_SAMPLES) {
            a->stats.invalid_packets++;
            if (a->stats.invalid_packets <= 5)
                alog("audio: paquet ecarte (cadrage invalide, len=%zu, nb=%d)",
                     len, nb);
            return;
        }
    }

#if HAVE_AUDOUT
    if (!a->audout_started) return;

    // Reclaim ALL the buffers audout has released (there may be several if we
    // are behind). Loop until audout has nothing left to release
    // (released_count == 0).
    AudioOutBuffer *released = NULL;
    u32 released_count = 0;
    while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &released_count))
           && released_count > 0) {
        // released points at ONE buffer (the API only returns one at a time). We mark the slot free.
        for (int j = 0; j < AUDIO_NUM_BUFFERS; j++) {
            if (released == &a->aob[j]) {
                a->buf_free[j] = true;
                aud_note_released(a, released);   /* L5 */
                break;
            }
        }
        released = NULL; released_count = 0;
    }

    // Find a free buffer to decode into.
    // If all are busy (audout is behind on playback): drop the packet, to avoid
    // overwriting a buffer being played (which caused the crackle).
    int idx = -1;
    for (int j = 0; j < AUDIO_NUM_BUFFERS; j++) {
        if (a->buf_free[j]) { idx = j; break; }
    }
    if (idx < 0) {
        a->buf_drop_count++;
        if (a->buf_drop_count <= 5 || (a->buf_drop_count % 100 == 0)) {
            alog("audio: dropping packet — no free buffer (count=%u)", a->buf_drop_count);
        }
        return;
    }

    int16_t *pcm = (int16_t *)a->bufmem[idx];

    /* A5 - measures the decode cost, so software versus hardware can be settled
     * on console rather than on principle. */
    uint64_t t0 = armGetSystemTick();
    int samples;

    if (a->hwopus_ready) {
        /* The service expects `[HwopusHeader][packet]`, and both header fields
         * are BIG-ENDIAN - that is this interface's only subtlety, and forgetting
         * it makes every packet fail. */
        if (len + sizeof(HwopusHeader) > sizeof(a->hwopus_in)) {
            a->stats.decode_errors++;
            return;                       /* nonsense packet: dropped */
        }
        HwopusHeader hdr;
        hdr.size        = __builtin_bswap32((uint32_t)len);
        hdr.final_range = 0;              /* left at zero, see libnx */
        memcpy(a->hwopus_in, &hdr, sizeof hdr);
        memcpy(a->hwopus_in + sizeof hdr, payload, len);

        s32 out_bytes = 0, out_samples = 0;
        Result hr = hwopusDecodeInterleaved(&a->hwopus, &out_bytes, &out_samples,
                                            a->hwopus_in, sizeof(HwopusHeader) + len,
                                            pcm, AUDIO_BUFFER_BYTES);
        if (R_FAILED(hr)) {
            /* DEFINITIVE fallback to software: if the hardware refuses one
             * packet it will refuse the next ones, and retrying on every frame
             * would cost an IPC round trip for nothing. Same logic as the sticky
             * fallback of the video decoding (S28). */
            a->stats.decode_errors++;
            alog("audio: hwopusDecodeInterleaved rc=0x%x - switching to software for good", hr);
            hwopusDecoderExit(&a->hwopus);
            a->hwopus_ready = false;
            samples = opus_decode(a->opus, payload, (opus_int32)len,
                                  pcm, AUDIO_BUFFER_SAMPLES, 0);
        } else {
            samples = out_samples;
        }
    } else {
        samples = opus_decode(a->opus, payload, (opus_int32)len,
                              pcm, AUDIO_BUFFER_SAMPLES, 0);
    }

    a->decode_ticks += armGetSystemTick() - t0;

    if (samples < 0) {
        a->stats.decode_errors++;
        if (a->stats.decode_errors < 5) {
            alog("audio: opus_decode rc=%d (%s) len=%zu", samples,
                 opus_strerror(samples), len);
        }
        return;
    }
    a->stats.frames_decoded++;

    /* A5 - one report every 500 frames (~10 s at 20 ms/frame). That figure is
     * what will say whether the hardware is worth its IPC round trip: compare it
     * between SHADOW_HWOPUS=0 and =1 on the same scene. */
    if ((a->stats.frames_decoded % 500) == 0) {
        uint64_t hz = armGetSystemTickFreq();
        double us = hz ? (double)a->decode_ticks * 1e6 / (double)hz / 500.0 : 0.0;
        alog("audio: %s decoding - %.1f us per frame (mean over 500)",
             a->hwopus_ready ? "HARDWARE" : "software", us);
        a->decode_ticks = 0;
    }

    /* OUT-1 - SHADOW_DIAG_AUDIO_MUTE_*: a decoded frame thrown away here never
     * reaches the output; the buffer chosen above stays free. */
    if (diag_audio_muted(a, samples)) return;
    /* OUT-1 - the refill, on the DECODED PCM (software or hwopus), before the
     * EQ and the gain rewrite it and before the buffer counts in flight. */
    gap_refill_audout(a, pcm, samples);

    /* === S90d 2026-08-29 - THE THIRD OUTPUT PATH ===
     *
     * There are THREE, not two: ALSA, the deferred submission through a free
     * buffer, and this one - the Opus decode that writes straight into the
     * already-chosen `audout` buffer. S90 had only wired the first two, and this
     * is the one used on console: the equaliser was therefore perfectly inaudible
     * exactly where it was meant to work, while working fine on desktop.
     *
     * The mistake came from a truncated listing: the `grep` used to find the
     * insertion points was cut off at twenty lines and showed only two of the
     * three calls to `audio_gain_apply`. That is the counter-case
     * `feedback_verifier_les_mesures_grep` describes - a count taken from a grep
     * is wrong three times out of four in this repo.
     *
     * The structural remedy is not to grep better: it is that the three paths run
     * the SAME sequence, equaliser then volume, written in the same place in all
     * three. */
    eq_on_buffer(pcm, (size_t)samples);

    /* Same rule as on the ALSA path: the last gesture before handing the
     * tampon au materiel. */
    gain_on_buffer(a, pcm, (size_t)samples);   /* EQV1(c): a volume change ramps */

    a->aob[idx].data_size = (u64)samples * AUDIO_CHANNELS * sizeof(int16_t);
    a->aob[idx].data_offset = 0;
    a->buf_free[idx] = false;  // marked busy BEFORE the append, freed when audout finishes playing
    Result r = audoutAppendAudioOutBuffer(&a->aob[idx]);
    if (R_FAILED(r)) {
        a->stats.decode_errors++;
        a->buf_free[idx] = true;  // append failed, release it
        if (a->stats.decode_errors < 5) {
            alog("audio: audoutAppendAudioOutBuffer rc=0x%x", r);
        }
        return;
    }
    a->stats.buffers_pushed++;
    aud_note_submitted(a, samples);   /* L5 */
    gap_played_audout(a, samples);    /* OUT-1: appended - the output accepted it */
    if (a->stats.frames_decoded <= 5) {
        alog("audio: frame #%u decoded %d samples → buf %d pushed",
             a->stats.frames_decoded, samples, idx);
    }
#elif HAVE_RING
    /* Decode, then deposit into the ring. No device write here (ALSA or, since
     * OUT-3, WASAPI): the thread that receives the UDP must never wait on the
     * sound card. */
    static int16_t pcm[AUDIO_BUFFER_SAMPLES * AUDIO_CHANNELS];
    int samples = opus_decode(a->opus, payload, (opus_int32)len,
                               pcm, AUDIO_BUFFER_SAMPLES, 0);
    if (samples < 0) {
        a->stats.decode_errors++;
        /* %u and a cast: this line compiles on Windows too since OUT-3. */
        if (a->stats.decode_errors <= 5)
            alog("audio: opus_decode rc=%d (%s) len=%u", samples,
                 opus_strerror(samples), (unsigned)len);
        return;
    }
    a->stats.frames_decoded++;
#if HAVE_WASAPI
    /* OUT-3: SHADOW_WIN_AUDIO=0 built no ring - the decode-only path of
     * before, with no deposit line and no `taux` report. */
    if (!a->ring) return;
#endif

    audio_push_pcm(a, pcm, samples);

    if (a->stats.frames_decoded <= 5)
        alog("audio: frame #%u - %d samples dropped into the ring",
             a->stats.frames_decoded, samples);

    /* How many SECONDS of audio per second elapsed?
     *
     * READ THIS CAREFULLY - this counter misled me on 2026-08-25. A CUMULATIVE
     * ratio below 1 does NOT mean packets are being lost: it forever adds up the
     * remote desktop's silences. A real session showed "x0.80" while the audio
     * had been tracking real time for a minute - the deficit dated from silences
     * at the start of the session and never
     * bougeait plus.
     *
     * So we log BOTH: the rate over the last interval, which says what is
     * happening NOW (1.00 = real time, below = silence or loss in progress), and
     * the cumulative one, which only serves to locate. Only the first should
     * trigger an investigation. The real losses are counted in `ecartees` and
     * `erreurs` (emitted keys, kept French). */
    {
        /* DEC-4: the state lives in the decoder (per session) - see there. */
        struct timespec ts2;
        clock_gettime(CLOCK_MONOTONIC, &ts2);
        int64_t now = (int64_t)ts2.tv_sec * 1000 + ts2.tv_nsec / 1000000;
        if (a->rate_t0_ms == 0) { a->rate_t0_ms = a->rate_last_ms = now; }
        a->rate_samples_acc += (uint64_t)samples;
        if (now - a->rate_last_ms >= 10000) {
            double reel     = (double)(now - a->rate_t0_ms) / 1000.0;
            double audio_s  = (double)a->rate_samples_acc / (double)AUDIO_SAMPLE_RATE;
            double d_reel   = (double)(now - a->rate_last_ms) / 1000.0;
            double d_audio  = (double)(a->rate_samples_acc - a->rate_samples_prev)
                              / (double)AUDIO_SAMPLE_RATE;
            double taux     = d_reel > 0 ? d_audio / d_reel : 0.0;
            alog("audio: rate %.2f over the last %.0f s %s | cumulative %.2f "
                 "(%.1f s for %.1f s, silences included) | frames=%u "
                 "anneau_plein=%u ecartees=%u erreurs=%u",
                 taux, d_reel,
                 taux >= 0.98 ? "(temps reel)" : "(silence ou perte EN COURS)",
                 reel > 0 ? audio_s / reel : 0.0, audio_s, reel,
                 a->stats.frames_decoded, a->stats.buffers_dropped,
                 a->stats.invalid_packets, a->stats.decode_errors);
            a->rate_samples_prev = a->rate_samples_acc;
            a->rate_last_ms = now;
        }
    }
#else
    /* No backend - decode only, to validate; no output. Since OUT-3 no
     * supported platform comes here: Windows has WASAPI, and its decode-only
     * mode (SHADOW_WIN_AUDIO=0, or no device) goes through the ring branch
     * above. */
    /* Static rather than on the stack: a 23 KB call frame would go badly on the
     * console's threads, whose stacks are small. Only one thread feeds the
     * decoder, so sharing is safe here. */
    static int16_t pcm[AUDIO_BUFFER_SAMPLES * AUDIO_CHANNELS];
    int samples = opus_decode(a->opus, payload, (opus_int32)len,
                               pcm, AUDIO_BUFFER_SAMPLES, 0);
    if (samples < 0) a->stats.decode_errors++;
    else a->stats.frames_decoded++;   /* OUT-3: nothing is pushed where nothing plays */
#endif
}

/* Tries decoding at several offsets and keeps the one that works. */
static int audio_payload_offset(audio_decoder *a, const uint8_t *payload, size_t len)
{
    (void)a;   /* the detection is global, not per decoder - see below */
    /* === S53 2026-08-26 - THIS STATE OUTLIVES THE SESSION ===
     *
     * `chosen`, `cand_ok`, `tested` and `warned` are function-level statics: the
     * chosen offset, the scores and the warning cap cross session boundaries.
     * That is exactly the defect family that cost the night of 25-26 August
     * (black screen from the 3rd session on, sound audible exactly once, silent
     * hardware detectors - KB §3.28, §3.31).
     *
     * DEC-1 2026-09-11 - THE EFFECT WAS NOT BOUNDED. This paragraph used to say
     * it was, which held in one direction only, FLAC then Opus. In the other,
     * `chosen` fixed at 5 - by an Opus session, or by eight stream descriptors
     * from eight FLAC sessions, each counting as one more "consecutive" frame -
     * let the next FLAC session's descriptor classify it as Opus, and every FLAC
     * frame after it was refused: silence for the whole session, deterministic
     * (bench 7/7; live, the 8th FLAC session of a process in two series out of
     * two). Two things keep these statics harmless now: only `0x12` plaintexts
     * reach the decoder (ctrl_session.c, SHADOW_AUDIO_TYPE_FILTER), so this
     * probe only ever sees Opus frames and converges to 5; and a FLAC frame
     * promotes a session classified Opus (audio_decoder_feed,
     * SHADOW_FLAC_LATE_DETECT). The remaining pathological case is still TWO
     * simultaneous producers on one decoder, which only `SHADOW_AUDIO_DTLS=1`
     * creates.
     *
     * Left as statics ON PURPOSE: moving them into `audio_decoder` was measured
     * and costs 7 dropped frames (70 ms) at the start of EVERY Opus session,
     * where today only the first session of a process pays it. */
    static int chosen = -2;          /* -2 = not searched yet */
    if (chosen == -2) {
        const char *e = getenv("SHADOW_AUDIO_HDR");
        chosen = e ? atoi(e) : -1;   /* -1 = a determiner */
        if (chosen >= 0) alog("audio: decalage d'en-tete force a %d", chosen);
    }
    if (chosen >= 0) return chosen;

    /* The frames carry a 5-byte `[0x12][sequence u32 LE]` header, then the Opus:
     * at byte 5 we read `0xf4`, i.e. full-band CELT, stereo, one frame - exactly
     * the 48 kHz stereo the channel announced.
     *
     * We no longer settle for an `opus_decode` that returns a positive number:
     * decoding at a wrong offset often SUCCEEDS and produces noise, which was
     * heard. `opus_packet_get_nb_samples` is the weaker half of the test: it
     * reads only the TOC byte, and a code-3 packet's frame count, never the
     * internal lengths - the stream descriptor passes it at every candidate
     * offset (DEC-1, 2026-09-11). What makes the probe hold is the other half:
     * the same offset must hold across several consecutive frames - one byte may
     * look like a header by chance, not eight times in a row - and since DEC-1
     * only `0x12` frames reach it. */
    static const int CANDIDATES[] = { 5, 0, 6, 8, 4, 12 };
    static int cand_ok[sizeof(CANDIDATES) / sizeof(CANDIDATES[0])];
    static int tested = 0;

    for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
        int off = CANDIDATES[i];
        if ((size_t)off >= len) continue;
        int n = opus_packet_get_nb_samples(payload + off, (opus_int32)(len - off),
                                           AUDIO_SAMPLE_RATE);
        if (n > 0) cand_ok[i]++; else cand_ok[i] = 0;
    }
    tested++;

    for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
        if (cand_ok[i] >= 8) {
            chosen = CANDIDATES[i];
            alog("audio: a %d-byte header before the Opus "
                 "(framing validated over %d consecutive frames)", chosen, cand_ok[i]);
            return chosen;
        }
    }
    if (tested < 40) return -1;   /* let the probe converge */

    /* No offset decodes: we say so once, without blocking the session. */
    static int warned = 0;
    if (!warned) {
        warned = 1;
        alog("audio: no offset yields valid Opus (len=%zu) - "
             "the content is not raw Opus", len);
    }
    return -1;
}

void audio_decoder_get_stats(audio_decoder *a, audio_stats_t *out) {
    if (!a || !out) return;
    *out = a->stats;
    /* === L5 2026-08-29 - THE DROP COUNTER WAS DEAD ON CONSOLE ===
     * `stats.buffers_dropped` is incremented ONLY in the ALSA branch. The audout
     * path - the only one used on Switch - increments `buf_drop_count`, a field
     * of `struct audio_decoder` absent from `audio_stats_t`: the glue therefore
     * always copied it as zero and the panel never showed its row. A saturated
     * output queue was invisible by construction. We add the two: they do not
     * coexist in the same binary (HAVE_AUDOUT and HAVE_ALSA are mutually
     * exclusive), so the sum is always that of the path actually running.
     *
     * AUD-INS-4 2026-09-11 - this comment used to list "the FLAC report
     * permanently announced `overflow=0`" among what the sum fixed. It did not:
     * that report read `a->stats` directly, so on console it kept printing 0
     * while the same log's `[L5] file audio ... dropped=` carried the true count.
     * Both now read aud_drops(), the one place the sum is written. */
    out->buffers_dropped = aud_drops(a);
}
