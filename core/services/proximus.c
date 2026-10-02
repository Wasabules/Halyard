/* memmem is a GNU extension - _GNU_SOURCE must come before the includes. */
#define _GNU_SOURCE
#if !defined(__SWITCH__) && !defined(_WIN32)
#include <sys/utsname.h>
#endif
#include "proximus.h"
#include "http.h"
#include "config.h"

#include <jansson.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
/* localtime_r: MinGW UCRT does not expose localtime_r - shimmed through
 * localtime_s (arguments swapped). journal.c carries the same shim. */
static inline struct tm *shadow_localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) == 0 ? out : NULL;
}
#  define localtime_r(t, out) shadow_localtime_r((t), (out))

/* memmem: a GNU extension, absent on Windows. A naive O(n*m) implementation,
 * amply sufficient for the SSE buffers (<8 KB per chunk). */
static const void *shadow_memmem(const void *haystack, size_t hs,
                                  const void *needle,   size_t ns) {
    if (ns == 0) return haystack;
    if (hs < ns) return NULL;
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    size_t last = hs - ns;
    for (size_t i = 0; i <= last; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, ns) == 0) return h + i;
    }
    return NULL;
}
#  define memmem(h, hs, n, ns) shadow_memmem((h), (hs), (n), (ns))
#endif

// device.uuid persistence shared with launcher.c - reachable through a getter.
extern const char *shadow_device_uuid_public(void);

static char *jstr(json_t *root, const char *key) {
    json_t *v = json_object_get(root, key);
    if (!v) return NULL;
    if (json_is_string(v)) {
        const char *s = json_string_value(v);
        return s ? strdup(s) : NULL;
    }
    return NULL;
}

static size_t accept_writer(void *ptr, size_t size, size_t nmemb, void *userdata) {
    http_response *r = (http_response *)userdata;
    size_t n = size * nmemb;
    char *grown = realloc(r->data, r->len + n + 1);
    if (!grown) return 0;
    r->data = grown;
    memcpy(r->data + r->len, ptr, n);
    r->len += n;
    r->data[r->len] = '\0';
    return n;
}

// curl debug for the calls to compute.shadow.tech - a separate file so it can
// be told apart from the launcher trace (api.eu.shadow.tech).
static FILE *g_proximus_debug = NULL;
static int proximus_debug_cb(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    (void)handle; (void)userptr;
    if (!g_proximus_debug) return 0;
    const char *prefix = "?";
    switch (type) {
        case CURLINFO_TEXT:        prefix = "[curl] "; break;
        case CURLINFO_HEADER_OUT:  prefix = "[>>] "; break;
        case CURLINFO_HEADER_IN:   prefix = "[<<] "; break;
        case CURLINFO_DATA_OUT:    prefix = "[> body] "; break;
        case CURLINFO_DATA_IN:     prefix = "[< body] "; break;
        default: return 0;
    }
    fputs(prefix, g_proximus_debug);
    fwrite(data, 1, size, g_proximus_debug);
    fputc('\n', g_proximus_debug);
    fflush(g_proximus_debug);
    return 0;
}
static void enable_proximus_debug(CURL *h) {
    if (!g_proximus_debug) g_proximus_debug = fopen(SHADOW_DATA_DIR "proximus_trace.log", "a");
    if (g_proximus_debug) {
        curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(h, CURLOPT_DEBUGFUNCTION, proximus_debug_cb);
    }
}

// POST JSON with a custom Bearer - no X-Vm-Id, we talk to the VM host directly.
static bool post_json_bearer(const char *url, const char *bearer, const char *body, http_response *out) {
    out->data = NULL; out->len = 0; out->status = 0;
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    shadow_curl_apply_share(h);   /* DNS + session TLS partagees */
    if (!h) return false;
    enable_proximus_debug(h);
    struct curl_slist *headers = NULL;
    char auth[8192];  // the proximus JWT is large (~2 KB)
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    /* The official desktop app does NOT send Origin/Referer (those headers
     * signal a web browser client). We send ONLY X-Shadow-Agent and the UA. */
    headers = curl_slist_append(headers, "X-Shadow-Agent: " SHADOW_X_AGENT);

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    if (body) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    } else {
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
    }
    curl_easy_setopt(h, CURLOPT_USERAGENT, SHADOW_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, SHADOW_HTTP_CONNECT_TIMEOUT);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, SHADOW_HTTP_TOTAL_TIMEOUT);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, accept_writer);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(h);
    shadow_curl_report(h, rc, "post_json_bearer");   /* DIAG1 */
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return rc == CURLE_OK;
}

// Builds "<proximus_url>/<suffix>", handling the possible trailing slash.
static char *make_proximus_url(const char *proximus_url, const char *suffix) {
    if (!proximus_url || !suffix) return NULL;
    size_t pl = strlen(proximus_url);
    bool slash = pl > 0 && proximus_url[pl-1] == '/';
    size_t n = pl + strlen(suffix) + 4;
    char *u = malloc(n);
    if (!u) return NULL;
    snprintf(u, n, "%s%s%s", proximus_url, slash ? "" : "/", suffix);
    return u;
}

static char *make_clients_url(const char *proximus_url) {
    return make_proximus_url(proximus_url, "clients");
}

static bool get_json_bearer(const char *url, const char *bearer, http_response *out) {
    out->data = NULL; out->len = 0; out->status = 0;
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    shadow_curl_apply_share(h);   /* DNS + session TLS partagees */
    if (!h) return false;
    enable_proximus_debug(h);
    struct curl_slist *headers = NULL;
    char auth[8192]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: application/json");
    /* The desktop app sends no Origin/Referer (= it is not a web client). */
    headers = curl_slist_append(headers, "X-Shadow-Agent: " SHADOW_X_AGENT);

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_USERAGENT, SHADOW_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, SHADOW_HTTP_CONNECT_TIMEOUT);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, SHADOW_HTTP_TOTAL_TIMEOUT);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, accept_writer);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(h);
    shadow_curl_report(h, rc, "get_json_bearer");   /* DIAG1 */
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return rc == CURLE_OK;
}

// Builds the expected "<json string>" opaque: client metadata JSON-encoded
// inside a JSON string.
// The 2026-05-06 HAR confirms the exact format Chrome sends on /7/clients:
//   {"arch":"x86_64","os-name":"Linux","os-version":"<full Chrome UA>",
//    "platform-type":"web","os":"Linux","timestamp":"2026-05-06 00:52:40"[,"device-id":"<uuid>"]}
// The os-version field carries the FULL User-Agent, not a short version - the
// server-side QoS scoring may well check it against the HTTP UA.
// device-id is added only for the main client.
/* Different opaques per client type (LD_PRELOAD capture, 2026-05-06):
 *
 * LAUNCHER (Electron):
 *   {"os":"Linux","arch":"x64","platform-type":"desktop",
 *    "os-version":"<kernel>","os-name":"Linux","timestamp":"..."}
 *
 * MAIN (the ShadowPCDisplay renderer):
 *   {"arch":"x86_64","device-id":"<40-hex>","os":"linux",
 *    "os-name":"Ubuntu","os-version":"24.04","platform-type":"desktop",
 *    "timestamp":"...","version":"1.0.0"}
 *
 * `include_device_id=true` => build the MAIN body (with device-id and version).
 * Otherwise => build the LAUNCHER body (kernel os-version, arch=x64, os=Linux
 * capitalised).
 */
static char *build_opaque(bool include_device_id) {
    json_t *o = json_object();
    char ts[32]; time_t now = time(NULL); struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    if (include_device_id) {
        /* Body main renderer */
        json_object_set_new(o, "arch", json_string("x86_64"));
        json_object_set_new(o, "device-id", json_string(shadow_device_uuid_public()));
        json_object_set_new(o, "os", json_string("linux"));
        json_object_set_new(o, "os-name", json_string("Ubuntu"));
        json_object_set_new(o, "os-version", json_string("24.04"));
        json_object_set_new(o, "platform-type", json_string("desktop"));
        json_object_set_new(o, "timestamp", json_string(ts));
        json_object_set_new(o, "version", json_string("1.0.0"));
    } else {
        /* Body launcher Electron */
        json_object_set_new(o, "arch", json_string("x64"));
        json_object_set_new(o, "os", json_string("Linux"));
        json_object_set_new(o, "os-name", json_string("Linux"));
        /* Read the real kernel version. Hardcoded fallback when uname is not
         * available (Switch, Windows - we must always send a Linux kernel
         * because the Shadow server checks the "Linux;x64;..." UA; returning a
         * Windows string would get us treated as a web client). */
#if defined(__SWITCH__) || defined(_WIN32)
        json_object_set_new(o, "os-version", json_string("6.17.0-22-generic"));
#else
        struct utsname u;
        if (uname(&u) == 0)
            json_object_set_new(o, "os-version", json_string(u.release));
        else
            json_object_set_new(o, "os-version", json_string("6.17.0-22-generic"));
#endif
        json_object_set_new(o, "platform-type", json_string("desktop"));
        json_object_set_new(o, "timestamp", json_string(ts));
    }

    char *s = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    return s;
}

static char *build_clients_body(const char *type, bool include_device_id) {
    char *opaque_json = build_opaque(include_device_id);
    if (!opaque_json) return NULL;

    json_t *outer = json_object();
    json_object_set_new(outer, "type", json_string(type));
    json_object_set_new(outer, "opaque", json_string(opaque_json));  // JSON-encoded string
    free(opaque_json);
    char *s = json_dumps(outer, JSON_COMPACT);
    json_decref(outer);
    return s;
}

void proximus_launcher_session_free(ProximusLauncherSession *s) {
    if (!s) return;
    free(s->id); free(s->spice_secret);
    memset(s, 0, sizeof(*s));
}

void proximus_main_session_free(ProximusMainSession *s) {
    if (!s) return;
    free(s->id); free(s->streaming_token);
    memset(s, 0, sizeof(*s));
}

static bool parse_client_response(const char *resp_data, json_t **out_data) {
    json_error_t err;
    json_t *root = json_loads(resp_data, 0, &err);
    if (!root) return false;
    json_t *data = json_object_get(root, "data");
    if (!data || !json_is_object(data)) { json_decref(root); return false; }
    *out_data = root;  // the caller decrefs
    return true;
}

bool proximus_create_launcher_client(const char *proximus_url, const char *launcher_jwt,
                                     ProximusLauncherSession *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char *url = make_clients_url(proximus_url);
    char *body = build_clients_body("launcher", false);
    if (!url || !body) { free(url); free(body); return false; }

    http_response resp;
    bool t = post_json_bearer(url, launcher_jwt, body, &resp);
    free(url); free(body);
    if (http_status) *http_status = resp.status;
    // Always dumped for RE - the APK's 1301X ports may well come from here.
    if (resp.data) {
        FILE *f = fopen(SHADOW_DATA_DIR "last_clients_launcher.json", "w");
        if (f) { fwrite(resp.data, 1, resp.len, f); fclose(f); }
    }
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_t *root = NULL;
    bool ok = parse_client_response(resp.data, &root);
    http_free(&resp);
    if (!ok) return false;

    json_t *data = json_object_get(root, "data");
    out->id = jstr(data, "id");
    out->spice_secret = jstr(data, "spice_secret");
    json_decref(root);
    return out->spice_secret != NULL;
}

bool proximus_create_main_client(const char *proximus_url, const char *main_jwt,
                                 ProximusMainSession *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char *url = make_clients_url(proximus_url);
    char *body = build_clients_body("main", true);
    if (!url || !body) { free(url); free(body); return false; }

    http_response resp;
    bool t = post_json_bearer(url, main_jwt, body, &resp);
    free(url); free(body);
    if (http_status) *http_status = resp.status;
    if (resp.data) {
        FILE *f = fopen(SHADOW_DATA_DIR "last_clients_main.json", "w");
        if (f) { fwrite(resp.data, 1, resp.len, f); fclose(f); }
    }
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_t *root = NULL;
    bool ok = parse_client_response(resp.data, &root);
    http_free(&resp);
    if (!ok) return false;

    json_t *data = json_object_get(root, "data");
    out->id = jstr(data, "id");
    out->streaming_token = jstr(data, "streamingtoken");
    json_t *jexp = json_object_get(data, "streamingtoken_expiry");
    out->streaming_token_expiry = (jexp && json_is_integer(jexp)) ? (long long)json_integer_value(jexp) : 0;
    json_decref(root);
    return out->streaming_token != NULL;
}

void proximus_usb_session_free(ProximusUsbSession *s) {
    if (!s) return;
    free(s->id);
    memset(s, 0, sizeof(*s));
}

bool proximus_create_usb_client(const char *proximus_url, const char *usb_jwt,
                                ProximusUsbSession *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char *url = make_clients_url(proximus_url);
    char *body = build_clients_body("usb", true);
    if (!url || !body) { free(url); free(body); return false; }

    http_response resp;
    bool t = post_json_bearer(url, usb_jwt, body, &resp);
    free(url); free(body);
    if (http_status) *http_status = resp.status;
    if (resp.data) {
        FILE *f = fopen(SHADOW_DATA_DIR "last_clients_usb.json", "w");
        if (f) { fwrite(resp.data, 1, resp.len, f); fclose(f); }
    }
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_t *root = NULL;
    bool ok = parse_client_response(resp.data, &root);
    http_free(&resp);
    if (!ok) return false;

    json_t *data = json_object_get(root, "data");
    out->id = jstr(data, "id");
    json_decref(root);
    return out->id != NULL;
}

// ============================================================================
// M6 : status / forward / stream
// ============================================================================

void proximus_status_free(ProximusStatus *s) {
    if (!s) return;
    free(s->vm_status);
    memset(s, 0, sizeof(*s));
}

bool proximus_get_status(const char *proximus_url, const char *jwt,
                         ProximusStatus *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char *url = make_proximus_url(proximus_url, "status");
    if (!url) return false;

    http_response resp;
    bool t = get_json_bearer(url, jwt, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    json_t *data = json_object_get(root, "data");
    if (!data || !json_is_object(data)) { json_decref(root); return false; }

    out->vm_status   = jstr(data, "vm_status");
    json_t *jr = json_object_get(data, "reachable");
    json_t *js = json_object_get(data, "streamer_up");
    out->reachable   = (jr && json_is_true(jr));
    out->streamer_up = (js && json_is_true(js));

    json_decref(root);
    return true;
}

void proximus_forward_result_free(ProximusForwardResult *r) {
    if (!r) return;
    free(r->id);
    memset(r, 0, sizeof(*r));
}

bool proximus_forward_event(const char *proximus_url, const char *jwt,
                            const char *event_type,
                            ProximusForwardResult *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char *url = make_proximus_url(proximus_url, "forward");
    if (!url) return false;

    // {"command":"forward","data":{"type":"<event_type>","sender":"launcher"}}
    json_t *data = json_object();
    json_object_set_new(data, "type", json_string(event_type));
    json_object_set_new(data, "sender", json_string("launcher"));
    json_t *outer = json_object();
    json_object_set_new(outer, "command", json_string("forward"));
    json_object_set_new(outer, "data", data);
    char *body = json_dumps(outer, JSON_COMPACT);
    json_decref(outer);
    if (!body) { free(url); return false; }

    http_response resp;
    bool t = post_json_bearer(url, jwt, body, &resp);
    free(url); free(body);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    out->id = jstr(root, "id");
    json_decref(root);
    return out->id != NULL;
}

// SSE stream - we dump the received content into a file as it arrives.
// The server keeps the connection open indefinitely, so we cut it through a
// short CURLOPT_TIMEOUT; CURLE_OPERATION_TIMEDOUT is expected and is NOT a
// failure.
typedef struct { FILE *out; size_t bytes; } sse_ctx;

static size_t sse_writer(void *ptr, size_t size, size_t nmemb, void *userdata) {
    sse_ctx *c = (sse_ctx *)userdata;
    size_t n = size * nmemb;
    if (c->out) { fwrite(ptr, 1, n, c->out); fflush(c->out); }
    c->bytes += n;
    return n;
}

bool proximus_dump_stream(const char *proximus_url, const char *jwt,
                          const char *out_path, int seconds, long *http_status) {
    if (http_status) *http_status = 0;
    char *url = make_proximus_url(proximus_url, "stream");
    if (!url) return false;

    sse_ctx ctx = { fopen(out_path, "w"), 0 };

    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    shadow_curl_apply_share(h);   /* DNS + session TLS partagees */
    if (!h) { free(url); if (ctx.out) fclose(ctx.out); return false; }
    enable_proximus_debug(h);

    struct curl_slist *headers = NULL;
    char auth[8192]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", jwt);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, "Origin: " SHADOW_ORIGIN);
    headers = curl_slist_append(headers, "Referer: " SHADOW_REFERER);
    headers = curl_slist_append(headers, "X-Shadow-Agent: " SHADOW_X_AGENT);

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_USERAGENT, SHADOW_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, SHADOW_HTTP_CONNECT_TIMEOUT);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)seconds);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sse_writer);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(h);
    shadow_curl_report(h, rc, "proximus_dump_stream");   /* DIAG1 */
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (http_status) *http_status = status;

    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    if (ctx.out) fclose(ctx.out);
    free(url);

    // CURLE_OPERATION_TIMEDOUT is expected: we are the ones cutting. As long as
    // we received at least a few bytes and a 200 status, it is a success.
    return (rc == CURLE_OK || rc == CURLE_OPERATION_TIMEDOUT) && status == 200 && ctx.bytes > 0;
}

// === SSE keepalive ===
//
// Keeps the /stream connection open for the whole duration of the session.
// Found on the WebRTC path (abandoned since): without it the Shadow VM cut the
// video at exactly t=120 s (the browser keeps this SSE permanently open, cf.
// memory project_shadow_protocol_har). The native bootstrap needs TWO of them
// (launcher + main JWT) before it will bind :base+11 (KB §3.17).
//
// curl pumps the bytes through the write callback. When abort_flag goes to 1 the
// callback returns 0 -> CURLE_WRITE_ERROR -> curl_easy_perform returns.

struct proximus_sse_keepalive {
    pthread_t  thread;
    char      *url;
    char      *jwt;
    char      *suffix;            /* "stream" or "status" - which SSE endpoint */
    volatile int abort_flag;
    bool       running;
    volatile int acquisition_ready;  /* set to 1 when an event is received */
};

/* Public getter - used by smoke_test to wait on the event */
int proximus_sse_acquisition_ready(struct proximus_sse_keepalive *k) {
    return k ? k->acquisition_ready : 0;
}

/* Review 2026-08-21 - without this callback, abort_flag was observable ONLY
 * from the writer, hence only when the server was sending bytes. On a silent
 * stream, curl_easy_perform never returned and proximus_sse_stop's pthread_join
 * blocked forever - a regression introduced by D9, since before the loop the
 * thread exited after its single perform. On Switch this is the §7.3 violation
 * (a long-lived thread must see abort_flag within 100 ms) that D9's comment
 * claimed to respect. curl calls this callback at least once a second; returning
 * non-zero aborts the transfer cleanly. */
static int sse_abort_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    struct proximus_sse_keepalive *k = userdata;
    return (k && k->abort_flag) ? 1 : 0;
}

static size_t sse_keepalive_writer(void *ptr, size_t size, size_t nmemb,
                                    void *userdata) {
    struct proximus_sse_keepalive *k = userdata;
    size_t total = size * nmemb;
    if (!k || k->abort_flag) return 0;
    if (!ptr || total == 0) return total;

    const char *buf = (const char *)ptr;

    /* Looks for "data:" - logs the line up to the \n. Safe on partial chunks. */
    if (total >= 5) {
        const char *p = memmem(buf, total, "data:", 5);
        if (p) {
            size_t avail = total - (size_t)(p - buf);
            const char *eol = memchr(p, '\n', avail);
            size_t line_len = eol ? (size_t)(eol - p) : avail;
            if (line_len > 240) line_len = 240;
            fprintf(stderr, "[sse] %.*s\n", (int)line_len, p);
            fflush(stderr);
            /* Detects the critical events. */
            if (memmem(p, line_len, "acquisition_is_ready", 20)) {
                k->acquisition_ready = 1;
                fprintf(stderr, "[sse] *** acquisition_is_ready DETECTED ***\n");
                fflush(stderr);
            }
        }
    }
    return total;
}

static void *sse_keepalive_thread(void *arg) {
    struct proximus_sse_keepalive *k = arg;
    char *url = make_proximus_url(k->url, k->suffix ? k->suffix : "stream");
    if (!url) { fprintf(stderr, "[sse] make_url FAIL\n"); k->running = false; return NULL; }

    fprintf(stderr, "[sse] connecting %s\n", url); fflush(stderr);
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    shadow_curl_apply_share(h);   /* DNS + session TLS partagees */
    if (!h) { fprintf(stderr, "[sse] curl_easy_init FAIL\n"); free(url); k->running = false; return NULL; }
    enable_proximus_debug(h);

    struct curl_slist *headers = NULL;
    char auth[8192]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", k->jwt);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, "Origin: " SHADOW_ORIGIN);
    headers = curl_slist_append(headers, "Referer: " SHADOW_REFERER);
    headers = curl_slist_append(headers, "X-Shadow-Agent: " SHADOW_X_AGENT);

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_USERAGENT, SHADOW_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, SHADOW_HTTP_CONNECT_TIMEOUT);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sse_keepalive_writer);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, k);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);          /* revue : abandon */
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, sse_abort_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, k);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    /* No global TIMEOUT - we cut through abort_flag in the callback. */

    CURLcode rc = curl_easy_perform(h);
    shadow_curl_report(h, rc, "sse_keepalive_thread");   /* DIAG1 */
    long http_status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_status);
    fprintf(stderr, "[sse] perform exit rc=%d http_status=%ld error=%s\n",
            (int)rc, http_status, curl_easy_strerror(rc));
    fflush(stderr);

    curl_slist_free_all(headers);
    curl_easy_cleanup(h);

    /* D9 2026-08-21 - RECONNECTION. This thread is called "keepalive" but did
     * only ONE curl_easy_perform: as soon as the transfer ended (end of the
     * server stream, network cut, timeout), the SSE was dead for ALL the rest of
     * the session, with nothing to signal it.
     *
     * Measured consequence (KB §3.17): the GUI opens the second SSE on /status
     * (matching the desktop); that stream breaks mid-session, and the server
     * closes the session ~118 s later - 4 deaths in 4 GUI runs. The headless
     * binary survived 5/5 only because it opens /stream for both, a stream that
     * does not end by itself. Since the SSE is what holds the server binding
     * (cf. DUAL SSE, §3.5), its silent death means teardown.
     *
     * So we loop as long as abort_flag is not raised, with a short backoff
     * polled at 100 ms to stay responsive to the stop (Switch constraint: every
     * long-lived thread must see abort_flag within 100 ms, cf. §7.3).
     * SHADOW_SSE_RECONNECT=0 goes back to the one-shot behaviour. */
    static int g_sse_reconnect = -1;
    if (g_sse_reconnect < 0) {
        const char *e = getenv("SHADOW_SSE_RECONNECT");
        g_sse_reconnect = e ? atoi(e) : 1;
    }
    unsigned attempt = 0;
    while (g_sse_reconnect && !k->abort_flag) {
        attempt++;
        /* Review 2026-08-21 - a GROWING backoff capped at 5 s. The first version
         * retried every 500 ms with no limit: on an expired JWT (a 401 in
         * ~200 ms) the two threads redid ~170 full TLS handshakes a minute for
         * the whole session, never giving up. */
        int slices = 5 * (attempt < 10 ? (int)attempt : 10);
        if (slices > 50) slices = 50;
        for (int i = 0; i < slices && !k->abort_flag; i++) {
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        if (k->abort_flag) break;
        fprintf(stderr, "[sse] reconnect #%u %s (precedent rc=%d http=%ld)\n",
                attempt, url, (int)rc, http_status);
        fflush(stderr);

        CURL *h2 = curl_easy_init();
        shadow_curl_apply_ca(h2);   /* WIN2 - see http.h */
        shadow_curl_apply_share(h2);   /* DNS + session TLS partagees */
        if (!h2) break;
        enable_proximus_debug(h2);
        struct curl_slist *hdr2 = NULL;
        char auth2[8192];
        snprintf(auth2, sizeof(auth2), "Authorization: Bearer %s", k->jwt);
        hdr2 = curl_slist_append(hdr2, auth2);
        hdr2 = curl_slist_append(hdr2, "Accept: text/event-stream");
        hdr2 = curl_slist_append(hdr2, "Origin: " SHADOW_ORIGIN);
        hdr2 = curl_slist_append(hdr2, "Referer: " SHADOW_REFERER);
        hdr2 = curl_slist_append(hdr2, "X-Shadow-Agent: " SHADOW_X_AGENT);
        curl_easy_setopt(h2, CURLOPT_URL, url);
        curl_easy_setopt(h2, CURLOPT_HTTPHEADER, hdr2);
        curl_easy_setopt(h2, CURLOPT_USERAGENT, SHADOW_USER_AGENT);
        curl_easy_setopt(h2, CURLOPT_CONNECTTIMEOUT, SHADOW_HTTP_CONNECT_TIMEOUT);
        curl_easy_setopt(h2, CURLOPT_WRITEFUNCTION, sse_keepalive_writer);
        curl_easy_setopt(h2, CURLOPT_WRITEDATA, k);
        curl_easy_setopt(h2, CURLOPT_NOPROGRESS, 0L);      /* revue : abandon */
        curl_easy_setopt(h2, CURLOPT_XFERINFOFUNCTION, sse_abort_cb);
        curl_easy_setopt(h2, CURLOPT_XFERINFODATA, k);
        curl_easy_setopt(h2, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h2, CURLOPT_SSL_VERIFYHOST, 2L);
        rc = curl_easy_perform(h2);
        shadow_curl_report(h2, rc, "sse_keepalive_thread");   /* DIAG1 */
        http_status = 0;
        curl_easy_getinfo(h2, CURLINFO_RESPONSE_CODE, &http_status);
        fprintf(stderr, "[sse] reconnect #%u exit rc=%d http_status=%ld error=%s\n",
                attempt, (int)rc, http_status, curl_easy_strerror(rc));
        fflush(stderr);
        curl_slist_free_all(hdr2);
        curl_easy_cleanup(h2);
    }

    free(url);
    k->running = false;
    return NULL;
}

proximus_sse_keepalive *proximus_sse_start(const char *proximus_url,
                                           const char *jwt) {
    return proximus_sse_start_ex(proximus_url, jwt, "stream");
}

proximus_sse_keepalive *proximus_sse_start_ex(const char *proximus_url,
                                                const char *jwt,
                                                const char *suffix) {
    if (!proximus_url || !jwt || !suffix) return NULL;
    struct proximus_sse_keepalive *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->url = strdup(proximus_url);
    k->jwt = strdup(jwt);
    k->suffix = strdup(suffix);
    k->abort_flag = 0;
    k->running = true;
    if (!k->url || !k->jwt || !k->suffix) {
        free(k->url); free(k->jwt); free(k->suffix); free(k);
        return NULL;
    }
    if (pthread_create(&k->thread, NULL, sse_keepalive_thread, k) != 0) {
        free(k->url); free(k->jwt); free(k->suffix); free(k);
        return NULL;
    }
    return k;
}

void proximus_sse_stop(proximus_sse_keepalive *k) {
    if (!k) return;
    k->abort_flag = 1;
    pthread_join(k->thread, NULL);
    free(k->url);
    free(k->jwt);
    free(k->suffix);
    free(k);
}

/* === K14 2026-08-21 - listing and deleting the VM's clients ===
 * The official client does a `GET /N/clients` BEFORE creating its own, and above
 * all a `DELETE /N/clients/{id}` at the end of the session. We never deleted
 * ours: after dozens of test sessions the VM can accumulate phantom clients.
 * Hypothesis to test: a newcomer facing already-registered clients would be
 * treated as secondary - which would fit the symptom "the server tracks our
 * cursor but injects nothing" (KB §3.21). */
bool proximus_list_clients(const char *proximus_url, const char *jwt,
                            char **out_json, long *http_status) {
    if (out_json) *out_json = NULL;
    if (http_status) *http_status = 0;
    char *url = make_proximus_url(proximus_url, "clients");
    if (!url) return false;
    http_response resp;
    bool t = get_json_bearer(url, jwt, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    if (!t || !resp.data) { http_free(&resp); return false; }
    if (out_json) *out_json = strdup(resp.data);
    bool ok = (resp.status >= 200 && resp.status < 300);
    http_free(&resp);
    return ok;
}

bool proximus_delete_client(const char *proximus_url, const char *jwt,
                             const char *client_id, long *http_status) {
    if (http_status) *http_status = 0;
    if (!client_id || !*client_id) return false;
    char sub[256];
    snprintf(sub, sizeof(sub), "clients/%s", client_id);
    char *url = make_proximus_url(proximus_url, sub);
    if (!url) return false;
    /* DELETE through CURLOPT_CUSTOMREQUEST - the same headers as
     * get_json_bearer (including X-Shadow-Agent, mandatory, cf. KB §4.3). */
    http_response resp; resp.data = NULL; resp.len = 0; resp.status = 0;
    bool t = false;
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    shadow_curl_apply_share(h);   /* DNS + session TLS partagees */
    if (h) {
        enable_proximus_debug(h);
        struct curl_slist *hd = NULL;
        char auth[8192]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", jwt);
        hd = curl_slist_append(hd, auth);
        hd = curl_slist_append(hd, "Accept: application/json");
        hd = curl_slist_append(hd, "Content-Type: application/json");
        hd = curl_slist_append(hd, "X-Shadow-Agent: " SHADOW_X_AGENT);
        curl_easy_setopt(h, CURLOPT_URL, url);
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hd);
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(h, CURLOPT_USERAGENT, SHADOW_USER_AGENT);
        /* Certificate verification RESTORED on 2026-08-25. This call went out with
         * VERIFYPEER=0 and VERIFYHOST=0 from the moment it was written, while it
         * targets the SAME proximus host as the six other requests in this file,
         * which leave it on - and while it carries the session JWT in the
         * Authorization header. Nothing in the history suggests a deliberate
         * workaround: the function was added in one block with those values.
         * Since the certificate validates for the other calls, it validates for
         * this one. */
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, NULL);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, NULL);
        CURLcode rc = curl_easy_perform(h);
        shadow_curl_report(h, rc, "proximus_delete_client");   /* DIAG1 */
        if (rc == CURLE_OK) {
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp.status);
            t = true;
        } else {
            fprintf(stderr, "[proximus] DELETE %s -> curl rc=%d (%s)\n",
                    url, (int)rc, curl_easy_strerror(rc));
        }
        curl_slist_free_all(hd);
        curl_easy_cleanup(h);
    }
    free(url);
    if (http_status) *http_status = resp.status;
    bool ok = t && resp.status >= 200 && resp.status < 300;
    http_free(&resp);
    return ok;
}
