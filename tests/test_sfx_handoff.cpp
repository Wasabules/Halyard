/* test_sfx_handoff - ui/sfx.cpp hands the console's single audio output to the
 * stream, and takes it back (AUDC-1 / OUT-2, 2026-09-11).
 *
 * === WHAT THIS SUITE EXISTS TO PREVENT ===
 *
 * libnx has ONE `audout` for the whole process. The UI sounds (ui/sfx.cpp) and
 * the stream (media/audio.c) both drove it, with no handoff:
 *   - every automatic retry plays Failed (showErrorUI); the UI-sound thread then
 *     kept audout STARTED until 2 s after the sound, while the reconnect loop
 *     waits 1 s and the glue opens the stream's output first thing. Either HOS
 *     refuses the second Start - no decoder for the whole session - or it
 *     accepts it, and the UI thread's idle close STOPS the shared output a second
 *     in. The reconnected session was silent either way (AUDC-1 bench: 0.0 %
 *     heard under REFUSE, 33 -> 3 % under ACCEPT);
 *   - the Connected chime was cut by release() microseconds after play(): 0 of
 *     its 34 464 frames, ever since S88;
 *   - a UI sound during a session (the wake-up lock over a live stream, AF2)
 *     opened audout under the stream: 48 refused Starts, or the stream stopped.
 *
 * === WHAT RUNS ===
 *
 * The REAL ui/sfx.cpp, compiled with -D__SWITCH__ so that its audout branch is
 * the one that runs, against a mock of libnx (tests/mock/switch.h, implemented
 * below): one refcounted IAudioOut, one released-buffer list, and a service that
 * plays the queued buffers back to back at 48 kHz while started and freezes them
 * while stopped. A second Start on a started output is REFUSED or ACCEPTED -
 * nobody has seen what HOS does, so both models run and the fix must hold under
 * both. The stream side is a stub that TRANSCRIBES media/audio.c's HAVE_AUDOUT
 * create / feed / destroy (audio.c itself needs libopus, libavcodec and libnx;
 * the AUDC-1 bench compiled it against the same kind of mock). The timelines
 * are connecting_activity.cpp's, in real time: about ten seconds per run.
 *
 * TWO RUNS, and both must pass (tests/run_tests.sh runs this binary twice):
 *   - default: SHADOW_SFX_HANDOFF on, the expectations are the FIXED behaviour;
 *   - SHADOW_SFX_HANDOFF=0: sfx.cpp restores the previous behaviour, so this run
 *     asserts the defect itself - the cut chime, the silent reconnect under both
 *     models, UI sounds opening audout under a live stream, a claim that returns
 *     with the UI output still started. That is the counter-case, demonstrated
 *     again on every run (the pattern of test_vid_reasm).
 * TEST_SFX_EXPECT=fixed|legacy overrides which expectations apply, for
 * demonstrations: SHADOW_SFX_HANDOFF=0 TEST_SFX_EXPECT=fixed prints, as FAIL
 * lines, exactly what the fix changes. TEST_SFX_VERBOSE=1 echoes the journal. */
#include "../clients/borealis/ui/sfx.hpp"

#include <switch.h>                    /* tests/mock/switch.h (-Imock) */
#include <borealis/core/assets.hpp>    /* tests/mock/: BRLS_RESOURCES */

extern "C" {
#include "../clients/borealis/ui/wav.h"
#include "../core/services/journal.h"
}

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ------------------------------------------------------------------ checks */
static int checks = 0, failures = 0;

static void check(bool ok, const char *fmt, ...)
{
    checks++;
    if (ok) return;
    failures++;
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::printf("  FAIL: %s\n", msg);
}

/* ------------------------------------------------------------------ time */
static double mono()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/* Windows rounds a sleep up to its 15.6 ms tick (and Windows 11 ignores a finer
 * request from a windowless process that has not opted out of power
 * throttling): a harness paced with plain sleeps reads ~35 % fewer blocks there
 * (OUT-2). So the last stretch of every wait yields instead of sleeping. The
 * sleeps inside sfx.cpp itself are left as they are: they only pace its idle
 * loop and the claim's polling, both bounded. */
#ifdef _WIN32
static const double SPIN_S = 0.017;
#else
static const double SPIN_S = 0.002;
#endif

static void wait_until(double target)
{
    for (;;) {
        const double rem = target - mono();
        if (rem <= 0.0) return;
        if (rem > SPIN_S)
            std::this_thread::sleep_for(std::chrono::duration<double>(rem - SPIN_S));
        else
            std::this_thread::yield();
    }
}

static void wait_s(double s) { wait_until(mono() + s); }

/* ------------------------------------------------------------------ journal stub */
static std::mutex               g_jm;
static std::vector<std::string> g_journal;
static bool                     g_verbose = false;

extern "C" void journal_write(journal_severity_t sev, journal_category_t cat,
                              const char *fmt, ...)
{
    (void)sev; (void)cat;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> g(g_jm);
    g_journal.push_back(buf);
    if (g_verbose) std::printf("    | %s\n", buf);
}

static int journal_count(const char *sub)
{
    std::lock_guard<std::mutex> g(g_jm);
    int n = 0;
    for (const std::string &l : g_journal)
        if (l.find(sub) != std::string::npos) n++;
    return n;
}

/* ------------------------------------------------------------------ mock libnx
 * See tests/mock/switch.h for what is modelled. Every call counts one IPC for
 * the thread that makes it: the UI-sound thread is SFX, main() marks itself
 * STREAM (media/audio.c runs on the connecting worker, which is this thread's
 * role here). */
namespace mock {

enum { SFX = 0, STREAM = 1 };

struct Counters {
    int  ipc = 0, start_ok = 0, start_fail = 0, stop = 0;
    long played = 0;                 /* frames the service played for this side */
};

struct Queued { AudioOutBuffer *b; long total; long played; int owner; };

static std::mutex           M;
static bool                 accept_second = false;
static int                  refcnt = 0;
static bool                 started = false;
static int                  started_by = -1;
static std::deque<Queued>   queue, released;
static double               last = 0.0, carry = 0.0;
static Counters             C[2];
static std::vector<int16_t> sfx_tape;   /* every UI-sound frame played, in order */
static thread_local int     owner = SFX;

/* Placeholders for the service's error codes: only "not zero" matters. */
static const Result RC_NOT_OPEN = 0x2A8FF, RC_BAD_STATE = 0x2A899, RC_TIMEOUT = 0xEA01;

/* Plays the queue forward to `now`. Assumes M held. */
static void advance(double now)
{
    if (started && now > last) {
        double avail = (now - last) * 48000.0 + carry;
        while (!queue.empty() && avail >= 1.0) {
            Queued &f = queue.front();
            const long rem  = f.total - f.played;
            const long take = ((long)avail < rem) ? (long)avail : rem;
            if (f.owner == SFX) {
                const int16_t *src = (const int16_t *)((const uint8_t *)f.b->buffer
                                                       + f.b->data_offset);
                sfx_tape.insert(sfx_tape.end(), src + f.played * 2,
                                src + (f.played + take) * 2);
            }
            C[f.owner].played += take;
            f.played += take;
            avail -= (double)take;
            if (f.played >= f.total) { released.push_back(f); queue.pop_front(); }
        }
        /* An empty queue is an underrun: time passes in silence. */
        carry = queue.empty() ? 0.0 : avail;
    }
    last = now;
}

/* The side that leaves while the other keeps the output open: its buffers are
 * forgotten, so the mock never reads memory its owner has freed. */
static void drop_owner(int o)
{
    for (std::deque<Queued> *dq : { &queue, &released })
        dq->erase(std::remove_if(dq->begin(), dq->end(),
                                 [o](const Queued &x) { return x.owner == o; }),
                  dq->end());
}

/* Assumes M held. */
static bool pop_released(AudioOutBuffer **out, u32 *cnt)
{
    advance(mono());
    if (!released.empty()) {
        *out = released.front().b;
        *cnt = 1;
        released.pop_front();
        return true;
    }
    *out = nullptr;
    *cnt = 0;
    return false;
}

static void begin(bool accept)
{
    std::lock_guard<std::mutex> g(M);
    if (refcnt != 0)
        std::printf("  (mock: refcount %d left over by the previous scenario, reset)\n", refcnt);
    refcnt = 0;
    accept_second = accept;
    started = false;
    started_by = -1;
    queue.clear();
    released.clear();
    last = mono();
    carry = 0.0;
    C[0] = Counters();
    C[1] = Counters();
    sfx_tape.clear();
}

static Counters counters(int o)
{
    std::lock_guard<std::mutex> g(M);
    return C[o];
}

static bool holds_started(int o)
{
    std::lock_guard<std::mutex> g(M);
    return started && started_by == o;
}

static int refcount()
{
    std::lock_guard<std::mutex> g(M);
    return refcnt;
}

static std::vector<int16_t> tape()
{
    std::lock_guard<std::mutex> g(M);
    return sfx_tape;
}

}  // namespace mock

extern "C" Result audoutInitialize(void)
{
    std::lock_guard<std::mutex> g(mock::M);
    mock::C[mock::owner].ipc++;
    if (mock::refcnt++ == 0) {               /* the only real open */
        mock::started = false;
        mock::started_by = -1;
        mock::queue.clear();
        mock::released.clear();
        mock::last = mono();
        mock::carry = 0.0;
    }
    return 0;
}

extern "C" void audoutExit(void)
{
    std::lock_guard<std::mutex> g(mock::M);
    mock::C[mock::owner].ipc++;
    if (mock::refcnt > 0 && --mock::refcnt == 0) {
        mock::advance(mono());
        mock::started = false;
        mock::started_by = -1;
        mock::queue.clear();
        mock::released.clear();
    } else {
        mock::drop_owner(mock::owner);
    }
}

extern "C" Result audoutStartAudioOut(void)
{
    std::lock_guard<std::mutex> g(mock::M);
    mock::C[mock::owner].ipc++;
    const double now = mono();
    mock::advance(now);
    if (mock::refcnt == 0) return mock::RC_NOT_OPEN;
    if (mock::started) {
        if (!mock::accept_second) {
            mock::C[mock::owner].start_fail++;
            return mock::RC_BAD_STATE;
        }
        mock::C[mock::owner].start_ok++;
        return 0;
    }
    mock::started = true;
    mock::started_by = mock::owner;
    mock::last = now;
    mock::carry = 0.0;
    mock::C[mock::owner].start_ok++;
    return 0;
}

extern "C" Result audoutStopAudioOut(void)
{
    std::lock_guard<std::mutex> g(mock::M);
    mock::C[mock::owner].ipc++;
    mock::advance(mono());
    mock::C[mock::owner].stop++;
    mock::started = false;
    mock::started_by = -1;
    return 0;
}

extern "C" Result audoutAppendAudioOutBuffer(AudioOutBuffer *b)
{
    std::lock_guard<std::mutex> g(mock::M);
    mock::C[mock::owner].ipc++;
    mock::advance(mono());
    if (mock::refcnt == 0) return mock::RC_NOT_OPEN;
    mock::queue.push_back(mock::Queued{ b, (long)(b->data_size / 4), 0, mock::owner });
    return 0;
}

extern "C" Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer **out, u32 *cnt)
{
    std::lock_guard<std::mutex> g(mock::M);
    mock::C[mock::owner].ipc++;
    mock::pop_released(out, cnt);
    return 0;
}

/* libnx: eventClear, GetReleased, and if nothing came back, wait on the ONE
 * buffer event of the process. What it returns can be any side's buffer. */
extern "C" Result audoutWaitPlayFinish(AudioOutBuffer **out, u32 *cnt, u64 timeout_ns)
{
    const double deadline = mono() + (double)timeout_ns / 1e9;
    bool got;
    {
        std::lock_guard<std::mutex> g(mock::M);
        mock::C[mock::owner].ipc++;
        got = mock::pop_released(out, cnt);
    }
    while (!got) {
        double wait;
        {
            std::lock_guard<std::mutex> g(mock::M);
            const double now = mono();
            mock::advance(now);
            wait = deadline - now;
            if (mock::started && !mock::queue.empty()) {
                const mock::Queued &f = mock::queue.front();
                const double end = ((double)(f.total - f.played) - mock::carry) / 48000.0;
                if (end < wait) wait = end;
            }
        }
        if (wait > 0.0) wait_s(wait + 30e-6);
        {
            std::lock_guard<std::mutex> g(mock::M);
            got = mock::pop_released(out, cnt);
        }
        if (got || mono() >= deadline) break;
    }
    return got ? 0 : mock::RC_TIMEOUT;
}

/* ------------------------------------------------------------------ the stream
 * media/audio.c's HAVE_AUDOUT path, TRANSCRIBED - audio.c itself needs libopus,
 * libavcodec and libnx:
 *   - audio_decoder_create: audoutInitialize, then audoutStartAudioOut; since
 *     AUDC-1 part 3 every failure goes through audio_decoder_destroy, which exits
 *     what was opened (and hwopus, which this stub has no use for);
 *   - audio_decoder_feed: reclaim every released buffer that is OURS, take a free
 *     one or drop the packet ("dropping packet — no free buffer"), append;
 *   - audio_decoder_destroy: Stop if started, Exit if opened.
 * Four buffers, one 20 ms frame each, as on console. */
namespace stream {

const int NUM    = 4;
const int FRAMES = 960;                  /* 20 ms at 48 kHz */

struct Decoder {
    bool           audout_init = false, audout_started = false;
    int16_t        pcm[NUM][FRAMES * 2];
    AudioOutBuffer aob[NUM];
    bool           buf_free[NUM];
    unsigned       drops = 0;
};

static void destroy(Decoder *a)
{
    if (!a) return;
    if (a->audout_started) audoutStopAudioOut();
    if (a->audout_init)    audoutExit();
    delete a;
}

static Decoder *create()
{
    Decoder *a = new Decoder();
    if (R_FAILED(audoutInitialize())) { destroy(a); return nullptr; }
    a->audout_init = true;
    if (R_FAILED(audoutStartAudioOut())) { destroy(a); return nullptr; }
    a->audout_started = true;
    for (int i = 0; i < NUM; i++) {
        a->aob[i] = AudioOutBuffer{ nullptr, a->pcm[i], sizeof a->pcm[i], 0, 0 };
        a->buf_free[i] = true;
    }
    return a;
}

static void feed(Decoder *a, unsigned seq)
{
    if (!a->audout_started) return;
    AudioOutBuffer *rel = nullptr;
    u32 n = 0;
    while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&rel, &n)) && n > 0) {
        for (int j = 0; j < NUM; j++)
            if (rel == &a->aob[j]) { a->buf_free[j] = true; break; }
        rel = nullptr;
        n = 0;
    }
    int idx = -1;
    for (int j = 0; j < NUM; j++)
        if (a->buf_free[j]) { idx = j; break; }
    if (idx < 0) { a->drops++; return; }
    for (int i = 0; i < FRAMES * 2; i++)
        a->pcm[idx][i] = (int16_t)(1 + ((seq * 7u + (unsigned)i) & 0x3FFFu));
    a->aob[idx].data_size = sizeof a->pcm[idx];
    a->aob[idx].data_offset = 0;
    a->buf_free[idx] = false;
    if (R_FAILED(audoutAppendAudioOutBuffer(&a->aob[idx]))) a->buf_free[idx] = true;
}

}  // namespace stream

/* ------------------------------------------------------------------ the chime */
static std::vector<int16_t> g_chime;     /* connecte.wav as mix_locked() renders it at volume 70 */
static long                 g_chime_frames = 0;

static bool load_chime()
{
    const std::string path = std::string(BRLS_RESOURCES) + "sfx/connecte.wav";
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> raw(1u << 20);
    const size_t n = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    wav_info_t w;
    if (!wav_analyser(raw.data(), n, &w) || w.frequency != 48000) return false;
    const int16_t *src = (const int16_t *)(raw.data() + w.offset);
    for (size_t i = 0; i < w.frames; i++) {
        const int16_t l = src[i * w.channels];
        const int16_t r = (w.channels == 2) ? src[i * 2 + 1] : l;
        g_chime.push_back((int16_t)((int32_t)l * 70 / 100));
        g_chime.push_back((int16_t)((int32_t)r * 70 / 100));
    }
    g_chime_frames = (long)w.frames;
    return g_chime_frames > 0;
}

/* ------------------------------------------------------------------ scenarios */
static bool g_fixed = true;      /* which expectations apply */
static int  g_claims = 0;        /* claims made while the handoff is on */
static int  g_lock_step = 0;

struct Session {
    bool     created = false;
    unsigned drops = 0;
    long     sent = 0, played = 0;
    int      sfx_ipc = 0;            /* UI-sound calls into audout DURING the session */
    double   heard() const { return sent ? 100.0 * (double)played / (double)sent : 0.0; }
};

typedef void (*AtFn)(double t);

/* ctrl_session_glue_run's audio part: X s of glue work (h264_decoder_create,
 * the decode thread) before audio_decoder_create, one 20 ms frame every 20 ms,
 * the decoder destroyed at the end. */
static Session run_session(double x, double dur, AtFn at)
{
    Session s;
    wait_s(x);
    const mock::Counters s0 = mock::counters(mock::SFX), q0 = mock::counters(mock::STREAM);
    const double tc = mono();
    stream::Decoder *a = stream::create();
    s.created = (a != nullptr);
    const int n = (int)(dur / 0.020 + 0.5);
    for (int k = 0; k < n; k++) {
        wait_until(tc + k * 0.020);
        if (at) at(mono() - tc);
        if (a) stream::feed(a, (unsigned)k);
    }
    wait_until(tc + n * 0.020 + 0.100);      /* let the queue drain */
    if (a) { s.drops = a->drops; stream::destroy(a); }
    const mock::Counters s1 = mock::counters(mock::SFX), q1 = mock::counters(mock::STREAM);
    s.sent    = (long)n * stream::FRAMES;
    s.played  = q1.played - q0.played;
    s.sfx_ipc = s1.ipc - s0.ipc;
    return s;
}

static const char *model(bool accept) { return accept ? "ACCEPT" : "REFUSE"; }

static void end_scenario(const char *what, bool accept)
{
    ui::sfx::close();                /* joins the UI-sound thread; the next play() starts another */
    const int rc = mock::refcount();
    check(rc == 0, "%s %s: audout closed by both sides at the end (refcount %d)",
          what, model(accept), rc);
}

/* First connection (connecting_activity.cpp): play(Connected), the push of the
 * stream screen, release(), 500 ms, the SSE and its 500 ms, then the claim
 * around ctrl_session_glue_run. */
static void scenario_chime(bool accept)
{
    mock::begin(accept);
    ui::sfx::play(ui::sfx::Sound::Connected);
    ui::sfx::release();
    wait_s(1.0);
    Session s;
    bool handed = false;
    {
        ui::sfx::StreamClaim claim;
        handed = claim.handed();
        if (g_fixed) g_claims++;
        s = run_session(0.0, 0.3, nullptr);
    }
    const std::vector<int16_t> t = mock::tape();
    long exact = 0;
    const size_t frames = std::min(t.size() / 2, g_chime.size() / 2);
    for (size_t i = 0; i < frames; i++) {
        if (t[2 * i] != g_chime[2 * i] || t[2 * i + 1] != g_chime[2 * i + 1]) break;
        exact++;
    }
    const double pct = 100.0 * (double)exact / (double)g_chime_frames;
    std::printf("  chime     %s: %ld / %ld frames bit-exact (%.2f %%) | stream %s, %u drop(s), "
                "%.1f %% heard | UI-sound IPC during the session %d\n",
                model(accept), exact, g_chime_frames, pct, s.created ? "open" : "NOT OPEN",
                s.drops, s.heard(), s.sfx_ipc);
    if (g_fixed) {
        check(pct >= 97.0, "chime %s: the Connected chime plays (%.2f %% of its frames, >= 97 %% "
              "expected - the last partial 10 ms block is the mixer's)", model(accept), pct);
        check(handed, "chime %s: the claim was acknowledged within 100 ms", model(accept));
        check(s.sfx_ipc == 0, "chime %s: no UI-sound call into audout during the session (%d)",
              model(accept), s.sfx_ipc);
    } else {
        check(pct < 10.0, "chime %s, legacy: release() cuts the chime (%.2f %% of its frames "
              "played, < 10 %% expected)", model(accept), pct);
    }
    /* Same in both runs: the first connection's stream was fine even before -
     * only because the chime was cut. */
    check(s.created && s.drops == 0 && s.heard() >= 95.0,
          "chime %s: the stream opens and plays everything (%s, %u drops, %.1f %%)",
          model(accept), s.created ? "open" : "not open", s.drops, s.heard());
    end_scenario("chime", accept);
}

/* The reconnect loop: the previous session's decoder is gone, showError
 * (reconnecting) plays Failed through showErrorUI, 10 x 100 ms, then the claim
 * and ctrl_session_glue_run, whose audio_decoder_create comes X = 0.2 s in. */
static void scenario_reconnect(bool accept)
{
    mock::begin(accept);
    ui::sfx::play(ui::sfx::Sound::Failed);
    wait_s(1.0);
    Session s;
    bool handed = false;
    {
        ui::sfx::StreamClaim claim;
        handed = claim.handed();
        if (g_fixed) g_claims++;
        s = run_session(0.2, 1.4, nullptr);
    }
    std::printf("  reconnect %s: stream %s, %u drop(s), %.1f %% heard | UI-sound IPC during "
                "the session %d\n", model(accept), s.created ? "open" : "NOT OPEN", s.drops,
                s.heard(), s.sfx_ipc);
    if (g_fixed) {
        check(handed, "reconnect %s: the claim was acknowledged within 100 ms", model(accept));
        check(s.created, "reconnect %s: the stream's audoutStartAudioOut succeeds", model(accept));
        check(s.drops == 0 && s.heard() >= 95.0,
              "reconnect %s: the reconnected session plays everything (%u drops, %.1f %%)",
              model(accept), s.drops, s.heard());
        check(s.sfx_ipc == 0, "reconnect %s: no UI-sound call into audout during the session (%d)",
              model(accept), s.sfx_ipc);
    } else if (!accept) {
        check(!s.created, "reconnect REFUSE, legacy: the stream's Start is refused - the UI "
              "sounds still hold audout, the session has no decoder");
    } else {
        check(s.created && s.drops > 0,
              "reconnect ACCEPT, legacy: the UI thread's idle close stops the shared output "
              "mid-session (%s, %u drops)", s.created ? "open" : "not open", s.drops);
    }
    end_scenario("reconnect", accept);
}

/* AF2: the wake-up lock pushed over a live session plays Section, then a PIN
 * digit (Navigation), then Confirm. */
static void lock_sounds(double t)
{
    if (g_lock_step == 0 && t >= 0.1)      { g_lock_step++; ui::sfx::play(ui::sfx::Sound::Section); }
    else if (g_lock_step == 1 && t >= 0.3) { g_lock_step++; ui::sfx::play(ui::sfx::Sound::Navigation); }
    else if (g_lock_step == 2 && t >= 0.5) { g_lock_step++; ui::sfx::play(ui::sfx::Sound::Confirm); }
}

static void scenario_lock(bool accept)
{
    mock::begin(accept);
    g_lock_step = 0;
    Session s;
    {
        ui::sfx::StreamClaim claim;
        if (g_fixed) g_claims++;
        s = run_session(0.0, 0.9, lock_sounds);
    }
    std::printf("  lock      %s: UI-sound IPC during the session %d | stream %s, %u drop(s), "
                "%.1f %% heard\n", model(accept), s.sfx_ipc, s.created ? "open" : "NOT OPEN",
                s.drops, s.heard());
    if (g_fixed) {
        check(s.sfx_ipc == 0, "lock %s: UI sounds over a live session never touch audout (%d IPC)",
              model(accept), s.sfx_ipc);
        check(s.created && s.drops == 0 && s.heard() >= 95.0,
              "lock %s: the stream is intact (%u drops, %.1f %%)", model(accept), s.drops, s.heard());
    } else {
        check(s.sfx_ipc > 0, "lock %s, legacy: the UI sounds open audout under the live stream "
              "(%d IPC)", model(accept), s.sfx_ipc);
    }
    end_scenario("lock", accept);
}

/* The claim raced against an open in progress: a sound, then the claim 0-25 ms
 * later - before, during or after the UI-sound thread opens its output. */
static void scenario_race()
{
    mock::begin(false);
    uint32_t r = 0x9E3779B9u;            /* xorshift: the same sequence everywhere */
    int started_at_return = 0, started_after = 0, timeouts = 0;
    std::vector<double> lat;
    const int N = 25;
    for (int i = 0; i < N; i++) {
        ui::sfx::play(ui::sfx::Sound::Navigation);
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        wait_s((double)(r % 2500u) / 100000.0);
        const double a = mono();
        if (!ui::sfx::suspend_for_stream()) timeouts++;
        if (g_fixed) g_claims++;
        lat.push_back((mono() - a) * 1e3);
        if (mock::holds_started(mock::SFX)) started_at_return++;
        const int s0 = mock::counters(mock::SFX).start_ok;
        wait_s(0.040);
        if (mock::counters(mock::SFX).start_ok > s0) started_after++;
        ui::sfx::resume_after_stream();
        wait_s(0.005);
    }
    std::sort(lat.begin(), lat.end());
    std::printf("  race      %d claims: UI output started at return %d, started after return %d, "
                "timeouts %d | claim p50 %.1f ms, worst %.1f ms\n", N, started_at_return,
                started_after, timeouts, lat[lat.size() / 2], lat.back());
    if (g_fixed) {
        check(started_at_return == 0, "race: no claim returns with the UI output still started (%d/%d)",
              started_at_return, N);
        check(started_after == 0, "race: the UI output never starts after a claim returned (%d/%d)",
              started_after, N);
        check(timeouts == 0, "race: every claim acknowledged within 100 ms (%d timeout(s))", timeouts);
    } else {
        check(started_at_return > 0, "race, legacy: the claim is a no-op and returns with the UI "
              "output started (%d/%d)", started_at_return, N);
    }
    end_scenario("race", false);
}

int main()
{
    const char *e = std::getenv("SHADOW_SFX_HANDOFF");
    const char *x = std::getenv("TEST_SFX_EXPECT");
    g_fixed = x ? (std::strcmp(x, "legacy") != 0) : !(e && std::atoi(e) == 0);
    g_verbose = std::getenv("TEST_SFX_VERBOSE") != nullptr;
    std::printf("== AUDC-1 / OUT-2: the UI sounds hand the audio output to the stream ==\n");
    std::printf("  SHADOW_SFX_HANDOFF=%s, expectations: %s\n", e ? e : "(unset)",
                g_fixed ? "the FIXED behaviour"
                        : "the PREVIOUS behaviour (the defect, the counter-case)");

    mock::owner = mock::STREAM;          /* this thread plays media/audio.c's part */
    if (!load_chime()) {
        check(false, "%ssfx/connecte.wav must be readable, 16-bit PCM at 48 kHz: the chime "
              "checks have nothing to compare against", BRLS_RESOURCES);
        std::printf("%d checks, %d failure(s)\n", checks, failures);
        return 1;
    }
    ui::sfx::init();
    ui::sfx::setVolume(70);              /* the setting's default */
    check(journal_count("14/14") == 1, "the 14 UI sounds load from %ssfx/", BRLS_RESOURCES);

    for (int m = 0; m < 2; m++) scenario_chime(m == 1);
    for (int m = 0; m < 2; m++) scenario_reconnect(m == 1);
    for (int m = 0; m < 2; m++) scenario_lock(m == 1);
    scenario_race();

    /* The journal: the live witnesses the console A/B reads. */
    const int witnessed = journal_count("[AUDC1] interface sounds suspended during the stream");
    const int refused   = journal_count("[AUDC1] sons d'interface : sortie non rendue en 100 ms");
    const int disabled  = journal_count("[AUDC1] handing the audio output to the stream is disabled");
    if (g_fixed) {
        check(witnessed == g_claims, "journal: one '[AUDC1] ... suspended' line per claim (%d for %d)",
              witnessed, g_claims);
        check(refused == 0, "journal: no '[AUDC1] ... sortie non rendue' line (%d)", refused);
        check(disabled == 0, "journal: the handoff is not reported disabled (%d)", disabled);
    } else {
        check(witnessed == 0, "journal, legacy: no claim is ever made (%d '[AUDC1] ... suspended' "
              "lines)", witnessed);
        check(disabled == 1, "journal, legacy: '[AUDC1] ... disabled (SHADOW_SFX_HANDOFF=0)' said "
              "once (%d)", disabled);
    }

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
