/* native_input.h - bridge from stream_view and the shadow_input drain thread
 * to ctrl_input_tcp.
 *
 * I1 phase 3 (2026-05-18): the native mode does NOT use SCTP/WebRTC for input.
 * This layer lets the existing call sites in stream_view.cpp (which call
 * shadow_input_post_*) keep working unchanged - the drain thread routes to
 * TCP+TLS :base+14 whenever our flag is set.
 *
 * Lifecycle:
 *   1. ctrl_session_glue_run() runs ctrl_input_tcp_open -> native_input_set()
 *   2. stream_view keeps calling shadow_input_post_mouse_move(dx,dy) and friends
 *   3. the shadow_input drain thread sees native_input_active() == true, so it
 *      bypasses the WebRTC path and calls native_input_send_mouse_move(dx,dy)
 *   4. ctrl_session cleanup -> native_input_clear()
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ctrl_input_tcp;
typedef struct ctrl_input_tcp ctrl_input_tcp_t;

/* Setter: called by ctrl_session after a successful ctrl_input_tcp_open.
 * Thread-safe. Pass NULL to clear. */
void native_input_set(ctrl_input_tcp_t *itc);

/* Quick test: true when the native mode is active (= ctrl_input_tcp connected).
 * Thread-safe, lock-free read. */
bool native_input_active(void);

/* Wrappers around ctrl_input_tcp_send_* - no-op if native_input_active==false.
 * Thread-safe. Called from the drain thread in shadow_input.c. */
int native_input_send_mouse_move(int dx, int dy);
/* C3 - move by absolute position (avoids the drift accumulated deltas cause). */
int native_input_send_mouse_move_abs(int x, int y);
int native_input_send_mouse_button(int button, bool pressed);
int native_input_send_scancode(uint16_t scancode, bool pressed);
int native_input_send_mouse_wheel(int direction);

#ifdef __cplusplus
}
#endif
