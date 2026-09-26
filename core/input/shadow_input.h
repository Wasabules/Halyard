/* shadow_input - the UI's input queue, and the thread that drains it.
 *
 * The Borealis UI thread posts mouse, keyboard and wheel events here; the
 * session's drain thread executes them through `native_input_*`, which writes
 * to the `:base+12` input channel. The UI never touches the socket or wolfSSL
 * itself: two threads in wolfSSL at once corrupted its heap (2026-05-03), which
 * is the whole reason this queue exists.
 *
 * 2026-09-26 - the WebRTC data-channel half of this module (FlatBuffer
 * templates, SCTP sends, the `sctp_assoc` handle, the stream and PPID numbers)
 * was removed with the rest of that path. What a session needed from it was
 * only ever "is there one": `shadow_input_session_active()`.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Remote mouse mode. 0 = absolute positions (correct on a desktop), 1 = relative
 * deltas (indispensable as soon as a game captures the mouse and recentres it
 * every frame, without which it runs a hundred times too fast). */
void shadow_input_set_mouse_relative(int on);
int  shadow_input_get_mouse_relative(void);

// The cursor's current absolute position on Shadow's side (px). Updated on every
// mouse move sent. Lets StreamView draw a synthetic overlay.
void shadow_input_get_cursor_pos(int *x, int *y);

// BUG5 2026-05-18 - syncs the tracker from the native path, which moves the
// remote cursor without going through this queue. Without it CMD_MOUSE_MOVE_ABS
// computes wrong deltas and elements under the cursor do not react.
void shadow_input_set_cursor_pos(int x, int y);

/* === PM2 2026-09-02 - THE TRACKER'S BOUNDS ARE THE REMOTE DESKTOP'S ===
 *
 * The clamp used to be 0..65535, which is the field's width on the wire, not a
 * screen. Push a relative pointer against the right edge for two seconds and
 * the VM stops at 1919 while our estimate keeps counting to 60000 - after which
 * coming back takes exactly as long as you spent pushing, and the drawn cursor
 * sits off screen the whole time.
 *
 * Set from the DECODED frame size, which is the remote desktop's real
 * resolution - not the one we asked for, which the server is free to refuse. */
void shadow_input_set_bounds(int w, int h);

// Whether an input session is live. The session sets it when its input channel
// opens and clears it when it ends; the UI only posts while it is true. Setting
// it either way flushes the queue.
void shadow_input_set_session_active(bool active);
bool shadow_input_session_active(void);

// === Thread-safe API for the Borealis UI thread ===
// All of these enqueue an event, consumed by the session's drain thread through
// shadow_input_drain_queue(). None touches the socket or wolfSSL.
void shadow_input_post_mouse_move(int dx, int dy);
// Brings the remote cursor to (x, y).
void shadow_input_post_mouse_move_abs(int x, int y);
void shadow_input_post_mouse_button(int button, bool pressed);
// A complete click (down + up). button: 0 = left, 1 = right.
void shadow_input_emit_click(int button);
// Wheel: direction +1 = scroll up, -1 = scroll down.
void shadow_input_post_mouse_wheel(int direction);
// A separate scancode down OR up (to hold a key in a game).
void shadow_input_post_scancode(uint16_t scancode, bool pressed);

// Called by the session's drain thread: executes the posted events.
void shadow_input_drain_queue(void);

/* L16 - waits for an input event to be queued, for at most `timeout_ms`. Returns
 * immediately when the queue is not empty. Replaces the drain thread's fixed
 * sleep: ~5 ms less average latency on every gesture. */
void shadow_input_wait_cmd(int timeout_ms);

// Debug counters exposed to the stream_view overlay, to see what is being sent in
// real time.
typedef struct {
    int n_mouse_moves;
    int n_clicks_left;
    int n_clicks_right;
    int n_btn_down;
    int n_btn_up;
    int n_keypress;
    int queue_depth;
    char last_action[64];  /* a short string, e.g. "click@(123,456)" */
} shadow_input_debug;

void shadow_input_get_debug(shadow_input_debug *out);

#ifdef __cplusplus
}
#endif
