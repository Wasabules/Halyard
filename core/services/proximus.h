// Calls to the VM's host (proximus_url, e.g.
// ipv6-gpu-....compute.shadow.tech/<slot>).
// Bearer = the JWT obtained through /shadow/vm/proximus-credentials (one per
// client_type). No X-Vm-Id here - this is another server, a plain Bearer plus
// JSON is enough.

#pragma once
#include <stdbool.h>

// POST <proximus_url>/clients, body {"type":"launcher", "opaque":"<json string>"}
// The server returns {"data":{...,"spice_secret":"..."}} with a 201.
typedef struct {
    char *id;            // the client identifier returned
    char *spice_secret;  // used later on the SPICE/streaming side
} ProximusLauncherSession;

void proximus_launcher_session_free(ProximusLauncherSession *s);

bool proximus_create_launcher_client(const char *proximus_url, const char *launcher_jwt,
                                     ProximusLauncherSession *out, long *http_status);

// POST <proximus_url>/clients, body {"type":"main", "opaque":"<json device-id>"}
// The server returns
// {"data":{...,"streamingtoken":"...","streamingtoken_expiry":...}} with a 201.
typedef struct {
    char *id;
    char *streaming_token;
    long long streaming_token_expiry;  // epoch seconds
} ProximusMainSession;

void proximus_main_session_free(ProximusMainSession *s);

bool proximus_create_main_client(const char *proximus_url, const char *main_jwt,
                                 ProximusMainSession *out, long *http_status);

// POST <proximus_url>/clients, body {"type":"usb"} with usb_jwt.
// A prerequisite for POST /devices (USB-over-TCP shadowusb).
typedef struct {
    char *id;
} ProximusUsbSession;

void proximus_usb_session_free(ProximusUsbSession *s);

bool proximus_create_usb_client(const char *proximus_url, const char *usb_jwt,
                                ProximusUsbSession *out, long *http_status);

// GET <proximus_url>/status — JSON {meta, data:{vm_status, reachable, streamer_up}}.
typedef struct {
    char *vm_status;     // ex: "started"
    bool  reachable;
    bool  streamer_up;
} ProximusStatus;

void proximus_status_free(ProximusStatus *s);

bool proximus_get_status(const char *proximus_url, const char *jwt,
                         ProximusStatus *out, long *http_status);

// POST <proximus_url>/forward with a "launcher" event. The body sent is:
//   {"command":"forward","data":{"type":"<event_type>","sender":"launcher", ... extras}}
// The VM answers {"id":"<n>"}. For this first attempt we only send the events
// that carry no value (type=get_version / is_streamer_reachable). It can be
// extended later.
typedef struct {
    char *id;  // the id returned by the VM
} ProximusForwardResult;

void proximus_forward_result_free(ProximusForwardResult *r);

bool proximus_forward_event(const char *proximus_url, const char *jwt,
                            const char *event_type,
                            ProximusForwardResult *out, long *http_status);

// GET <proximus_url>/stream (SSE). Opens the connection, writes the raw bytes
// into `out_path` as they arrive for `seconds`, then cuts (curl timeout).
// Returns true when at least one chunk was received (a normal SSE timeout
// included).
bool proximus_dump_stream(const char *proximus_url, const char *jwt,
                          const char *out_path, int seconds, long *http_status);

// SSE keepalive - opens /stream on a parallel thread for the duration of the
// session and holds it open until abort. Without it the Shadow VM considers the
// session "abandoned by the client" (first seen on the abandoned WebRTC path as
// a video cut at exactly t=120 s; the browser keeps this SSE open permanently,
// cf. the HAR).
typedef struct proximus_sse_keepalive proximus_sse_keepalive;

proximus_sse_keepalive *proximus_sse_start(const char *proximus_url,
                                           const char *jwt);

/* Variant with a custom suffix ("stream" or "status"). The official desktop app
 * opens one /N/stream and one /N/status in parallel (on two different fds). The
 * second SSE on /status seems to be required for the server to bind
 * :port_base+11. */
proximus_sse_keepalive *proximus_sse_start_ex(const char *proximus_url,
                                                const char *jwt,
                                                const char *suffix);

/* === SSE1 2026-10-03 — A SINK, SO SOMEBODY CAN READ THE STREAM ===========
 *
 * The keepalive exists because the control port does not open without it,
 * and that is all it did: each `data:` line went to stderr and nowhere else.
 * `bsod` and `get-out` are in the taxonomy and would have gone to stderr too.
 *
 * A REGISTERED CALLBACK and not a link-time symbol, for the same reason the
 * decoded picture leaves by `ctrl_session_glue_set_frame_sink`: this is a
 * bottom-layer service and it must not know what a client is.
 *
 * THE CALLBACK RUNS ON THE KEEPALIVE'S OWN THREAD, inside curl's write
 * handler. Two consequences the caller does not get to ignore: it must not
 * block (the stream stalls behind it, and with it the thing the server uses
 * to decide we are still here), and it must not touch a UI directly - post,
 * do not paint. `json` is NOT NUL-terminated; use `len`, and copy anything
 * that must outlive the call.
 *
 * Set it before `proximus_sse_start` returns its handle to another thread,
 * or accept that the first frames arrive with no sink. */
typedef void (*proximus_sse_sink)(const char *json, int len, void *user);
void proximus_sse_set_sink(proximus_sse_keepalive *k,
                           proximus_sse_sink cb, void *user);

void proximus_sse_stop(proximus_sse_keepalive *k);

/* K14 - GET <proximus_url>/clients: returns the raw JSON (to be freed). */
bool proximus_list_clients(const char *proximus_url, const char *jwt,
                            char **out_json, long *http_status);
/* K14 - DELETE <proximus_url>/clients/<id>: the official client does this at
 * the end of a session; without it the clients pile up on the VM. */
bool proximus_delete_client(const char *proximus_url, const char *jwt,
                             const char *client_id, long *http_status);
