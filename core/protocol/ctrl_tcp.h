// SslCtrlChanV2 transport - TCP + wolfSSL TLS 1.3, length-prefixed framing.
//
// Wire format:
//   [ uint32_be len ][ protobuf-message body of length `len` ]
//
// Before the EncryptionReply: body in the clear (= raw protobuf).
// After a shadow_cipher is attached: body = AEAD wire `[ ct | nonce12 | tag16 ]`
// (see streaming/encryption.h).
//
// The API is synchronous. The caller owns its thread on Switch (see memory
// `feedback_switch_thread_lifecycle` - poll abort_flag).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "encryption.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctrl_tcp_session ctrl_tcp_session;

// TCP IPv4 connect to host:443 + TLS 1.3 handshake (SNI=host, **NO ALPN**).
// timeout_ms applies to the TCP connect; the TLS handshake then blocks.
// Returns NULL on failure (logged via journal_uncategorised).
ctrl_tcp_session *ctrl_tcp_open(const char *host, int timeout_ms);

// Variant that takes a custom port (used by shadowusb on its dynamic port).
ctrl_tcp_session *ctrl_tcp_open_port(const char *host, int port, int timeout_ms);

/* S57b - same, but the connect's wait gives up within 100 ms of `*abort`. */
ctrl_tcp_session *ctrl_tcp_open_port_abortable(const char *host, int port,
                                               int timeout_ms,
                                               const volatile int *abort);

/* Why the last `ctrl_tcp_open_port` returned NULL - "TCP connect",
 * "wolfSSL_new (entropy or memory)", "TLS handshake"... The caller sees only a
 * NULL and used to guess a cause; see the comment in ctrl_tcp.c. Never NULL. */
const char *ctrl_tcp_last_failure(void);

// Send/recv plaintext bytes after the TLS handshake (= bypass the HTTP
// envelope). Useful for binary protocols such as usbredir.
bool ctrl_tcp_send_raw(ctrl_tcp_session *s, const uint8_t *buf, size_t len);
bool ctrl_tcp_recv_raw(ctrl_tcp_session *s, uint8_t *buf, size_t want,
                        int timeout_ms);

// Sets the bearer token (Authorization header) used for the HTTP POST
// envelopes. The bearer string must stay valid for as long as the session is
// in use.
void ctrl_tcp_set_bearer(ctrl_tcp_session *s, const char *bearer);

// Sets the instance number (= API version path /N/) used in the requests.
// Decoded from JWT.instance by the caller.
void ctrl_tcp_set_instance(ctrl_tcp_session *s, int instance);

// Sets the path used for the HTTP POST (default = "/forward").
// Variants left to try: "/messages", "/control", "/ctrlchan", "/binary".
void ctrl_tcp_set_path(ctrl_tcp_session *s, const char *path);

// Sends a protobuf message in the clear: [uint32_be len] prefix + body.
bool ctrl_tcp_send_cleartext(ctrl_tcp_session *s,
                             const uint8_t *body, size_t body_len);

// Reads the 4-byte length then the body. Stores into buf (cap), sets *out_len.
// timeout_ms = total budget for this frame (header + body).
bool ctrl_tcp_recv_cleartext(ctrl_tcp_session *s,
                             uint8_t *buf, size_t cap, size_t *out_len,
                             int timeout_ms);

// Non-blocking poll: true if bytes are available to read (either buffered by
// SSL or readable on the socket via select(0)). Does not consume anything.
// V16 2026-05-16: used to ACK the server-pushed kRequestFlush without blocking
// the main loop.
bool ctrl_tcp_has_pending(ctrl_tcp_session *s);

// ING-2 2026-09-11 - what the session loop needs to WAIT on this channel, not
// only check it once per pass. ctrl_tcp_poll_fd() returns the socket to add to
// a poll() set, or -1 with no session or once the peer has closed: a closed
// socket stays readable for ever and would turn the wait into a spin. Adding it
// is only safe while something reads it on every pass - see `ctrl_in_poll` in
// ctrl_session.c. ctrl_tcp_buffered() says whether TLS already holds decrypted
// bytes: poll() cannot see those, so the caller must not sleep on them.
int  ctrl_tcp_poll_fd(const ctrl_tcp_session *s);
bool ctrl_tcp_buffered(const ctrl_tcp_session *s);

// Attaches a cipher for the operations that follow (encrypt on Tx, decrypt on
// Rx). The cipher stays owned by the caller - ctrl_tcp_close does not free it.
void ctrl_tcp_attach_cipher(ctrl_tcp_session *s, shadow_cipher *cipher);

// Encrypted send/recv (or cleartext if no cipher is attached).
bool ctrl_tcp_send(ctrl_tcp_session *s, const uint8_t *body, size_t body_len);
bool ctrl_tcp_recv(ctrl_tcp_session *s, uint8_t *buf, size_t cap,
                   size_t *out_len, int timeout_ms);

void ctrl_tcp_close(ctrl_tcp_session *s);

/* D1 2026-08-20 - true as soon as the peer has closed the transport (clean
 * FIN, SOCKET_PEER_CLOSED_E, ECONNRESET/EPIPE). Once true, every I/O returns
 * false with no syscall and no log: the calling loop MUST bail out.
 * `close_err` exposes the wolfSSL code for diagnosis. */
bool ctrl_tcp_peer_closed(const ctrl_tcp_session *s);
int  ctrl_tcp_close_err(const ctrl_tcp_session *s);

#ifdef __cplusplus
}
#endif

/* S48: last server clock (ms) seen in a status report, 0 if none. The official
 * client echoes it back in its channel-unregister messages. */
extern volatile uint32_t g_srv_clock_ms;

/* D4: control frames RECEIVED since the start. When the video goes quiet this
 * counter answers the only question that matters - is the server still talking
 * to us? If it keeps rising through the outage, the media path is at fault, not
 * the VM and not the link. */
extern volatile uint32_t g_ctrl_rx_frames;

/* S56: 1 as soon as the server announces CHANNEL_DOWN(AUDIO). The oracle for
 * the intermittent-audio defect - readable ~1 s into the session (KB.md §3.35). */
extern volatile int g_audio_channel_down;

/* AUD18 2026-09-11: the number of CHANNEL_DOWN(AUDIO) announced this session
 * (the flag above only says "at least one"). Reset per session with the flag;
 * the session loop times each one against its last emission on :base+30. */
extern volatile unsigned g_audio_channel_down_n;

/* S60: mask of the channels the server declared dead during the session
 * (bit N = channel N: 0 VIDEO, 1 AUDIO, 2 INPUT, 3 CURSOR, 4 MICRO,
 * 5 CONTROLLER, 6 CLIPBOARD, 7 FILETRANSFER). See KB.md §3.37. */
extern volatile unsigned g_channels_down;
