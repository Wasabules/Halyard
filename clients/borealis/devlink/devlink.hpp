/* devlink - driving the UI from the development machine.
 *
 * === WHY THIS MODULE EXISTS ===
 *
 * Checking a visual change cost a human gesture: launch the application, look at
 * the screen, describe what you see. An assistant cannot loop on that, and it is
 * exactly the loop we repeat most often. This module gives three capabilities,
 * the minimal equivalent of an `adb`:
 *
 *   - `shot`   take the screen as displayed, and BRING IT BACK;
 *   - `state`  describe the screen in ONE line of text;
 *   - `btn` / `nav` / `tap`  act (input injection: see below, this module only
 *              PARSES them, it does not execute them).
 *
 * `state` is often MORE useful than `shot`: a line of text is asserted with a
 * string comparison, where an image needs a human eye. The capture serves to
 * understand a layout defect; the state serves to drive.
 *
 * === THE CHANNEL IS THE LOG'S, AND IT DOES NOT OPEN ONE ===
 *
 * Nothing new listens on the network. We reuse the OUTGOING socket of
 * `core/services/journal.c`: the application connects to the address read from
 * SHADOW_DATA_DIR "logsink.txt", pours its log lines into it and reads its
 * commands back. Hence three security properties that are not negotiable, and
 * which this module EXTENDS rather than adding to:
 *
 *   1. no `logsink.txt`, no channel - and then this module costs ZERO:
 *      `available()` returns false and everything else returns at once;
 *   2. the application CONNECTS, it never listens: there is nothing to reach
 *      from outside, even knowing the console's address;
 *   3. the commands come from the network, so they are HOSTILE by default.
 *      Parsing is done by `devcmd.h` next door: it bounds every length, works on
 *      an EXPLICIT length rather than on a null terminator, refuses instead of
 *      truncating and accepts only words from a closed list. An unrecognised
 *      line does NOTHING.
 *
 *      THERE IS ONLY ONE PARSER, and that is the one. This file carried a second
 *      one at first; two parsers of the same dialect end up diverging with
 *      nothing to signal it - the repo has already paid for that with two kind
 *      tables. devcmd.h's is the harder of the two (explicit length, ~150 checks
 *      under ASan in tests/test_devcmd.c), so mine is the one that went away.
 *
 * log.c's rule of caution applies here verbatim: "a log that stalls the
 * application it observes would be worse than no log". A command channel that
 * drops the frame rate would be worse than no channel - which is why sending a
 * capture is SPREAD over several frames (see `sendCapture`).
 *
 * === PROTOCOL (all three pieces must agree on it) ===
 *
 * Machine -> application, ONE command per line, terminated by \n:
 *
 *     shot           captures the screen (the shrink factor is a setting,
 *                    SHADOW_DEVLINK_REDUCTION, not an argument of the command)
 *     state          describes the current screen
 *     btn <name>     a b x y l r zl zr plus minus
 *     nav <dir>      haut bas gauche droite
 *     tap <x> <y>    touch at the given screen position
 *     quit           stop (existed before this module, cf. main.cpp)
 *
 * Application -> machine, ordinary log lines prefixed [devlink]:
 *
 *     [devlink] ok <command>
 *     [devlink] err <command> <one-word reason>
 *     [devlink] shot-begin <width> <height> <png-bytes>
 *     [devlink] shot-data <base64>          (repeated, at most 512 characters)
 *     [devlink] shot-end
 *     [devlink] state <text on one line>
 *
 * The base64 is cut into slices of 384 raw bytes, because 384 * 4 / 3 is EXACTLY
 * 512: every line is therefore exactly the maximum length, with no `=` padding
 * in the middle of the stream, and every line decodes independently. Only the
 * last one can be shorter and padded.
 *
 * === HOW THE WORK IS SPLIT BETWEEN THE TWO HALVES OF THIS FILE ===
 *
 * Like `ui/screen.hpp`, this file STARTS IN C. Everything pure - the base64, the
 * image flip, formatting a state line - is ordinary C, checkable offline with no
 * console, no OpenGL context and no network (cf. `tests/`). The C++ part below
 * contains only what really touches the hardware: OpenGL, the clock, the log
 * socket.
 *
 * This is deliberate, and it is what makes the capture chain checkable
 * end to end on the desktop: you can build yourself a fake OpenGL buffer, pass
 * it through `devlink_upright` then `devlink_base64`, and read back the
 * resulting PNG without ever turning the console on. That is how the row flip
 * was proven rather than assumed.
 *
 * Parsing the received commands is NOT here: it lives in `devlink/devcmd.h`,
 * which is the channel's only parser.
 */
#ifndef DEVLINK_HPP
#define DEVLINK_HPP

#include <stddef.h>
#include <stdio.h>   /* snprintf, for the state line */

/* =========================================================================
 * THE PURE PART - no global state, no I/O, no allocation, no graphics type.
 * Nothing here calls getenv, malloc, or a clock function.
 * ========================================================================= */

/* --- Bounds -------------------------------------------------------------
 *
 * Any value outside these bounds is REFUSED, never clipped - the same rule as
 * devcmd.h, for the same reason: truncated data stays plausible, and a truncated
 * capture produces a PNG that will not open without saying why. */
#define DEVLINK_BYTES_PER_LINE 384   /* 384 octets bruts -> 512 caracteres pile */
#define DEVLINK_B64_PER_LINE    512
#define DEVLINK_STATE_MAX         400   /* the state line, excluding the [devlink] prefix */
#define DEVLINK_WIDTH_MAX      4096  /* guard rail on the framebuffer */
#define DEVLINK_HEIGHT_MAX      2304

/* --- base64 --------------------------------------------------------------
 *
 * Standard alphabet (RFC 4648), with padding. Returns the number of characters
 * written excluding the final zero, or 0 when the capacity is insufficient -
 * never a truncated output, which would give a corrupt PNG nobody could explain
 * at the other end. */
static inline size_t devlink_base64_size(size_t n) { return 4 * ((n + 2) / 3); }

static inline size_t devlink_base64(const unsigned char *src, size_t n,
                                    char *dst, size_t cap)
{
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t needed = devlink_base64_size(n);
    size_t i = 0, o = 0;

    if (!dst || cap == 0) return 0;
    if (!src && n) return 0;
    if (needed + 1 > cap) return 0;

    while (i + 2 < n) {
        const unsigned v = ((unsigned)src[i] << 16) | ((unsigned)src[i + 1] << 8)
                         | (unsigned)src[i + 2];
        dst[o++] = alpha[(v >> 18) & 63];
        dst[o++] = alpha[(v >> 12) & 63];
        dst[o++] = alpha[(v >> 6) & 63];
        dst[o++] = alpha[v & 63];
        i += 3;
    }
    if (i < n) {
        const unsigned rem = (unsigned)(n - i);          /* 1 or 2 */
        unsigned v = (unsigned)src[i] << 16;
        if (rem == 2) v |= (unsigned)src[i + 1] << 8;
        dst[o++] = alpha[(v >> 18) & 63];
        dst[o++] = alpha[(v >> 12) & 63];
        dst[o++] = (rem == 2) ? alpha[(v >> 6) & 63] : '=';
        dst[o++] = '=';
    }
    dst[o] = 0;
    return o;
}

/* --- Turning the image the right way up ----------------------------------
 *
 * OpenGL returns the rows BOTTOM TO TOP: `glReadPixels` delivers row 0 at the
 * very bottom of the screen. An unflipped capture therefore gives an upside-down
 * image.
 *
 * That is obvious on a photograph, and NOT AT ALL obvious on a user interface: a
 * centred list, a gradient background, a bar at the top and one at the bottom -
 * upside down, all of that looks like a botched layout, not a flipped image. You
 * would look for the defect in the layout code. Hence this function, and hence
 * the explicit counter-case in the tests.
 *
 * `shrink` is 1 (flip only) or 2 (flip + 2x2 average). The average, rather than
 * taking one pixel in four: interface text is thin, and nearest-neighbour
 * sampling wipes out every other glyph row - the capture becomes illegible,
 * which removes its only value. The average costs one extra read per output
 * pixel and returns text you can still read.
 *
 * An ODD dimension loses its last row (or column) rather than reading a pixel
 * outside the buffer: `fh = h / 2` in integer division guarantees that
 * `2*y + 1 <= h - 1`.
 *
 * Returns 1, or 0 when an argument is invalid or the capacity insufficient. */
static inline size_t devlink_upright_size(int w, int h, int comp, int shrink)
{
    if (w <= 0 || h <= 0 || (comp != 3 && comp != 4)) return 0;
    if (shrink != 1 && shrink != 2) return 0;
    if (w > DEVLINK_WIDTH_MAX || h > DEVLINK_HEIGHT_MAX) return 0;
    if (w / shrink < 1 || h / shrink < 1) return 0;
    return (size_t)(w / shrink) * (size_t)(h / shrink) * (size_t)comp;
}

static inline int devlink_upright(const unsigned char *src, int w, int h, int comp,
                                    int shrink, unsigned char *dst, size_t cap)
{
    const size_t needed = devlink_upright_size(w, h, comp, shrink);
    int fw, fh, x, y, c;

    if (!src || !dst || needed == 0 || cap < needed) return 0;
    fw = w / shrink;
    fh = h / shrink;

    if (shrink == 1) {
        for (y = 0; y < fh; y++) {
            const unsigned char *line = src + (size_t)(h - 1 - y) * (size_t)w * (size_t)comp;
            unsigned char       *out = dst + (size_t)y * (size_t)fw * (size_t)comp;
            for (x = 0; x < fw * comp; x++) out[x] = line[x];
        }
        return 1;
    }

    /* Upright row y covers upright rows 2y and 2y+1, i.e. OpenGL rows
     * (h-1-2y) and (h-2-2y). Both exist as long as 2y+1 <= h-1, which
     * fh = h/2 guarantees. */
    for (y = 0; y < fh; y++) {
        const unsigned char *l0 = src + (size_t)(h - 1 - 2 * y) * (size_t)w * (size_t)comp;
        const unsigned char *l1 = src + (size_t)(h - 2 - 2 * y) * (size_t)w * (size_t)comp;
        unsigned char       *out = dst + (size_t)y * (size_t)fw * (size_t)comp;
        for (x = 0; x < fw; x++) {
            const size_t a = (size_t)(2 * x) * (size_t)comp;
            const size_t b = a + (size_t)comp;
            for (c = 0; c < comp; c++) {
                const int sum = (int)l0[a + (size_t)c] + (int)l0[b + (size_t)c]
                                + (int)l1[a + (size_t)c] + (int)l1[b + (size_t)c];
                out[(size_t)x * (size_t)comp + (size_t)c] = (unsigned char)(sum / 4);
            }
        }
    }
    return 1;
}

/* --- The state line ------------------------------------------------------
 *
 * The protocol says "<free text on one line>". "On one line" is the part that
 * matters, and it is NOT free: the label comes from the displayed content, hence
 * from a virtual machine name returned by the server. A newline inside that name
 * would cut the reply in two, and the dev machine would read the bottom half as
 * an unknown message. So we sanitise, we do not trust.
 *
 * Bytes above 0x7e are KEPT: the labels are UTF-8 and contain accents. Only the
 * control bytes are replaced by a space, and 0x7f (delete) with them. */
static inline size_t devlink_sanitize(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    if (!dst || cap == 0) return 0;
    dst[0] = 0;
    if (!src) return 0;
    while (src[o] && o + 1 < cap) {
        const unsigned char c = (unsigned char)src[o];
        dst[o] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
        o++;
    }
    dst[o] = 0;
    return o;
}

/* Composes the state line as key=value, stable and easy to assert from a
 * script:
 *
 *     ecran=liste-vm nb=3 focus=1 libelle=My machine
 *
 * `libelle` comes LAST, expressing that it is the only field that may contain
 * spaces: a script therefore cuts on the first "libelle=" and keeps all the
 * rest, without having to escape anything.
 *
 * Returns the number of characters written excluding the final zero, or 0 when
 * the capacity is not even enough for the numeric fields. */
static inline size_t devlink_format_state(char *dst, size_t cap, const char *screen,
                                           int focus, int nb, const char *label)
{
    char   name[64];
    char   lbl[DEVLINK_STATE_MAX];
    size_t o = 0;
    int    written;

    if (!dst || cap == 0) return 0;
    dst[0] = 0;
    devlink_sanitize(name, sizeof name, screen ? screen : "inconnu");
    if (name[0] == 0) { name[0] = '?'; name[1] = 0; }
    devlink_sanitize(lbl, sizeof lbl, label ? label : "");

    /* The bounds protect the reader as much as us: an absurd `nb` is a
     * symptom (a list refreshed out from under the focus), not a reason to
     * write anything at all. We report it as is, bounded. */
    if (nb    < -1)      nb    = -1;
    if (nb    > 999999)  nb    = 999999;
    if (focus < -1)      focus = -1;
    if (focus > 999999)  focus = 999999;

    written = snprintf(dst, cap, "screen=%s nb=%d focus=%d label=%s", name, nb, focus, lbl);
    if (written < 0) { dst[0] = 0; return 0; }
    o = (size_t)written;
    if (o >= cap) o = cap - 1;                 /* snprintf truncated: we say so */
    return o;
}

/* =========================================================================
 * THE C++ PART - OpenGL, the clock, the log socket.
 * ========================================================================= */
#ifdef __cplusplus

#include <functional>
#include <string>
#include <vector>

namespace devlink {

/* Does the channel exist? False as long as SHADOW_DATA_DIR "logsink.txt" is
 * absent, and then all the rest of this module returns immediately. The result
 * is cached: it is a STARTUP constant (the presence of a file on the SD card),
 * not session state - the distinction matters in this repo, where a function
 * `static` carrying session state has caused four failures. */
bool available();

/* --- The log thread's side ------------------------------------------------ */

/* Interprets a received line. To be called from the handler installed by
 * `journal_set_command_handler`, hence from the DRAIN THREAD: it only records
 * a request and returns immediately.
 *
 * Returns true when the line was HANDLED by this module (`shot`, `state`).
 * Returns false for everything else - including `btn`, `nav`, `tap` and `quit`,
 * which belong to other modules - so the caller can chain. The parsing itself is
 * common: `devcmd_parse` from devlink/devcmd.h. */
bool handleCommand(const char *line);

/* --- The render thread's side --------------------------------------------
 *
 * `onFrame()` must be called ONCE per frame, on the thread that owns the OpenGL
 * context. It is the module's central piece and its main trap: the command
 * arrives on the log thread, while `glReadPixels` only means something on the
 * render thread and only while a context is current. Calling the capture from
 * the drain thread would not return a wrong image - it would return a null
 * context, hence nothing at all, or worse a crash depending on the driver. Hence
 * the queue: we record the request and serve it on the next frame.
 *
 * `onFrame()` does two things:
 *   1. serves at most ONE request (capture or state);
 *   2. drains at most N lines of the capture in progress (see `sendCapture`). */
void onFrame();

/* Reads the framebuffer and returns a PNG. MUST be called from the render
 * thread. Returns false and logs an `err` on any problem; `png` is then empty.
 * `w` and `h` receive the dimensions AFTER shrinking, those of the PNG. */
bool capture(std::vector<unsigned char> &png, int &w, int &h);

/* Emits the `shot-begin` header then queues the `shot-data` and `shot-end`
 * lines.
 *
 * The send is NOT immediate, and that is the important point. Every log line
 * goes back out through the dev machine's socket, with a blocking `send()` under
 * the log's lock. A 720p capture is on the order of 500 lines; emitting them in
 * one go from the render thread means 500 `send()` calls inside a single frame,
 * i.e. several hundred milliseconds of freeze - a very visible hitch, and worse:
 * one we would blame on the change we are in the middle of measuring. The drain
 * is therefore spread out by `onFrame()`. */
void sendCapture(const std::vector<unsigned char> &png, int w, int h);

/* --- Describing the screen ----------------------------------------------
 *
 * THIS MODULE KNOWS NO SCREEN, and must never know one: it declares no UI type
 * and includes neither borealis nor nanovg. It is the current screen that comes
 * and describes itself, in one of the two ways below.
 *
 * (1) BY CALLBACK - `setStateDescriber()`. The screen installs a function on
 *     entry and calls `clearStateDescriber()` on exit. The line is then always
 *     fresh, computed at the moment it is asked for.
 *
 *     MIND THE LIFETIME. A callback that captures `this` and outlives the screen
 *     that installed it is exactly the defect that produced three "Instruction
 *     Abort" crashes in a single day in this repo (cf. the header of
 *     ui/screen.hpp): a pointer to a destroyed view. If the screen cannot
 *     GUARANTEE that it removes its callback before dying, use (2).
 *
 * (2) BY PUBLICATION - `publishState()`. The screen pushes its line whenever it
 *     changes (on every focus move, on every list refresh). Nothing is retained
 *     towards the screen: only copied text. No lifetime, therefore no possible
 *     dangling pointer. This is the RECOMMENDED form, and the one that follows
 *     the doctrine of the rest of the UI: no hidden state, no implicit lifetime.
 *
 * Priority: the callback if one is installed, otherwise the last published line,
 * otherwise an "unknown" line - never nothing, so a script always receives a
 * parsable answer rather than a silence to interpret. */
using Describer = std::function<std::string()>;

void setStateDescriber(Describer d);
void clearStateDescriber();
void publishState(const std::string &line);

/* The current state line, sanitised and bounded. Called from the render thread
 * by `onFrame()`; the callback is copied then invoked OUTSIDE the lock, so we
 * never hold a lock across code we do not control. */
/* To be called ONCE per frame, with the clock the screen has already read (a
 * second read would be one frame off). Feeds the `fps=` of state(). */
void noteFrame(double t);

std::string state();

/* C++ wrapper around `devlink_format_state`, so every screen produces the same
 * shape. A single format is what makes a test script readable. */
std::string composeState(const char *screen, int focus, int nb, const char *label);

/* --- Demandes (appelables depuis n'importe quel fil) ---------------------- */

/* `shrink`: 1 = real size, 2 = half, 0 = the default value
 * (SHADOW_DEVLINK_REDUCTION, 2 by default). The `shot` command always passes 0:
 * the protocol carries no argument, the factor is a setting. Served on the next
 * frame. */
void requestCapture(int shrink);
void requestState();

/* --- Protocol replies -----------------------------------------------------
 * Exposed so the input-injection module replies in exactly the same shape: a
 * machine that has to recognise two dialects automates nothing. `reason` must
 * fit in ONE word (hyphens allowed) - the other end reads it as a token, not as
 * a sentence. */
void replyOk(const char *command);
void replyErr(const char *command, const char *reason);

}  // namespace devlink

#endif  /* __cplusplus */
#endif  /* DEVLINK_HPP */
