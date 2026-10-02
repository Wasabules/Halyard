/* devlink - see the devlink.hpp header for the protocol and the safety rules.
 * This file contains only what touches the hardware: OpenGL, the clock, the log
 * socket. Everything else is pure, lives in the header, and is checkable
 * offline.
 */
#include "clients/borealis/devlink/devlink.hpp"
#include "clients/borealis/devlink/devcmd.h"   /* the common parser for received commands */

/* --- stb_image_write ----------------------------------------------------
 *
 * The header is already in the repo, as a GLFW dependency. There is therefore NO
 * new dependency to install, which is the project's constraint.
 *
 * The implementation must exist in exactly ONE place: two definitions would
 * break linking. Checked before writing this - the only other
 * STB_IMAGE_WRITE_IMPLEMENTATION in the repo is in
 * library/borealis/library/lib/extern/glfw/examples/offscreen.c, and GLFW's
 * examples are added as EXCLUDE_FROM_ALL: they are never compiled.
 * STB_IMAGE_WRITE_STATIC settles the point for good: every symbol becomes
 * internal to this translation unit, and a future instantiation elsewhere cannot
 * conflict with this one.
 *
 * STBI_WRITE_NO_STDIO removes the variants that write a file: we only write to
 * memory, and on console there is no reason to keep code capable of creating a
 * file on the SD card from a network command. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../../third_party/borealis/library/lib/extern/glfw/deps/stb_image_write.h"
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* Borealis draws through GLFW + OpenGL on BOTH sides: on console too,
 * SwitchPlatform::createWindow builds a GLFWVideoContext (checked in
 * library/borealis/library/lib/platforms/switch/switch_platform.cpp). That is
 * what makes glReadPixels usable on the Switch as on the desktop, and it is the
 * whole reason this module can exist.
 *
 * The guard below is only there for configurations where Borealis does NOT use
 * OpenGL (deko3d). They are not built here, but a capture that refuses cleanly
 * beats a compile error the day someone tries. */
#include "../gl_compat.h"
#if defined(BOREALIS_USE_DEKO3D) || !SHADOW_HAVE_GLAD
#  define DEVLINK_NO_GL 1
#else
#  include <glad/glad.h>
#endif

extern "C" {
#include "core/services/config.h"
#include "core/services/log.h"
/* S81 - this module's category. See shadow/journal.h: it is declared here,
 * never inferred from the text of the messages. */
#define dllog(...) JOURNAL_INFO_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)
#define dldbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)

}

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace devlink {
namespace {

/* --- Settings -----------------------------------------------------------
 *
 * The same mechanism as everywhere in this repo: one toggle per decision, a
 * default commented with the measurement that set it, and a revert with no
 * rebuild. The values are read ONCE then kept; they are startup settings, not
 * session state. */
int setting(const char *name, int fallback, int lo, int hi)
{
    const char *e = std::getenv(name);
    if (!e || !*e) return fallback;
    const int v = std::atoi(e);
    return (v < lo || v > hi) ? fallback : v;
}

/* Default shrink factor.
 *
 * 2, not 1. A 1920x1080 capture is 6 MB raw, which stb's PNG encoder then has to
 * walk: that is time taken ON THE RENDER THREAD (see below). Halving divides
 * that cost by four and stays far legible enough for what we do with it - check
 * a layout, read a label, see which element has the focus. `shot 1` remains
 * available line by line the day we want to examine a fine detail. */
int defaultShrink() { static const int v = setting("SHADOW_DEVLINK_REDUCTION", 2, 1, 2); return v; }

/* Capture lines emitted per frame.
 *
 * 32 * 512 = 16 KB per frame, i.e. about 1 MB/s at 60 frames per second: the
 * same order of magnitude as what the log socket already carries during a
 * talkative session. A half-size 720p capture fits in a few hundred lines, so it
 * leaves in a fraction of a second - fast enough for an automation loop, slow
 * enough to stay invisible to the eye.
 *
 * THE COUNTER-CASE is the number one would be tempted to choose: emit everything
 * at once. Every line is a blocking `send()` taken under the log's lock; several
 * hundred inside a single frame is a freeze on the order of half a second, and
 * it would be blamed on the change being measured. */
int linesPerFrame() { static const int v = setting("SHADOW_DEVLINK_LIGNES", 32, 1, 512); return v; }

/* stb's PNG compression effort (1 to 9).
 *
 * 5: an interface image is made of flat areas, it compresses very well anyway,
 * and stb's compressor is slow on a Switch core. Going to 9 gains a few percent
 * of bytes for a noticeably longer time, on the render thread. */
int pngEffort() { static const int v = setting("SHADOW_DEVLINK_ZLIB", 5, 1, 9); return v; }

/* Which buffer to read. 0 = GL_FRONT, 1 = GL_BACK, 2 = decide by looking (default).
 *
 * WHY THE FRONT IS THE RIGHT ANSWER IN THEORY. Borealis is an upstream submodule
 * we do not modify, and it exposes no hook BEFORE the buffer swap:
 * `Application::frame()` ends with `videoContext->endFrame()`, which swaps. The
 * only clean hook, `getRunLoopEvent()`, fires JUST AFTER. At that moment the
 * image we want is the one that has just been presented, hence the front; the
 * back has no defined contents after a swap, and reading it would return,
 * depending on the driver, a black image, the previous one, or randomness.
 *
 * WHY THAT IS NOT ENOUGH IN PRACTICE (measured 2026-09-12, Linux/Mesa/Wayland).
 * On an EGL surface there is no readable front buffer at all: `glReadPixels`
 * succeeds, raises NO GL error, and fills the buffer with zeroes. So the capture
 * arrives whole, opens fine, and is uniformly black - and a black capture looks
 * exactly like a rendering bug. A whole session was spent on the wrong side of
 * the C code because of it. The back buffer, on the same driver, holds the image
 * that has just been presented.
 *
 * Neither buffer is right everywhere, and nothing in the GL state says which one
 * this driver will serve. So DEFAULT 2: read the front, and if every single
 * pixel came back identical, read the back and keep that one if it has content.
 * The choice is made once per process and logged, because a tool that silently
 * changes what it measures is worse than one that needs a toggle.
 *
 * The counter-case for the uniformity test is a screen that is LEGITIMATELY one
 * colour - the boot screen, a stream before its first picture. The second read
 * then comes back uniform too, we keep the first, and nothing is lost but one
 * `glReadPixels`. That is why the fallback requires the second buffer to have
 * CONTENT, and does not merely prefer the back. */
int bufferChoice() { static const int v = setting("SHADOW_DEVLINK_TAMPON", 2, 0, 2); return v; }

/* Does this image carry anything at all, or is it one flat colour?
 *
 * Compares against the first pixel and stops at the first difference: on a real
 * screen that happens within a few pixels, so the cost is nil. On a uniform
 * image it is one pass over the buffer, which is still far below the PNG
 * encoding that follows. */
bool hasContent(const unsigned char *p, size_t n)
{
    if (n < 3u) return false;
    for (size_t i = 3; i + 2 < n; i += 3)
        if (p[i] != p[0] || p[i + 1] != p[1] || p[i + 2] != p[2]) return true;
    return false;
}

/* Hard bound on the produced PNG. Beyond it we refuse the capture rather than
 * truncate it: a truncated PNG transmits perfectly and does not open, and we
 * would look for the cause on the base64 side. */
const size_t PNG_MAX = 6u * 1024u * 1024u;

/* --- Module state --------------------------------------------------------
 *
 * File `static`s, not function ones, and nothing in here is SESSION state: they
 * are the send queue in progress and the descriptor of the displayed screen. The
 * distinction is the one that has cost this repo four failures. The corollary is
 * respected here: no interval counter compared against a monotonic clock, hence
 * no immediate firing at startup. */
std::mutex               g_queue_lock;    /* protege g_file / g_curseur */
std::vector<std::string> g_queue;
size_t                   g_cursor = 0;

std::mutex  g_state_lock;                 /* protege g_desc / g_publie */
Describer g_desc;
std::string g_published;

/* Which framebuffer this driver actually serves: -1 = not settled, 0 = GL_FRONT,
 * 1 = GL_BACK. A property of the driver, not of the session - it is decided at
 * the first capture and never reconsidered, which is why it lives here and not
 * in a function `static` (see the block comment above). */
int g_read_buffer = -1;

/* 0 = nothing requested; 1 or 2 = capture requested with that shrink factor. */
std::atomic<int>  g_capture_request{0};

/* The factor the capture in progress serves. It exists because `onFrame()`
 * CONSUMES `g_demande_capture` (exchanged for 0) before calling `capture()`:
 * without this relay, `capture()` re-read a counter already reset to zero and
 * fell back on the default setting. An explicit `requestCapture(1)` then returned
 * a half-size image, silently. Read and written by the render thread only. */
int g_shrink_served = 0;
std::atomic<bool> g_state_request{false};

long long nowMs()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Write context for stb. The vector alone is not enough: stb has no way to
 * signal a write refusal, so we track the overflow ourselves and report it
 * instead of delivering a truncated PNG. */
struct PngSink {
    std::vector<unsigned char> *bytes = nullptr;
    bool                        overflow = false;
};

void writePng(void *ctx, void *data, int size)
{
    PngSink *s = static_cast<PngSink *>(ctx);
    if (!s || !s->bytes || size <= 0 || s->overflow) return;
    if (s->bytes->size() + (size_t)size > PNG_MAX) { s->overflow = true; return; }
    const unsigned char *p = static_cast<const unsigned char *>(data);
    s->bytes->insert(s->bytes->end(), p, p + (size_t)size);
}

/* Is a capture still being drained? */
bool transferInProgress()
{
    std::lock_guard<std::mutex> v(g_queue_lock);
    return g_cursor < g_queue.size();
}

}  // namespace

/* ========================================================================= */

bool available()
{
    /* A STARTUP constant: the presence of a file on the SD card. We do NOT
     * re-read it every frame - `onFrame()` comes through here 60 times a second,
     * and one `fopen` per frame on the SD card would cost more than the whole
     * rest of the module put together.
     *
     * We redo this read even though log.c has already done it for itself: log.c
     * does not publish the result. A sink-state accessor over
     * there would make these few lines unnecessary (see the report). */
    static int avail = -1;
    if (avail < 0) {
        char path[256];
        std::snprintf(path, sizeof path, "%slogsink.txt", SHADOW_DATA_DIR);
        std::FILE *f = std::fopen(path, "r");
        avail = f ? 1 : 0;
        if (f) std::fclose(f);
    }
    return avail == 1;
}

/* Both buffers are deliberately small: a command name and a reason fit in one
 * word. What arrives from the network has already been refused or reduced to a
 * CANONICAL name by devcmd.h before reaching here - we never copy a network
 * string as is into a reply. */
void replyOk(const char *command)
{
    if (!available()) return;
    char c[32];
    devlink_sanitize(c, sizeof c, command ? command : "?");
    dllog("[devlink] ok %s", c);
}

void replyErr(const char *command, const char *reason)
{
    if (!available()) return;
    char c[32], r[32];
    devlink_sanitize(c, sizeof c, command ? command : "?");
    devlink_sanitize(r, sizeof r, reason ? reason : "inconnue");
    dllog("[devlink] err %s %s", c, r);
}

/* --- Describing the screen ------------------------------------------------ */

void setStateDescriber(Describer d)
{
    std::lock_guard<std::mutex> v(g_state_lock);
    g_desc = std::move(d);
}

void clearStateDescriber()
{
    std::lock_guard<std::mutex> v(g_state_lock);
    g_desc = nullptr;
}

void publishState(const std::string &line)
{
    std::lock_guard<std::mutex> v(g_state_lock);
    g_published = line;
}

/* --- Frame rate ----------------------------------------------------------
 *
 * A one-second moving average, noted by the screen on every frame. It exists to
 * answer "does this effect cost frames?" with a MEASUREMENT, a question that
 * comes up as soon as one adds background strokes - here about fifty per frame
 * for the waves. Without it, one can only guess. */
std::atomic<double> g_fps{0.0};
std::atomic<double> g_fps_t0{0.0};
std::atomic<int>    g_fps_n{0};

void noteFrame(double t)
{
    const double t0 = g_fps_t0.load(std::memory_order_relaxed);
    if (t0 <= 0.0 || t < t0) {          /* first frame, or the clock went backwards */
        g_fps_t0.store(t, std::memory_order_relaxed);
        g_fps_n.store(0, std::memory_order_relaxed);
        return;
    }
    const int    n  = g_fps_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const double dt = t - t0;
    if (dt >= 1.0) {
        g_fps.store((double)n / dt, std::memory_order_relaxed);
        g_fps_t0.store(t, std::memory_order_relaxed);
        g_fps_n.store(0, std::memory_order_relaxed);
    }
}

std::string composeState(const char *screen, int focus, int nb, const char *label)
{
    char line[DEVLINK_STATE_MAX + 128];
    const size_t n = devlink_format_state(line, sizeof line, screen, focus, nb, label);
    return std::string(line, n);
}

std::string state()
{
    Describer d;
    std::string published;
    {
        /* We COPY the callback, then invoke it outside the lock. Holding a lock
         * across code we do not control is the mistake that produced the sender
         * thread starvation in wss.c; here the callback belongs to a screen and
         * may do whatever it likes. */
        std::lock_guard<std::mutex> v(g_state_lock);
        d = g_desc;
        published = g_published;
    }

    std::string raw;
    if (d) raw = d();
    else   raw = published;

    /* Never an empty reply: a script that receives silence cannot tell "no
     * screen described itself" from "the channel went down". */
    if (raw.empty()) raw = composeState("inconnu", -1, 0, "");

    /* The frame rate is added to EVERY state: it is the measurement we want at
     * hand without having to ask for it. */
    {
        const double f = g_fps.load(std::memory_order_relaxed);
        if (f > 0.0) {
            char suf[32];
            snprintf(suf, sizeof suf, " fps=%.1f", f);
            raw += suf;
        }
    }

    char clean[DEVLINK_STATE_MAX];
    const size_t n = devlink_sanitize(clean, sizeof clean, raw.c_str());
    return std::string(clean, n);
}

/* --- Capture ------------------------------------------------------------- */

bool capture(std::vector<unsigned char> &png, int &w, int &h)
{
    png.clear();
    w = 0;
    h = 0;
    if (!available()) return false;

#if defined(DEVLINK_NO_GL)
    replyErr("shot", "no-gl");
    return false;
#else
    /* Dimensions of the framebuffer. We take them from GL_VIEWPORT rather than
     * from Borealis: `Application::windowWidth` is a LOGICAL size, which differs
     * from the pixel size as soon as the scale factor is not 1. The viewport is
     * set by glfwWindowFramebufferSizeCallback with the real size, and nothing
     * in  modifies it (checked). */
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    const int lw = (int)vp[2], lh = (int)vp[3];
    if (lw < 16 || lh < 16 || lw > DEVLINK_WIDTH_MAX || lh > DEVLINK_HEIGHT_MAX) {
        dllog("[devlink] err shot viewport");
        return false;
    }

    int shrink = g_shrink_served;
    if (shrink != 1 && shrink != 2) shrink = defaultShrink();

    const size_t out_size = devlink_upright_size(lw, lh, 3, shrink);
    if (out_size == 0) { replyErr("shot", "dimensions"); return false; }

    const long long t0 = nowMs();

    std::vector<unsigned char> raw;
    std::vector<unsigned char> image;
    try {
        raw.resize((size_t)lw * (size_t)lh * 3u);
        image.resize(out_size);
    } catch (...) {
        /* On console, a capture that fails for lack of memory must SAY so and
         * leave the application alive: it is a development tool, not a product
         * feature. */
        replyErr("shot", "memoire");
        return false;
    }

    /* GL_RGB rather than GL_RGBA: a quarter fewer bytes to read, to flip and to
     * compress, and the framebuffer's alpha channel carries no information here.
     * GL_PACK_ALIGNMENT is 1 because an odd width in RGB does not land on a
     * multiple of 4: with the default alignment every row would be shifted and
     * the image would come out slanted. */
    GLint prev_align = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    /* Only touch the read buffer when we really are on the framebuffer: asking
     * for GL_FRONT while a framebuffer object is bound is a
     * GL_INVALID_OPERATION, and the error would be blamed on the read. */
    GLint read_fbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    GLint read_prev = 0;
    const bool switch_buffer = (read_fbo == 0);
    if (switch_buffer) glGetIntegerv(GL_READ_BUFFER, &read_prev);

    /* One read from the named buffer. Returns the GL error, GL_NO_ERROR if the
     * read went through. Switching the read buffer only means something on the
     * framebuffer itself; with an FBO bound we read whatever it has. */
    const auto readFrom = [&](GLenum which) -> GLenum {
        if (switch_buffer) glReadBuffer(which);
        while (glGetError() != GL_NO_ERROR) { }   /* cleared, so the next one can be attributed */
        glReadPixels(vp[0], vp[1], lw, lh, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
        return glGetError();
    };

    /* Settled once: after the first capture the question is answered and every
     * later one costs a single `glReadPixels`. */
    const int choice = bufferChoice();
    if (choice != 2) g_read_buffer = choice;

    GLenum err = readFrom(g_read_buffer == 1 ? GL_BACK : GL_FRONT);

    /* The automatic decision. A first read that comes back one flat colour is
     * the signature of a buffer this driver does not serve; try the other one,
     * and keep it only if it actually has content - see the counter-case on
     * `bufferChoice`. */
    if (g_read_buffer < 0) {
        if (err == GL_NO_ERROR && hasContent(raw.data(), raw.size())) {
            g_read_buffer = 0;              /* the front works here */
        } else {
            std::vector<unsigned char> keep;
            const bool front_read = (err == GL_NO_ERROR);
            if (front_read) keep = raw;     /* so a legitimately uniform screen is not lost */

            const GLenum err_back = readFrom(GL_BACK);
            if (err_back == GL_NO_ERROR && hasContent(raw.data(), raw.size())) {
                g_read_buffer = 1;
                err = GL_NO_ERROR;
                dllog("[devlink] capture: GL_FRONT returns a flat picture on this driver - "
                      "GL_BACK kept from now on (SHADOW_DEVLINK_TAMPON to force)");
            } else if (front_read) {
                raw = keep;                 /* the screen really is one colour: nothing to decide yet */
                err = GL_NO_ERROR;
            } else {
                err = err_back;             /* neither buffer could be read: report it */
            }
        }
    }

    if (switch_buffer) glReadBuffer((GLenum)read_prev);
    glPixelStorei(GL_PACK_ALIGNMENT, prev_align);

    if (err != GL_NO_ERROR) {
        dllog("[devlink] err shot gl (0x%04x)", (unsigned)err);
        return false;
    }

    const long long t1 = nowMs();

    /* stb can flip by itself (`stbi_flip_vertically_on_write`), and we do NOT
     * use it, for two reasons. The first: it is a GLOBAL, hidden toggle, exactly
     * the kind of state this repo pays dearly for when it lingers (cf. the
     * family of defects around session `static`s). The second is decisive: the
     * flip and the shrink are done here in a SINGLE pass, where stb would impose
     * two passes over 6 MB. */
    if (!devlink_upright(raw.data(), lw, lh, 3, shrink, image.data(), image.size())) {
        replyErr("shot", "redressement");
        return false;
    }
    raw.clear();
    raw.shrink_to_fit();      /* released before encoding: this is the memory peak */

    const int fw = lw / shrink, fh = lh / shrink;

    /* Do not try the five PNG filters row by row. By default stb tries them all
     * and keeps the best, i.e. five passes over the image for a few percent of
     * bytes. Here we look at a capture, we do not archive it: the "Sub" filter
     * alone is enough and halves the encoding time. */
    stbi_write_png_compression_level = pngEffort();
    stbi_write_force_png_filter      = 1;

    PngSink out;
    out.bytes = &png;
    const int ok = stbi_write_png_to_func(writePng, &out, fw, fh, 3,
                                          image.data(), fw * 3);
    if (!ok || out.overflow || png.empty()) {
        png.clear();
        replyErr("shot", out.overflow ? "trop-gros" : "png");
        return false;
    }

    const long long t2 = nowMs();
    w = fw;
    h = fh;

    /* These two durations are the only way to know whether the frame-rate defect
     * we observe comes from the capture or from the change under test. Measuring
     * them costs two clock calls; not measuring them costs an investigation
     * session. */
    dllog("[devlink] capture %dx%d (reduction %d) — lecture %lld ms, png %lld ms, %zu octets",
               fw, fh, shrink, t1 - t0, t2 - t1, png.size());
    return true;
#endif
}

void sendCapture(const std::vector<unsigned char> &png, int w, int h)
{
    if (!available()) return;
    if (png.empty() || w <= 0 || h <= 0) { replyErr("shot", "vide"); return; }

    /* Prepare ALL the lines before announcing anything: if the base64 failed
     * halfway, the dev machine would already have received a `shot-begin` and
     * would wait for a `shot-end` that never comes. */
    std::vector<std::string> lines;
    lines.reserve(png.size() / DEVLINK_BYTES_PER_LINE + 2);

    char b64[DEVLINK_B64_PER_LINE + 8];
    for (size_t o = 0; o < png.size(); o += DEVLINK_BYTES_PER_LINE) {
        size_t n = png.size() - o;
        if (n > DEVLINK_BYTES_PER_LINE) n = DEVLINK_BYTES_PER_LINE;
        const size_t written = devlink_base64(png.data() + o, n, b64, sizeof b64);
        if (written == 0) { replyErr("shot", "base64"); return; }
        lines.emplace_back(std::string("[devlink] shot-data ") + b64);
    }
    lines.emplace_back("[devlink] shot-end");

    /* The header goes out immediately: it fits on one line, and it gives the
     * other end what it needs to size its reception before the first byte. */
    dllog("[devlink] shot-begin %d %d %zu", w, h, png.size());

    {
        std::lock_guard<std::mutex> v(g_queue_lock);
        g_queue = std::move(lines);
        g_cursor = 0;
    }
}

/* --- Demandes ------------------------------------------------------------ */

void requestCapture(int shrink)
{
    if (!available()) return;
    if (shrink != 1 && shrink != 2) shrink = defaultShrink();
    g_capture_request.store(shrink);
}

void requestState()
{
    if (!available()) return;
    g_state_request.store(true);
}

/* --- The log thread's side ------------------------------------------------ */

bool handleCommand(const char *line)
{
    if (!available()) return false;
    if (!line) return false;

    /* devcmd_parse works on an EXPLICIT length: it does not assume the line is
     * null-terminated. So we measure it in a bounded way - the buffer it comes
     * from (journal.c's command reader) is 256 bytes, and a line longer than the parser's bound
     * will be refused by it. */
    size_t n = 0;
    while (n <= DEVCMD_LINE_MAX && line[n]) n++;

    devcmd_t cmd;
    if (!devcmd_parse(line, n, &cmd)) return false;

    /* This module serves ONLY `shot` and `state`. `btn`, `nav` and `tap` belong
     * to the input-injection module, and `quit` predates it: we return false so
     * the caller chains on to them. */
    switch (cmd.kind) {
    case DEVCMD_SHOT:
        /* One capture at a time. Without this guard, two `shot`s close together
         * would interleave their `shot-data` lines and the other end would
         * rebuild a PNG made of two different halves - a file that does not open,
         * with nothing to say why. */
        if (transferInProgress() || g_capture_request.load() != 0) {
            replyErr("shot", "occupe");
            return true;
        }
        requestCapture(0);          /* the factor comes from the setting, not from the network */
        return true;

    case DEVCMD_STATE:
        requestState();
        return true;

    default:
        return false;
    }
}

/* --- The render-thread side ----------------------------------------------- */

void onFrame()
{
    if (!available()) return;

    /* 1. Serve at most ONE request per frame.
     *
     * This is where the module's central trap is resolved: the command arrived
     * on the log drain thread, where no OpenGL context is current.
     * `glReadPixels` only means something on the drawing thread, and the screen
     * description reads data that same thread is in the middle of modifying.
     * Both are therefore done HERE, one frame later. */
    const int shrink = g_capture_request.exchange(0);
    if (shrink != 0) {
        std::vector<unsigned char> png;
        int w = 0, h = 0;
        g_shrink_served = shrink;
        if (capture(png, w, h)) sendCapture(png, w, h);
        g_shrink_served = 0;
        /* On failure, `capture` has already emitted its `err`: do not emit a
         * second one, the other end counts replies. */
    }

    if (g_state_request.exchange(false)) {
        const std::string s = state();
        dllog("[devlink] state %s", s.c_str());
    }

    /* 2. Drain the capture in progress at a bounded rate. See `sendCapture`
     * for how the number was chosen. */
    const size_t budget = (size_t)linesPerFrame();
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> v(g_queue_lock);
        for (size_t i = 0; i < budget && g_cursor < g_queue.size(); i++)
            batch.emplace_back(std::move(g_queue[g_cursor++]));
        if (g_cursor >= g_queue.size()) {
            g_queue.clear();
            g_cursor = 0;
        }
    }
    /* Emission OUTSIDE the lock: every line is a blocking `send()`, and holding
     * a lock across blocking I/O is wss.c's documented mistake. */
    for (size_t i = 0; i < batch.size(); i++)
        dllog("%s", batch[i].c_str());
}

}  // namespace devlink
