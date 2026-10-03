/* sse_event - what the VM says on its event stream, as a struct.
 *
 * === SSE1 2026-10-03 — THE STREAM WE HELD OPEN AND NEVER READ =============
 *
 * Both clients open two SSE streams at bootstrap because the control port
 * does not open without them (KB §3.37), and that is all they do with them:
 * the keepalive thread prints each `data:` line to stderr, watches for the
 * one substring `acquisition_is_ready`, and throws the rest away. Nothing
 * reaches a UI.
 *
 * What the stream carries is known (`memory/project_sse_event_stream_RE.md`,
 * RE of `ShadowPCDisplay`, C95): five data types and eight event sub-types,
 * with the envelope
 *
 *     {"type":"event","event":"<name>","data":{...}}
 *
 * SET EXPECTATIONS HONESTLY. The same note measured a 235-second session and
 * counted ONE event in it - `shadow-manager.encoding_is_ready` at bootstrap -
 * and refuted the hypothesis that the stream drives anything mid-session. So
 * this parser is not going to make a machine list live on its own. It exists
 * because `bsod` and `get-out` are in the taxonomy, and those two are worth
 * showing the moment they ever arrive; and because a stream that is read is
 * a stream whose silence we can state as a measurement rather than assume.
 *
 * PURE on purpose: one JSON string in, a struct out, no I/O and no state, so
 * `tests/test_sse_event.c` can hold the taxonomy down without a VM.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Level 1, the `type` field. */
typedef enum {
    SSE_TYPE_UNKNOWN = 0,
    SSE_TYPE_EVENT,      /* carries an `event` sub-type - the interesting one */
    SSE_TYPE_FORWARD,
    SSE_TYPE_ERROR,
    SSE_TYPE_SESSION,    /* {"type":"session","length":N} */
    SSE_TYPE_MAIN,       /* the main-JWT subscription acknowledgement */
} sse_type;

/* Level 2, the `event` field, present when `type` is `event`. */
typedef enum {
    SSE_EV_NONE = 0,
    SSE_EV_L2TP,
    SSE_EV_VM_REACHABLE,
    SSE_EV_STATUS_CHANGED,
    SSE_EV_BSOD,
    SSE_EV_SHADOW_MANAGER,
    SSE_EV_GET_OUT,
    SSE_EV_UNKNOWN,      /* a name the taxonomy does not have - see below */
} sse_event_kind;

/* Fixed buffers, so parsing allocates nothing and a caller can keep one of
 * these on the stack inside a curl writer callback - which is where this is
 * called from, on a thread that must not block. The sizes are generous
 * against the captured events; anything longer is truncated rather than
 * refused, because a truncated detail is still a usable report. */
typedef struct {
    sse_type        type;
    sse_event_kind  event;
    char            event_name[64];   /* verbatim, even when UNKNOWN */
    /* For `shadow-manager`, `data.type` - e.g. "encoding_is_ready". For
     * `status-changed`, the new state when the server put one there. Empty
     * when the event has no sub-type. */
    char            sub[64];
    /* `data.status` / `data.value` / `data.state` as text, whichever exists:
     * a one-line summary for a UI, not a parsed payload. Empty when none. */
    char            detail[192];
} sse_event;

/* Parses one event. `json` need not be NUL-terminated if `len` is given;
 * pass 0 for `len` to use strlen.
 *
 * Returns false when the text is not JSON or not an object - which includes
 * the SSE comment lines and the keepalive newlines the server sends between
 * events, so a caller can hand it every `data:` line without filtering.
 *
 * An unrecognised `event` name yields `SSE_EV_UNKNOWN` with `event_name`
 * filled, and that is deliberate: the taxonomy was read off a binary in May
 * and a server can add to it. A name we do not know is a finding, and
 * silently dropping it is how it stays unknown. */
bool sse_event_parse(const char *json, int len, sse_event *out);

/* The name for a kind, for a log line. Never NULL. */
const char *sse_event_kind_name(sse_event_kind k);

#ifdef __cplusplus
}
#endif
