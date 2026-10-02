/* clip_tcp - the clipboard channel on `:base+14`, socket and TLS.
 *
 * === CLIP2 2026-10-02 — THE PIECE THAT WAS MISSING =========================
 *
 * `clip_wire.{c,h}` encodes and decodes the messages and `clip_chan.{c,h}`
 * reassembles them; both are pure and tested offline. Neither touches a socket,
 * which was deliberate — but it left every client having to write its own
 * wolfSSL plumbing before it could exchange a single byte. This is that
 * plumbing, written once, against the same `tls_chan` helper the video, control
 * and input channels already use.
 *
 * WHAT IT DOES, and the protocol facts it encodes (KB §3.50 neighbourhood, full
 * write-up in halyard-lab/notes/findings/clipboard.md):
 *   - TCP + TLS to `:base+14`, the transport the server grants for this channel
 *     (`accord presse-papier : TCP, offset +14`).
 *   - Sends CONNECT (opcode 0) FIRST, because that is the demux key, not a
 *     formality: the server's `IsThisMessageMine` returns true only for opcode
 *     0, and it is that return which binds the socket to the clipboard engine.
 *     A wrong first message gives an open channel, a valid TLS session, and
 *     every subsequent byte routed nowhere — with no error on either side.
 *   - VM → client is a PULL. An UPDATE carries no content; the text arrives
 *     only after we send a REQUEST. A client that merely listens gets nothing.
 *   - client → VM: only REPLY (opcode 4) acts. FLUSH and UPDATE are silently
 *     ignored by the server, so the official client's triplet is decorative in
 *     two thirds. We send REPLY alone.
 *
 * TWO BEHAVIOURS THE CALLER MUST KNOW, because a correct wire still gets them
 * wrong:
 *   - **An UPDATE does not mean the text changed.** When the VM's clipboard
 *     holds an image, `GetClipboardData(CF_UNICODETEXT)` fails server-side and
 *     it pushes UPDATE anyway without touching its cache, so our REQUEST
 *     returns the PREVIOUS text. `clip_chan_text_is_new()` is what stops a
 *     client re-pasting stale content, and this module calls it before every
 *     callback.
 *   - **A zero-length REPLY CLEARS the VM's clipboard** (the server empties it
 *     before setting). That is a usable operation, not an accident — but it
 *     means an empty string is not a no-op.
 *
 * Payload is UTF-8 (`CP_UTF8`, named in the server binary), no NUL, exact
 * length.
 *
 * THREADING. One receive thread owned by this module; the callback runs on it,
 * so a client must not block in it. `clip_tcp_send_text()` is safe to call from
 * another thread: sends are serialised against the receive thread by a mutex,
 * because wolfSSL here is built `-DSINGLE_THREADED` and two threads writing one
 * TLS session would interleave records (the lesson K16c paid for on the video
 * channel).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clip_tcp clip_tcp_t;

/* Called when the VM's clipboard text has arrived AND differs from the last
 * text seen, on the receive thread. `text` is UTF-8, `n` bytes, not
 * NUL-terminated, and valid only for the duration of the call. */
typedef void (*clip_tcp_text_cb)(const uint8_t *text, size_t n, void *user);

/* The clipboard's port offset from the session's port base. Exported because a
 * caller holding only the base needs it; a caller holding the SNAPSHOT should
 * pass `caps.chan[SHADOW_CHAN_IDX_CLIPBOARD].port` and ignore this. */
#define CLIP_PORT_OFFSET 14

/* Opens TCP+TLS to host:port, sends CONNECT, starts the receive thread.
 * Returns 0 on success. `*out` is allocated on success and NULL otherwise.
 *
 * `port` is the ABSOLUTE port, not a base. That is deliberate and it is the
 * INT1 rule: `session_caps.h` resolves `port_base + offset` once, because a
 * caller recomputing it is how the file-transfer self-test came to knock on
 * 7015 while the channel listened on 14015.
 *
 * `abort_flag` may be NULL; when it is not, the connect wait and the receive
 * thread abandon within 100 ms of it becoming non-zero. On a console that is
 * not optional: a thread that polls its abort flag less often than that leaks
 * into HOS on process exit and only a reboot clears it (CLAUDE.md). */
int clip_tcp_open(clip_tcp_t **out, const char *host, int port,
                  clip_tcp_text_cb cb, void *user,
                  const volatile int *abort_flag);

/* Stops the thread, closes TLS then the socket, frees. Safe on NULL. */
void clip_tcp_close(clip_tcp_t *c);

/* === CLIP4 2026-10-02 - A DIRECTION IS NOT A FILTER ON DELIVERY ===========
 *
 * A client that does not WANT the VM's clipboard must not PULL it. Dropping the
 * text in the callback would look the same from the outside and be wrong: the
 * REQUEST still goes out, the VM still answers, and the user's remote clipboard
 * still crosses the network - to be thrown away. Whatever they had copied in
 * the VM travels anyway, which is the one thing a one-way setting is chosen to
 * prevent.
 *
 * So the direction is enforced at the pull. `on` (the default) requests the
 * text when the VM announces an UPDATE; `off` leaves the announcement counted
 * and unanswered, and nothing is ever sent back.
 *
 * Safe to call at any time, from any thread: it writes one int that the receive
 * thread only reads. */
void clip_tcp_set_auto_request(clip_tcp_t *c, bool on);

/* Called when the VM ASKS for our clipboard, on the receive thread.
 *
 * Without this the server's REQUEST goes unanswered - and answering an empty
 * REPLY is not an option, because a zero-length REPLY CLEARS the VM's
 * clipboard. The handler must NOT read the clipboard and answer inline: it runs
 * on the receive thread, which has 100 ms to come back to its abort flag. It
 * should note the request and let its own loop call `clip_tcp_send_text()`. */
typedef void (*clip_tcp_request_cb)(void *user);
void clip_tcp_set_request_cb(clip_tcp_t *c, clip_tcp_request_cb cb, void *user);

/* Asks the VM for its clipboard text (REQUEST). Normally unnecessary: this
 * module requests automatically when the VM announces an UPDATE. Exposed for a
 * client that wants to refresh on its own schedule — a paste shortcut, say.
 * Returns 0 if the request went out. */
int clip_tcp_request(clip_tcp_t *c);

/* Pastes `text` (UTF-8, `n` bytes) into the VM's clipboard as a REPLY.
 * `n == 0` CLEARS the VM's clipboard — see the header comment. Returns 0 on
 * success, -1 on a bad argument or a send failure, -2 when `n` exceeds the
 * configured cap. */
int clip_tcp_send_text(clip_tcp_t *c, const uint8_t *text, size_t n);

typedef struct {
    uint32_t rx_updates;     /* UPDATE announcements from the VM */
    uint32_t rx_texts;       /* texts delivered to the callback (new ones only) */
    uint32_t rx_stale;       /* texts dropped as identical to the last one */
    uint32_t tx_requests;
    uint32_t tx_replies;
    uint32_t rx_bad;         /* messages the codec refused */
    uint32_t rx_updates_ignored; /* UPDATEs left unanswered: VM->PC is off */
    uint32_t rx_asked;       /* times the VM asked for OUR clipboard */
    uint64_t rx_bytes;
} clip_tcp_stats_t;

void clip_tcp_get_stats(const clip_tcp_t *c, clip_tcp_stats_t *out);

#ifdef __cplusplus
}
#endif
