/* ctrl_input_tcp - the native Shadow input channel.
 *
 * === THE FILE NAME AND THE PORT IN IT ARE BOTH OUT OF DATE ===================
 * The input channel is **DTLS/UDP on `:base+12`** (§3.21, and the open/send
 * path in ctrl_input_tcp.c:836+). `:base+14` is the CLIPBOARD (§3.37); the
 * "TCP" in this module's name dates from I1, when we believed otherwise, and a
 * rename is a mechanical commit of its own. Read the port off the code, never
 * off this name.
 *
 * I1 2026-05-18: the message templates were found in the V16 plaintext capture,
 * SSL_WRITE on ssl=0x3781b750. Wire format, after DTLS:
 *   [u32_LE size] [FlatBuffer payload (size B)]
 * Connect msg = 96 B, mouse/keyboard events = 144 B each with an incrementing seq.
 *
 * TWO SERVER RULES THIS MODULE MUST KEEP (ShadowStreamer 6.3.1,
 * `Input::Clients::FlatBufferClient::DealWithInput` @0x140c912a0):
 *   - a message that fails to deserialise does not cost one event, it costs the
 *     CHANNEL: the server logs `Deserialize error result %d --> invalid the
 *     client` and invalidates the stream client, after which nothing we send on
 *     `:base+12` is even looked at (see session_reannounce_channel in
 *     ctrl_session.c). So anything that GENERATES a message rather than patching
 *     a captured template must validate it before sending.
 *   - messages sent before the Connect blob is accepted are dropped
 *     (`Received valid messages but client is not connected`). The ordering this
 *     module already enforces is a hard server rule, not a courtesy.
 *
 * Reuses the wolfSSL stack (already linked for ctrl_tcp + ctrl_video_tcp).
 * No SNI, no ALPN, same as desktop.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctrl_input_tcp ctrl_input_tcp_t;

/* Open the TLS connection on :base+14, send the Connect blob, spawn the RX
 * thread (log only). Returns 0 on success, -1 on failure. *out is allocated. */
int ctrl_input_tcp_open(ctrl_input_tcp_t **out,
                         const char *vm_host, uint16_t base_port);

/* Blocking. Closes TLS + joins the RX thread. Frees the context. */
void ctrl_input_tcp_close(ctrl_input_tcp_t *c);

/* AUD1 2026-08-21 - candidate audio output.
 *
 * We do not yet know which channel carries the sound: none of our captures
 * contained any, the remote desktop having stayed silent throughout the
 * reverse-engineering sessions. Only three UDP channels receive anything at all
 * (video, cursor, and this DTLS one), so the cheapest hypothesis is that audio
 * is multiplexed here, alongside the input return path.
 *
 * This entry point receives everything that is NOT a recognised input echo. If
 * the hypothesis holds, wiring the Opus decoder onto it is enough; if it does
 * not, the size census this channel logs will say where to look instead. */
typedef void (*ctrl_input_tcp_audio_cb)(const uint8_t *payload, size_t len,
                                         uint32_t ts, void *user);
void ctrl_input_tcp_set_audio_cb(ctrl_input_tcp_t *c,
                                  ctrl_input_tcp_audio_cb cb, void *user);

/* === Input API - same signatures as shadow_input.h ================ */

/* Mouse delta motion. The server reads it as a relative displacement. */
int ctrl_input_tcp_send_mouse_move(ctrl_input_tcp_t *c, int dx, int dy);
/* C3 - move to an ABSOLUTE position (the wire already carries X/Y at @128-131).
 * Prefer this wherever the caller knows the position it wants: deltas drift
 * (see ctrl_input_tcp.c). */
int ctrl_input_tcp_send_mouse_move_abs(ctrl_input_tcp_t *c, int x, int y);

/* Mouse button: 0=LEFT, 1=RIGHT, 2=MIDDLE. */
int ctrl_input_tcp_send_mouse_button(ctrl_input_tcp_t *c, int button, bool pressed);

/* Keyboard scancode (= Linux evdev codes, input-event-codes.h). */
int ctrl_input_tcp_send_scancode(ctrl_input_tcp_t *c, uint16_t scancode, bool pressed);

/* Mouse wheel: +1 = scroll up, -1 = scroll down. */
int ctrl_input_tcp_send_mouse_wheel(ctrl_input_tcp_t *c, int direction);

/* Stats, for debugging */
typedef struct {
    bool     handshake_ok;
    uint32_t bytes_sent;
    uint32_t messages_sent;
    uint32_t bytes_recv;
    uint32_t messages_recv;
} ctrl_input_tcp_stats_t;

void ctrl_input_tcp_get_stats(const ctrl_input_tcp_t *c, ctrl_input_tcp_stats_t *out);

#ifdef __cplusplus
}
#endif

/* S7 - raw port probe, see ctrl_input_tcp.c (SHADOW_PROBE_PORTS). */
void ctrl_input_tcp_probe_ports(const char *vm_host, uint16_t base_port,
                                 const uint8_t *preamble, size_t preamble_len);

/* S9 - TLS+Connect scan of the ports, to locate the input channel. */
void ctrl_input_tcp_scan(const char *vm_host, uint16_t base_port);

/* S16 - cursor position as reported by the server (0 if unknown). */
int ctrl_input_tcp_server_pos(ctrl_input_tcp_t *c, int *x, int *y);
