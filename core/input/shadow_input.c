/* shadow_input - the UI's input queue, and the thread that drains it.
 *
 * Borealis posts mouse, keyboard and wheel events from the UI thread; the
 * session's drain thread takes them off this queue and hands them to
 * `native_input_*`, which writes them to the `:base+12` input channel.
 *
 * 2026-09-26 - this file used to carry, alongside that queue, the WebRTC
 * data-channel encoder of the path abandoned in May: FlatBuffer templates
 * captured from Chrome, their timestamp and sequence patching, and the SCTP
 * sends. None of it was reachable from the native path - the drain loop took
 * the native branch and `continue`d before reaching it - and it is gone with
 * the rest of that path. The KB keeps what those captures taught.
 */
#include "shadow_input.h"
#include "../protocol/latency.h"   /* L5 : instrumentation du chemin d'entree */
#include "../services/log.h"
#include "../services/time_sync.h"
#include "../protocol/native_input.h"  /* I1 phase 3 2026-05-18 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __SWITCH__
#ifdef __SWITCH__
#include <switch.h>
#endif
#endif

/* S81 - the category is DECLARED here, not inferred from the message text.
 * `silog` stays at INFO: the existing calls do not disappear. `sidbg` is there
 * for the bulky lines, which move over to it one at a time. */
#define silog(...) JOURNAL_INFO_(JOURNAL_CAT_INPUT, __VA_ARGS__)
#define sidbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_INPUT, __VA_ARGS__)
// Client-side cursor state: we track the absolute position so we can serve the
// delta APIs.
// I4 FIX 2026-06-02: initialised at the centre of 1920x1080 (960,540) to MATCH
// ctrl_input_tcp.c (cursor_x=960, cursor_y=540). It used to be 640,360 (the
// centre of 1280x720) -> mismatch -> the native path accumulated a permanent
// offset of (320,180) between the two trackers -> clicks landing ~320 px right
// and 180 px low -> "nothing happens" where you click.
static int      g_cursor_x   = 960;
static int      g_cursor_y   = 540;

// === Public API ===

/* Remote mouse mode: 0 = absolute positions (desktop), 1 = relative deltas (a
 * game that captures the mouse).
 *
 * Pushed from the UI rather than read from the settings: this file is C and also
 * links into the headless binary, which does not carry the C++ layer. A pushed
 * setting avoids that boundary problem without a stub. */
static int g_mouse_relative = 0;

void shadow_input_set_mouse_relative(int on) { g_mouse_relative = on ? 1 : 0; }
int  shadow_input_get_mouse_relative(void)   { return g_mouse_relative; }

void shadow_input_get_cursor_pos(int *x, int *y) {
    if (x) *x = g_cursor_x;
    if (y) *y = g_cursor_y;
}

/* BUG5 2026-05-18 - setter for the native path (cf. shadow_input.h). */
/* PM2 - the remote desktop's size. 1920x1080 until a frame says otherwise: it is
 * the resolution we ask for by default, so the estimate is right from the first
 * move rather than after the first frame. */
static int g_bound_w = 1920, g_bound_h = 1080;

void shadow_input_set_bounds(int w, int h) {
    if (w > 0 && h > 0) { g_bound_w = w; g_bound_h = h; }
}

void shadow_input_set_cursor_pos(int x, int y) {
    if (x < 0) x = 0;
    if (x > g_bound_w - 1) x = g_bound_w - 1;
    if (y < 0) y = 0;
    if (y > g_bound_h - 1) y = g_bound_h - 1;
    g_cursor_x = x;
    g_cursor_y = y;
}

/* Whether an input session is live - the UI only posts while one is.
 * It used to be a `sctp_assoc *` set to the mock value 0x1 on the native path. */
static volatile int g_session_active = 0;

/* === UI command queue ===
 * The Borealis UI thread (StreamView::draw at 60 Hz, the overlay keyboard, the
 * touch handler) MUST NOT call dtls_ctx_send_app directly: wolfSSL is not
 * thread-safe (its internal _malloc_r buffers get corrupted). Race observed on
 * 2026-05-03 -> Atmosphere crash 0x4A8 on a null fault address inside malloc.
 *
 * Solution: a ring queue of UI events, drained by the session's own drain
 * thread, the only one that writes to the input channel. */
enum cmd_kind {
    CMD_NONE = 0,
    CMD_MOUSE_MOVE,
    CMD_MOUSE_MOVE_ABS,
    CMD_MOUSE_BUTTON,
    CMD_CLICK,
    CMD_KEYPRESS,
    CMD_KEYPRESS_SHIFTED,
    CMD_SCANCODE,        /* a separate down OR up (hold) */
    CMD_WHEEL,
};
struct ui_cmd {
    enum cmd_kind kind;
    int       a, b;       /* dx/dy / x/y / button / scancode */
    bool      pressed;
    /* === L5 2026-08-29 - STAMPED WHEN POSTED ===
     * Stamped by `cmd_post`, the MANDATORY passage of every input event, called
     * from the UI thread immediately after the device read: the gap between the
     * real read and this instant is a few instructions. Read back at the moment
     * the event is WRITTEN ON THE SOCKET, which yields the only thing the player
     * actually experiences: finger -> byte on the wire, client side.
     * It lives in the command rather than in a shared variable: between the post
     * and the send there is a thread and a 10 ms sleep, so a shared variable
     * would describe the NEXT event. */
    int64_t   t_posted_us;
};
#define CMD_QUEUE_CAP 256
static struct ui_cmd  g_cmd_queue[CMD_QUEUE_CAP];
static int            g_cmd_head = 0;  /* read */
static int            g_cmd_tail = 0;  /* write */
static pthread_mutex_t g_cmd_lock = PTHREAD_MUTEX_INITIALIZER;
/* === L16 2026-08-29 - THE DRAIN THREAD NO LONGER WAITS ON A CLOCK ===
 *
 * It used to sleep a fixed 10 ms between two rounds, and its original comment
 * owned up to it: "input latency ~5 ms average (acceptable for gaming)". Five
 * milliseconds on average, ten at worst, added to EVERY gesture before the byte
 * even leaves - for nothing, since nothing forces you to wait on a clock when you
 * can wait on the event itself.
 *
 * This is the same defect as L6 on video reception (sleeping 5 ms rather than
 * waiting on the packets, -33 % on reassembly) and as L9 on the gamepad (read at
 * the screen's rate rather than its own). The same shape three times: a path
 * paced by an arbitrary clock instead of by its source.
 *
 * The poster signals, the thread wakes. The maximum delay stays bounded at 10 ms
 * - not for latency, but because a long-lived thread must revisit its abort flag
 * at coarse granularity on this console, failing which HOS leaks its handle. */
static pthread_cond_t  g_cmd_cv = PTHREAD_COND_INITIALIZER;
static shadow_input_debug g_dbg = {0};

static void cmd_post(const struct ui_cmd *cmd) {
    /* L5: the clock is read BEFORE the lock. Reading it after would count the
     * lock wait outside the travel time, when it is part of what we want to
     * measure. */
    const int64_t t_posted = latency_enabled() ? latency_now_us() : 0;
    pthread_mutex_lock(&g_cmd_lock);
    int next = (g_cmd_tail + 1) % CMD_QUEUE_CAP;
    if (next != g_cmd_head) {  /* drop when full */
        g_cmd_queue[g_cmd_tail] = *cmd;
        g_cmd_queue[g_cmd_tail].t_posted_us = t_posted;
        g_cmd_tail = next;
    }
    /* Update debug counters + last_action label. */
    g_dbg.queue_depth = (g_cmd_tail - g_cmd_head + CMD_QUEUE_CAP) % CMD_QUEUE_CAP;
    switch (cmd->kind) {
    case CMD_MOUSE_MOVE:
        g_dbg.n_mouse_moves++;
        snprintf(g_dbg.last_action, sizeof(g_dbg.last_action),
                 "move(dx=%d,dy=%d)", cmd->a, cmd->b);
        break;
    case CMD_MOUSE_MOVE_ABS:
        g_dbg.n_mouse_moves++;
        snprintf(g_dbg.last_action, sizeof(g_dbg.last_action),
                 "moveAbs(%d,%d)", cmd->a, cmd->b);
        break;
    case CMD_CLICK:
        if (cmd->a == 0) g_dbg.n_clicks_left++; else g_dbg.n_clicks_right++;
        snprintf(g_dbg.last_action, sizeof(g_dbg.last_action),
                 "click(btn=%d)", cmd->a);
        break;
    case CMD_MOUSE_BUTTON:
        if (cmd->pressed) g_dbg.n_btn_down++; else g_dbg.n_btn_up++;
        snprintf(g_dbg.last_action, sizeof(g_dbg.last_action),
                 "btn(%d,%s)", cmd->a, cmd->pressed ? "DOWN" : "UP");
        break;
    case CMD_KEYPRESS:
    case CMD_KEYPRESS_SHIFTED:
        g_dbg.n_keypress++;
        snprintf(g_dbg.last_action, sizeof(g_dbg.last_action),
                 "kbd(scan=%d)", cmd->a);
        break;
    case CMD_SCANCODE:
        g_dbg.n_keypress++;
        snprintf(g_dbg.last_action, sizeof(g_dbg.last_action),
                 "scan(%d,%s)", cmd->a, cmd->pressed ? "DOWN" : "UP");
        break;
    case CMD_WHEEL:
        snprintf(g_dbg.last_action, sizeof(g_dbg.last_action),
                 "wheel(dir=%d)", cmd->a);
        break;
    default: break;
    }
    /* L16 - wake the drain NOW. Signal while holding the lock: the sleeper tests
     * the queue under that same lock, and signalling after releasing it leaves
     * open the case where it falls asleep between the test and the signal. */
    pthread_cond_signal(&g_cmd_cv);
    pthread_mutex_unlock(&g_cmd_lock);
}

/* L16 - waits for an event to arrive, or for the delay to expire. Returns
 * immediately if the queue is not empty. `timeout_ms` bounds the wait so the
 * caller can revisit its abort flag. */
void shadow_input_wait_cmd(int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)timeout_ms * 1000 * 1000;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_cmd_lock);
    if (g_cmd_head == g_cmd_tail)
        pthread_cond_timedwait(&g_cmd_cv, &g_cmd_lock, &ts);
    pthread_mutex_unlock(&g_cmd_lock);
}

void shadow_input_get_debug(shadow_input_debug *out) {
    pthread_mutex_lock(&g_cmd_lock);
    *out = g_dbg;
    out->queue_depth = (g_cmd_tail - g_cmd_head + CMD_QUEUE_CAP) % CMD_QUEUE_CAP;
    pthread_mutex_unlock(&g_cmd_lock);
}

void shadow_input_set_session_active(bool active) {
    g_session_active = active ? 1 : 0;
    /* Flush the queue whenever a session starts or ends: nothing posted for the
     * previous one may reach the next. */
    pthread_mutex_lock(&g_cmd_lock);
    g_cmd_head = g_cmd_tail = 0;
    pthread_mutex_unlock(&g_cmd_lock);
}

/* Called by the session's drain thread. Executes the UI events posted from
 * Borealis, through the native input channel. */
void shadow_input_drain_queue(void) {
    if (!native_input_active()) return;
    for (int i = 0; i < 32; i++) {  /* at most 32 events per tick */
        struct ui_cmd cmd;
        pthread_mutex_lock(&g_cmd_lock);
        if (g_cmd_head == g_cmd_tail) {
            pthread_mutex_unlock(&g_cmd_lock);
            return;
        }
        cmd = g_cmd_queue[g_cmd_head];
        g_cmd_head = (g_cmd_head + 1) % CMD_QUEUE_CAP;
        pthread_mutex_unlock(&g_cmd_lock);

        {
            /* Forwarded through native_input.c -> ctrl_input_tcp */
            switch (cmd.kind) {
            case CMD_MOUSE_MOVE:
                native_input_send_mouse_move(cmd.a, cmd.b);
                /* === PM2 2026-09-02 - A RELATIVE MOVE MUST ADVANCE OUR TRACKER
                 *
                 * It did not, and CMD_MOUSE_MOVE_ABS did - so the tracker only
                 * ever moved when the input was already absolute. Everything
                 * that sends DELTAS (the controller mouse, the touchscreen in
                 * relative mode, a game that captures the pointer) moved the
                 * cursor in the VM while our own estimate stayed where it was
                 * initialised.
                 *
                 * Nothing broke, which is why it lasted: the deltas we send are
                 * correct and the remote pointer goes exactly where it should.
                 * The only thing that reads the tracker is what we DRAW - hence
                 * the report: "the Shadow cursor does not move, yet I can see my
                 * mouse exists, it is just invisible". The sprite was pinned at
                 * (960,540) for the whole session.
                 *
                 * Clamped like the absolute path: an unclamped tracker walks off
                 * to infinity when you push against an edge - the VM stops at the
                 * border, our estimate does not, and coming back would then take
                 * as long as you spent pushing. */
                shadow_input_set_cursor_pos(g_cursor_x + cmd.a,
                                            g_cursor_y + cmd.b);
                break;
            case CMD_MOUSE_MOVE_ABS: {
                /* C3 2026-08-21 - we forward the position AS IS. We used to
                 * convert it into a delta against a local counter, while
                 * `ctrl_input_tcp` keeps its own: the two diverged (a constant
                 * offset plus drift on every clamped delta), which is where the
                 * "offset between the mouse, the displayed cursor and the click"
                 * came from. SHADOW_INPUT_ABS=0 goes back to deltas. */
                /* Absolute by default (desktop), relative as soon as a game
                 * captures the mouse: see shadow_input_set_mouse_relative(). The
                 * environment variable stays authoritative for a trial without
                 * going through the UI. */
                static int g_abs_env = -2;
                if (g_abs_env == -2) {
                    const char *e = getenv("SHADOW_INPUT_ABS");
                    g_abs_env = e ? atoi(e) : -1;
                }
                const int g_abs = (g_abs_env >= 0) ? g_abs_env
                                                   : (g_mouse_relative ? 0 : 1);
                int cur_x = 0, cur_y = 0;
                shadow_input_get_cursor_pos(&cur_x, &cur_y);
                int dx = cmd.a - cur_x, dy = cmd.b - cur_y;
                if (g_abs) {
                    native_input_send_mouse_move_abs(cmd.a, cmd.b);
                    shadow_input_set_cursor_pos(cmd.a, cmd.b);
                } else if (dx || dy) {
                    native_input_send_mouse_move(dx, dy);
                    /* BUG5 2026-05-18 - sync the tracker for the next delta.
                     * Otherwise we compute deltas against the stale init
                     * (= 640,360) -> the server's absolute positions diverge ->
                     * elements no longer react to hover or click. */
                    shadow_input_set_cursor_pos(cmd.a, cmd.b);
                }
                break;
            }
            case CMD_MOUSE_BUTTON:
                native_input_send_mouse_button(cmd.a, cmd.pressed);
                break;
            case CMD_CLICK:
                native_input_send_mouse_button(cmd.a, true);
                native_input_send_mouse_button(cmd.a, false);
                break;
            case CMD_KEYPRESS:
                native_input_send_scancode((uint16_t)cmd.a, true);
                native_input_send_scancode((uint16_t)cmd.a, false);
                break;
            case CMD_KEYPRESS_SHIFTED:
                native_input_send_scancode(42, true);  /* LSHIFT */
                native_input_send_scancode((uint16_t)cmd.a, true);
                native_input_send_scancode((uint16_t)cmd.a, false);
                native_input_send_scancode(42, false);
                break;
            case CMD_SCANCODE:
                native_input_send_scancode((uint16_t)cmd.a, cmd.pressed);
                break;
            case CMD_WHEEL:
                native_input_send_mouse_wheel(cmd.a);
                break;
            default: break;
            }
            /* === L5 - DEVICE READ -> SOCKET WRITE ===
             * Measured AFTER the send call, so DTLS encryption and the system
             * call are included: `native_input_send_*` goes all the way to
             * `wolfSSL_write`. It covers the queue wait (the drain thread sleeps
             * 10 ms between two rounds) and nothing else upstream - sampling at
             * the drawing rate is read on the `video/cadence` stage instead,
             * which gives the period that sets it.
             * One reading for all event types: a click emits two (press +
             * release), and the second is counted in the same measurement, which
             * is exactly what the player experiences. */
            if (cmd.t_posted_us > 0)
                latency_add(LAT_IN_SEND, latency_now_us() - cmd.t_posted_us);
        }
    }
}

/* Helpers exposed to stream_view's touch handler. */

bool shadow_input_session_active(void) {
    return g_session_active != 0;
}

void shadow_input_emit_click(int button) {
    struct ui_cmd c = { .kind = CMD_CLICK, .a = button };
    cmd_post(&c);
}

/* Posted to the queue, executed by the drain thread. SAFE from the UI thread. */
void shadow_input_post_mouse_move_abs(int x, int y) {
    struct ui_cmd c = { .kind = CMD_MOUSE_MOVE_ABS, .a = x, .b = y };
    cmd_post(&c);
}

/* Thread-safe wrapper for the touch handler. SAFE from the UI thread. */
void shadow_input_post_mouse_move(int dx, int dy) {
    struct ui_cmd c = { .kind = CMD_MOUSE_MOVE, .a = dx, .b = dy };
    cmd_post(&c);
}

void shadow_input_post_mouse_button(int button, bool pressed) {
    struct ui_cmd c = { .kind = CMD_MOUSE_BUTTON, .a = button, .pressed = pressed };
    cmd_post(&c);
}

void shadow_input_post_scancode(uint16_t scancode, bool pressed) {
    struct ui_cmd c = { .kind = CMD_SCANCODE, .a = (int)scancode, .pressed = pressed };
    cmd_post(&c);
}

void shadow_input_post_mouse_wheel(int direction) {
    struct ui_cmd c = { .kind = CMD_WHEEL, .a = direction };
    cmd_post(&c);
}
