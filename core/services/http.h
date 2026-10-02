// A light libcurl wrapper for the Shadow REST calls.
// Every function is synchronous and blocking. The Switch code refreshes the
// screen itself through consoleUpdate() between calls.

#pragma once

/* Linkage guard: this header is included from C++ files (the Borealis UI).
 * Without it, every include site has to remember to wrap it in `extern "C"` -
 * forgetting once gives a link error on a mangled symbol, far from the cause.
 * The guard belongs to the header. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

// Dynamic buffer for the HTTP response. Free it with http_free().
typedef struct {
    char *data;        // null-terminated, or NULL when there is no payload
    size_t len;        // length excluding the final \0
    long status;       // code HTTP (200, 401, ...)
} http_response;

// Global libcurl init/cleanup. Call once at startup / shutdown.
bool http_global_init(void);
void http_global_cleanup(void);

/* AF3 2026-09-10 - declares the application as EXITING. From then on, a
 * request still in flight is abandoned once it has had HTTP_SHUTDOWN_GRACE_MS
 * to finish: enough for the DELETE of our clients (~100 ms measured), not
 * enough for a silent server to hold the exit for 30 s. Idempotent, callable
 * from any thread. See http.c. */
void http_request_shutdown(void);

// Frees an http_response's internal buffer.
void http_free(http_response *r);

// GET the URL. When bearer != NULL, adds Authorization: Bearer <bearer>.
// Returns true when the transport succeeded (the status code is then
// available). Inspect r->status for logical success.
bool http_get(const char *url, const char *bearer, http_response *out);

// POST the URL with a JSON body. When bearer != NULL, adds
// Authorization: Bearer. When body == NULL or body_len == 0, POSTs an empty
// body.
bool http_post_json(const char *url, const char *bearer, const char *body, size_t body_len, http_response *out);

// POST the URL with an application/x-www-form-urlencoded body. For the OAuth
// token endpoints.
bool http_post_form(const char *url, const char *form_body, http_response *out);

// UX1 2026-05-18 — Network resilience helpers.

// Check connectivity to internet (= HEAD request to https://1.1.1.1, 3s timeout).
// Retourne true si reachable (= n'importe quel statut HTTP indique connectivity).
bool http_check_connectivity(void);

// Wraps http_get with an exponential backoff retry.
// max_retries = 3 by default, backoff starts at 2 s and doubles each attempt.
// Returns true when one of the attempts succeeded.
bool http_get_retry(const char *url, const char *bearer, http_response *out, int max_retries);

/* Extracts a string field `"key": "value"` from a JSON body.
 *
 * Deliberately minimal: it scans the whole body, which also finds nested fields
 * (`data.id`), but it handles neither objects nor complex escapes. That is
 * enough for the Shadow replies, which are flat and short. `cap` includes the
 * terminating zero; returns false when the field is absent.
 *
 * It used to be copied identically into ctrl_rest.c and shadowusb.c - the same
 * code with two different comments. Unified here on 2026-08-25. */
bool http_json_get_string(const char *json, const char *key,
                          char *out, size_t cap);

/* Attaches the process-wide DNS + TLS-session cache to a curl handle. Call it
 * on every handle created outside http.c: without it that request pays a full
 * TLS handshake, which on a slow console is seconds, not milliseconds. */
struct Curl_easy;
void shadow_curl_apply_share(void *h);

/* === WIN2 2026-10-02 - THE CERTIFICATE STORE, FOR A HANDLE WE DID NOT BUILD =
 *
 * Fourteen `curl_easy_init()` handles live OUTSIDE this file (launcher,
 * proximus, oauth, ctrl_rest, tinag, shadowusb). They each set their own
 * options, and every one of them sets SSL_VERIFYPEER=1 and then says nothing
 * about WHICH authorities to verify against - so each relied on libcurl's
 * built-in default.
 *
 * On a Windows build whose DLLs sit beside the executable that default is
 * wrong, and silently: MSYS2's libcurl derives its CA path from its own DLL's
 * directory (see SHADOW_CA_DEFAULT in http.c). Fixing `common_setopts` fixed
 * only the five handles in this file; `vms/<id>/capabilities` and
 * `vms/<id>/turn-servers` kept failing with CURLE_SSL_CACERT_BADFILE, reported
 * as `capabilities: HTTP 0 (non-fatal, continuing)` because `get_with_vmid`
 * neither logs the CURLcode nor sets a status - and then /vm/start failed the
 * same way, which is the "the very first step failed" that was reported.
 *
 * So the rule has ONE home and every handle asks for it, exactly as every
 * handle already asks for the DNS/TLS share next door. A handle that forgets is
 * a handle that trusts whatever the host happens to hold - which is why this
 * sits beside `shadow_curl_apply_share` rather than inside a helper only this
 * file calls. */
void shadow_curl_apply_ca(void *h);

/* === DIAG1 2026-10-02 - SAY WHY A TRANSFER FAILED, WHEREVER IT FAILED ======
 *
 * The thirteen handles outside this file each call `curl_easy_perform` and
 * then throw the `CURLcode` away. Two of them do not even set an HTTP status,
 * so the caller sees 0 and reports it as a status - which is how
 *
 *     capabilities: HTTP 0 (non-fatal, continuing)
 *
 * came to mean "the CA bundle could not be opened" for an hour. Zero is not a
 * status; it means the request never got an answer, and only the CURLcode says
 * what happened instead.
 *
 * So: one line, after every perform, with the code, curl's own text, and the
 * HTTP status when there is one. In the JOURNAL, not on stderr - the journal is
 * what a user can send and what `devlink` ships; stderr is lost the moment the
 * window closes.
 *
 * `what` names the call ("capabilities", "vm/start", "sse"), because a code
 * with no subject is as unhelpful as a status of 0. It logs NOTHING on success
 * beyond what the caller already logs: a line per successful request would bury
 * the one that matters. */
void shadow_curl_report(void *h, int curlcode, const char *what);


#ifdef __cplusplus
}
#endif
