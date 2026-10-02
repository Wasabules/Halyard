/* clip_chan.h - the CLIPBOARD channel `:base+14`: reassembly and sequencing.
 *
 * === CLIP 2026-10-02 - WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT ========
 *
 * `clip_wire.{c,h}` is the codec and is pure. This file is the part that needs
 * to REMEMBER something between two reads: the reassembly cache, and the one
 * piece of sequencing the protocol forces on us (the pull).
 *
 * It owns no socket, no TLS context and no thread. The caller feeds it bytes
 * and drains events. Three reasons, in order of weight:
 *
 *  1. The channel is a TLS stream whose bytes arrive in arbitrary pieces, and
 *     the VM's own reader does the same thing - `DataCache::PushDataToCache`
 *     @0x140c30dc0 appends what it got and logs `state=waiting` until
 *     `received == announced`. A state machine is the protocol's shape, not an
 *     implementation choice.
 *  2. It makes the whole thing testable offline, which is this repo's bar for
 *     anything whose bug costs an RE campaign.
 *  3. The caller that will drive it does not exist yet (the desktop client),
 *     and `ctrl_comchan.c` already has the socket and wolfSSL code to copy
 *     from when it does. NOTHING calls this module today, and nothing opens
 *     `:base+14`: `SHADOW_COMCHAN` stays off, not because it is dangerous -
 *     it is not any more, see clip_wire.h on the stale KB §3.37 warning - but
 *     because the channel it opens transfers nothing.
 *
 * The state lives in a caller-allocated `clip_chan_t`, never in a function
 * static: a clipboard that only worked on the first session of a boot is
 * exactly the failure the house rule about function statics is about.
 *
 * === THE PULL, WHICH IS THE WHOLE REASON THIS FILE HAS STATE ================
 *
 * VM -> us is a pull (clip_wire.h has the evidence): UPDATE arrives, and the VM
 * then WAITS for our REQUEST before it sends anything. So "an update is
 * outstanding" is state we have to hold, and we have to hold it across reads.
 *
 * us -> VM is a push of REPLY_TEXT alone, which needs no state at all - build
 * it with `clip_wire_build_reply_text` and write it.
 *
 * === THREE SERVER BEHAVIOURS A CALLER MUST KNOW ABOUT =======================
 *
 *  - NO ECHO. `OnLocalClipboardUpdate_` @0x140c30570 returns immediately when
 *    `GetClipboardOwner() == ` its own fake window, and that window is what
 *    `SetClipboardData` ran under. So our push does not come back as an UPDATE:
 *    there is no ping-pong to break.
 *
 *  - AN UPDATE DOES NOT MEAN THE TEXT CHANGED. When `GetClipboardData(0xD)`
 *    returns NULL - the VM's clipboard holds an image, say - the same function
 *    logs the failure and STILL pushes the UPDATE, leaving `a1+296` holding the
 *    PREVIOUS text. Our REQUEST then returns that stale string. A caller that
 *    pastes whatever arrives will silently re-paste old content, so
 *    `clip_chan_text_is_new()` is offered to compare against what was last
 *    seen. This is the one place where a correct implementation of the wire
 *    still gets the user-visible behaviour wrong.
 *
 *  - CONNECT IS NOT A FORMALITY. `StfpClient::IsThisMessageMine` @0x140c2eb50
 *    returns true ONLY when the opcode is 0, and that return is what binds the
 *    socket to the clipboard client and calls `MarkConnected`. Until we send
 *    CONNECT, nothing we send afterwards is routed to the clipboard engine at
 *    all. Send it as the first application bytes after the TLS handshake.
 */
#ifndef CLIP_CHAN_H
#define CLIP_CHAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clip_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The server's own two words for its cache, from the `state=%s` of
 * `PushDataToCache:31`. There is no third: the state is derived from
 * `received == announced` and nothing else. */
typedef enum {
    CLIP_CACHE_IDLE = 0,   /* no payload outstanding ("complete", at rest) */
    CLIP_CACHE_WAITING,    /* a REPLY_TEXT is announced and still arriving */
    CLIP_CACHE_SKIPPING    /* ours, not the server's: a payload too large for
                            * our buffer is being discarded so the framing
                            * survives it. Dropping the bytes without counting
                            * them would resume parsing inside the payload. */
} clip_cache_state_t;

typedef struct {
    uint32_t cap;                 /* largest payload accepted AND sent */
    uint32_t request_timeout_ms;  /* 0 disables the retry */
    bool     strict;              /* clip_wire_parse_header's two guards */
} clip_chan_cfg_t;

/* === WHY 1000 ms, AND WHY IT IS NOT A SERVER DELAY =========================
 *
 * A REQUEST is answered from a cached string: `OnClipboardRequest_` @0x140c30bc0
 * takes the mutex, copies `a1+296` and pushes the reply - no Win32 clipboard
 * call, so none of the 10 x 15 ms `OpenClipboard` retry of `ClipboardOpen_`
 * @0x140c30410 applies to it. The only delay is the network, measured at ~20 ms
 * on the one capture that recorded the cycle (INV1).
 *
 * So this timeout is not tuned to a server behaviour; it exists so that ONE
 * lost REQUEST does not wedge the channel for the rest of the session. 1000 ms
 * is 50x the measured round trip - late enough never to double a request that
 * is merely in flight, soon enough that a user does not notice. */
#define CLIP_CHAN_REQUEST_TIMEOUT_MS 1000u

/* Fills `cfg` with the defaults: CLIP_WIRE_CAP_DEFAULT, the timeout above, and
 * strict = true. Pure. */
void clip_chan_cfg_default(clip_chan_cfg_t *cfg);

/* The same, then overridden by the environment. THE ONLY IMPURE FUNCTION IN
 * THIS MODULE, which is why it is a separate call: tests build a cfg by hand
 * and never touch it.
 *
 *   SHADOW_CLIP_STRICT=0  stops checking `reserved` and `format` on receive,
 *                         i.e. restores the server's own laxity. Default 1,
 *                         because the strict side is the official CLIENT's
 *                         (`bodySize()` @0xabd460 returns -1 on a non-zero
 *                         `reserved`) and because eight of a header's ten bytes
 *                         are normally zero: without those two guards the first
 *                         ten bytes of a TLS alert parse as a header announcing
 *                         33 MB. Measured on the cursor channel, where exactly
 *                         that happened (K16a).
 *   SHADOW_CLIP_MAX=<n>   the byte ceiling. Default 4 MiB = the VM's own
 *                         1 Mi UTF-16 clamp at its worst-case UTF-8 cost,
 *                         rounded up (see clip_wire.h). A value of 0 restores
 *                         CLIP_WIRE_CAP_DEFAULT rather than refusing
 *                         everything - a toggle must not be able to disable the
 *                         feature by arithmetic accident. */
void clip_chan_cfg_from_env(clip_chan_cfg_t *cfg);

typedef enum {
    CLIP_EV_FLUSH = 1,   /* opcode 1. The server NEVER sends this - see below. */
    CLIP_EV_UPDATE,      /* opcode 2: the VM has something. REQUEST it. */
    CLIP_EV_REQUEST,     /* opcode 3: the VM wants ours. Send REPLY_TEXT. */
    CLIP_EV_TEXT,        /* opcode 4, fully reassembled */
    CLIP_EV_UNKNOWN,     /* a well-formed header with an opcode outside 0..4 */
    CLIP_EV_OVERSIZE,    /* a REPLY_TEXT larger than our buffer or our cap */
    CLIP_EV_BAD_UTF8,    /* reassembled, but not valid UTF-8 */
    CLIP_EV_PROTO_ERROR  /* a header we refuse: the stream cannot be resynced */
} clip_ev_kind_t;

typedef struct {
    clip_ev_kind_t kind;
    uint16_t       opcode;
    uint32_t       announced;  /* the header's length, for OVERSIZE and TEXT */
    const uint8_t *text;       /* CLIP_EV_TEXT / CLIP_EV_BAD_UTF8: points into
                                * the caller's buffer, valid until the next
                                * clip_chan_feed. NOT NUL-terminated: there is
                                * no terminator on the wire. */
    size_t         text_len;
} clip_ev_t;

typedef struct {
    clip_chan_cfg_t cfg;

    uint8_t *buf;          /* caller-owned reassembly buffer, never freed here */
    size_t   buf_cap;

    uint8_t  hdr[CLIP_WIRE_HEADER_LEN];
    uint8_t  hdr_have;     /* 0..10: a header split across reads */

    clip_cache_state_t state;
    uint32_t announced;    /* payload bytes the current message declared */
    uint32_t received;     /* of those, how many are in `buf` (or skipped) */

    /* A REPLY_TEXT is fully in `buf` but its event has not been delivered,
     * because the caller's event array was full. It MUST be a flag and not a
     * rewind of the input cursor: those bytes are already in `buf`, so feeding
     * them again would have them read a second time as a header. */
    bool     complete_pending;

    /* The pull. `update_outstanding` survives reads on purpose. */
    bool     update_outstanding;
    bool     request_in_flight;
    uint32_t request_sent_ms;

    /* Last text handed to the caller, for `clip_chan_text_is_new`. A 64-bit
     * FNV-1a and a length, not a copy: holding a second megabyte to answer
     * "did this change?" is not worth it, and a collision costs one skipped
     * paste, not a wrong one. */
    uint64_t last_text_hash;
    size_t   last_text_len;
    bool     have_last_text;
} clip_chan_t;

/* Binds `st` to a caller-owned reassembly buffer. `buf_cap` is what bounds a
 * REPLY_TEXT in practice: anything larger raises CLIP_EV_OVERSIZE and is
 * skipped rather than buffered. Returns false on a NULL argument or a zero
 * capacity. */
bool clip_chan_init(clip_chan_t *st, uint8_t *buf, size_t buf_cap,
                    const clip_chan_cfg_t *cfg);

/* Forgets every partial message but keeps the configuration and the buffer.
 * Call it on reconnect: a half-received payload from a dead socket would
 * otherwise be completed by the first bytes of the new one. */
void clip_chan_reset(clip_chan_t *st);

/* Consumes `data[0..n)`, appending events to `evs`.
 *
 * Returns the number of bytes consumed. That is `n` unless `evs` filled up
 * first, in which case the caller drains the events and feeds the remainder.
 * A caller that ignores the return value and does not re-feed loses bytes and
 * desynchronises - so the value is not advisory.
 *
 * After CLIP_EV_PROTO_ERROR nothing more is consumed in that call: a header we
 * refuse gives no length, so there is no way to find the next message boundary.
 * The caller must drop the connection. */
size_t clip_chan_feed(clip_chan_t *st, const uint8_t *data, size_t n,
                      clip_ev_t *evs, size_t max_evs, size_t *n_evs);

/* True when a REQUEST should go out now: an UPDATE is outstanding and either no
 * REQUEST is in flight, or the one in flight has timed out. `now_ms` is a
 * monotonic millisecond count supplied by the caller - this module reads no
 * clock, so it can be tested. */
bool clip_chan_should_request(const clip_chan_t *st, uint32_t now_ms);

/* Records that a REQUEST has just been written to the socket. */
void clip_chan_note_request_sent(clip_chan_t *st, uint32_t now_ms);

/* True when `text` differs from the last text this channel reported. See the
 * second server behaviour in the file header: the VM announces an UPDATE even
 * when its clipboard holds no text, and then replies with its stale cache, so
 * identical text arriving twice is the normal case and not a sign of a bug. */
bool clip_chan_text_is_new(const clip_chan_t *st, const uint8_t *text, size_t n);

/* Records `text` as the last one seen, so that `clip_chan_text_is_new` answers
 * false for it. Separate from `_is_new` so the caller can decide to accept a
 * repeat (a user pressing paste twice) without that decision being implicit in
 * the question. */
void clip_chan_note_text(clip_chan_t *st, const uint8_t *text, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* CLIP_CHAN_H */
