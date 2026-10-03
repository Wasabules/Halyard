#include "launcher.h"
#include "jwt.h"
#include "http.h"
#include "config.h"
#include "journal.h"

#include <jansson.h>
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// X-Shadow-Uuid: a stable v4 UUID persisted to the SD card on the first launch.
// The Shadow server probably uses it for telemetry / rate-limiting, not for
// auth - any valid v4 UUID does.
static char g_device_uuid[42] = {0};  /* 40 hex chars + NUL + slack */

/* The v4 formatter that used to live here is gone: the identifier the server
 * wants is the official client's 40 hex characters, not a 36-character dashed
 * UUID, and `shadow_device_uuid()` below has built the former since. It had no
 * caller left. */

static const char *shadow_device_uuid(void) {
    if (g_device_uuid[0]) return g_device_uuid;

    /* The official desktop app's format = 40 hex chars (= a SHA-1 hash without
     * dashes). Different from the 36-char v4 UUID we used before. The Shadow
     * server probably filters clients with a "non-sha1" device-id (= web/APK
     * clients).
     *
     * On Linux desktop we read `/etc/machine-id` (32 stable hex) plus 8 random
     * hex to reach 40. Stable per machine. */
    FILE *f = fopen(SHADOW_DEVICE_UUID_PATH, "r");
    if (f) {
        size_t n = fread(g_device_uuid, 1, 40, f);
        fclose(f);
        if (n == 40) { g_device_uuid[40] = '\0'; return g_device_uuid; }
    }

#ifndef __SWITCH__
    /* Try the Linux machine-id for stability. */
    FILE *m = fopen("/etc/machine-id", "r");
    if (m) {
        char mid[33] = {0};
        if (fread(mid, 1, 32, m) == 32) {
            mid[32] = 0;
            /* Convert chars to lowercase hex (already is) — append 8 random hex chars */
            unsigned int extra[2];
            srand((unsigned)time(NULL));
            extra[0] = (unsigned)rand();
            extra[1] = (unsigned)rand();
            snprintf(g_device_uuid, sizeof(g_device_uuid),
                     "%s%04x%04x", mid, extra[0] & 0xFFFF, extra[1] & 0xFFFF);
            fclose(m);
            f = fopen(SHADOW_DEVICE_UUID_PATH, "w");
            if (f) { fwrite(g_device_uuid, 1, 40, f); fclose(f); }
            return g_device_uuid;
        }
        fclose(m);
    }
#endif

    /* Fallback: 40 random hex. */
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&seeded); seeded = 1; }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 40; i++) g_device_uuid[i] = hex[rand() & 0xF];
    g_device_uuid[40] = '\0';

    f = fopen(SHADOW_DEVICE_UUID_PATH, "w");
    if (f) { fwrite(g_device_uuid, 1, 40, f); fclose(f); }
    return g_device_uuid;
}

static char *jstrdup(json_t *root, const char *key) {
    json_t *v = json_object_get(root, key);
    if (!v) return NULL;
    if (json_is_string(v)) {
        const char *s = json_string_value(v);
        return s ? strdup(s) : NULL;
    }
    if (json_is_integer(v)) {
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)json_integer_value(v));
        return strdup(buf);
    }
    return NULL;
}

// Builds "<base>/<path>", making sure there is exactly ONE "/" between them.
static char *make_url(const char *base, const char *path) {
    if (!base || !path) return NULL;
    size_t bl = strlen(base);
    size_t pl = strlen(path);
    bool base_slash = bl > 0 && base[bl-1] == '/';
    bool path_slash = pl > 0 && path[0] == '/';
    size_t n = bl + pl + 2;
    char *u = malloc(n);
    if (!u) return NULL;
    if (base_slash && path_slash)        snprintf(u, n, "%s%s", base, path + 1);
    else if (!base_slash && !path_slash) snprintf(u, n, "%s/%s", base, path);
    else                                 snprintf(u, n, "%s%s", base, path);
    return u;
}

// ============================================================================
// vms list
// ============================================================================

/* Defined further down with the capabilities parser; declared here because
 * `parse_vm` needs it and moving it would separate it from its own comment. */
static char *json_array_join(json_t *arr);

void vminfo_free(VmInfo *v) {
    if (!v) return;
    free(v->id); free(v->alias); free(v->name); free(v->state); free(v->raw_json);
    /* VMK1 - the five keys the parser gained. */
    free(v->hwconfig); free(v->datacenter); free(v->provider); free(v->tags);
    free(v->speedtest_url);
    memset(v, 0, sizeof(*v));
}

void vmpage_free(VmPage *p) {
    if (!p) return;
    for (size_t i = 0; i < p->count; i++) vminfo_free(&p->items[i]);
    free(p->items);
    memset(p, 0, sizeof(*p));
}

static void parse_vm(json_t *jvm, VmInfo *out) {
    memset(out, 0, sizeof(*out));
    out->id    = jstrdup(jvm, "id");
    out->alias = jstrdup(jvm, "alias");
    out->name  = jstrdup(jvm, "name");

    /* VMK1 - kept as a field of its own as well as used as the fallback. It
     * was only ever the fallback, so a machine WITH a name could never show
     * its hardware tier - which is the more interesting of the two when an
     * account has several. */
    out->hwconfig   = jstrdup(jvm, "hwconfig");
    /* VMK1 - `datacenter` is an OBJECT, not a string: {name, timezone,
     * messaging_url, signaling_url, speedtest_url, turn_servers, ...}. The
     * first version read it with `jstrdup` and silently got NULL, so the
     * cards showed nothing and the bug looked like "the server does not send
     * it". A plain string is still accepted, because an API that nests a
     * field today may flatten it tomorrow. */
    {
        json_t *dc = json_object_get(jvm, "datacenter");
        if (dc && json_is_string(dc)) out->datacenter = strdup(json_string_value(dc));
        else if (dc && json_is_object(dc)) {
            out->datacenter    = jstrdup(dc, "name");
            out->speedtest_url = jstrdup(dc, "speedtest_url");
        }
    }
    out->provider   = jstrdup(jvm, "provider");
    out->tags       = json_array_join(json_object_get(jvm, "tags"));
    {
        json_t *m = json_object_get(jvm, "maintenance");
        /* The server has sent this as a bool and as an object with a flag
         * inside; both are honoured, and anything else reads as false. A
         * maintenance flag wrongly ON would hide a usable machine, so the
         * doubt resolves toward "available". */
        if (m && json_is_true(m)) out->maintenance = true;
        else if (m && json_is_object(m)) {
            json_t *en = json_object_get(m, "enabled");
            if (!en) en = json_object_get(m, "active");
            out->maintenance = (en && json_is_true(en));
        }
    }

    if (!out->name || out->name[0] == '\0') {
        free(out->name);
        out->name = out->hwconfig ? strdup(out->hwconfig) : NULL;
    }
    json_t *jstate = json_object_get(jvm, "state");
    if (jstate && json_is_string(jstate)) {
        out->state = strdup(json_string_value(jstate));
    } else {
        json_t *jstatus = json_object_get(jvm, "status");
        if (jstatus && json_is_string(jstatus)) {
            out->state = strdup(json_string_value(jstatus));
        } else if (jstatus && json_is_object(jstatus)) {
            out->state = jstrdup(jstatus, "state");
        }
        /* No recognised state field. We leave NULL rather than invent a text:
         * "(no state)" used to surface as is in the machine list, in English and
         * meaningless to the user. It is up to the UI to decide what to show.
         *
         * We log the available keys once: the field may exist under another
         * name, and this trace is what will let us find it without having to
         * recapture a session. */
        if (!out->state) {
            static int logged = 0;
            if (!logged) {
                logged = 1;
                const char *key;
                json_t *val;
                char keys[512] = {0};
                json_object_foreach(jvm, key, val) {
                    if (strlen(keys) + strlen(key) + 2 >= sizeof(keys)) break;
                    if (keys[0]) strcat(keys, ", ");
                    strcat(keys, key);
                }
                /* UI12 2026-10-02 - SAY WHAT `status` IS, not just that it
                 * was not what we expected.
                 *
                 * The key list has always contained `status`, so this message
                 * read as a contradiction: the field is right there. It is the
                 * SHAPE that does not match - neither a string nor an object
                 * carrying `state` - and the message gave no way to tell which
                 * of the two branches above fell through, nor what to write
                 * instead. The type and the compact value settle it in one
                 * line. A machine's run state is not a credential; it is
                 * `started`, `stopped` or the like, which is exactly what the
                 * screen means to show. */
                json_t *jst = json_object_get(jvm, "status");
                const char *shape = !jst ? "absent"
                                  : json_is_string(jst)  ? "string"
                                  : json_is_object(jst)  ? "object"
                                  : json_is_integer(jst) ? "integer"
                                  : json_is_real(jst)    ? "real"
                                  : json_is_boolean(jst) ? "boolean"
                                  : json_is_array(jst)   ? "array"
                                  : json_is_null(jst)    ? "null" : "?";
                char *dump = jst ? json_dumps(jst, JSON_COMPACT | JSON_ENCODE_ANY)
                                 : NULL;
                /* UI12 - ANSWERED the same day it was instrumented: the server
                 * sends `"status": null`. There is no run state in this reply
                 * at all, so leaving `out->state` NULL is correct rather than a
                 * parse we got wrong, and the live state arrives on the SSE
                 * stream instead (`status_changed: started`) - which is what
                 * the connecting screen already follows.
                 *
                 * The line stays, at one per process, for two reasons: a
                 * `status` that one day becomes a string or an object is worth
                 * noticing, and the previous wording ("no VM state; keys seen:
                 * ... status ...") read as a contradiction - the field is right
                 * there in the list it prints. */
                if (json_is_null(jst)) {
                    fprintf(stderr,
                        "launcher: the server sends `status: null` - the VM list "
                        "carries no run state; it arrives on the SSE stream\n");
                } else {
                    fprintf(stderr,
                        "launcher: `status` is %s = %.200s, neither a string nor an "
                        "object carrying `state`, so the machine list shows no "
                        "state; keys seen: %s\n",
                        shape, dump ? dump : "(none)", keys);
                }
                free(dump);
            }
        }
    }
    char *dump = json_dumps(jvm, JSON_COMPACT);
    if (dump) out->raw_json = dump;
}

bool launcher_list_vms(const char *launcher_base, const char *bearer,
                        int offset, int limit, VmPage *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char path[64];
    snprintf(path, sizeof(path), "vms?offset=%d&limit=%d", offset, limit);
    char *url = make_url(launcher_base, path);
    if (!url) return false;

    http_response resp;
    bool t = http_get(url, bearer, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        if (resp.data) fprintf(stderr, "list_vms HTTP %ld body: %.300s\n", resp.status, resp.data);
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    // Confirmed Shadow schema:
// { "pagination": {total_count, limit, offset}, "entries": [...] }
    json_t *arr = NULL;
    if (json_is_array(root)) arr = root;
    if (!arr) arr = json_object_get(root, "entries");
    if (!arr) arr = json_object_get(root, "vms");
    if (!arr) arr = json_object_get(root, "items");
    if (!arr) arr = json_object_get(root, "data");
    if (!arr || !json_is_array(arr)) {
        json_decref(root);
        return false;
    }

    size_t n = json_array_size(arr);
    out->items = calloc(n > 0 ? n : 1, sizeof(VmInfo));
    if (!out->items) { json_decref(root); return false; }
    out->count = n;

    for (size_t i = 0; i < n; i++) {
        parse_vm(json_array_get(arr, i), &out->items[i]);
    }

    json_t *jpag = json_object_get(root, "pagination");
    if (jpag && json_is_object(jpag)) {
        json_t *jtotal = json_object_get(jpag, "total_count");
        if (jtotal && json_is_integer(jtotal)) out->total = (size_t)json_integer_value(jtotal);
        else out->total = n;
    } else {
        json_t *jtotal = json_object_get(root, "total");
        if (jtotal && json_is_integer(jtotal)) out->total = (size_t)json_integer_value(jtotal);
        else out->total = n;
    }
    out->offset = offset;
    out->limit = limit;

    json_decref(root);
    return true;
}

// ============================================================================
// single vm
// ============================================================================

bool launcher_get_vm(const char *launcher_base, const char *bearer,
                     const char *vm_id, VmInfo *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char path[256];
    snprintf(path, sizeof(path), "vms/%s", vm_id);
    char *url = make_url(launcher_base, path);
    if (!url) return false;

    http_response resp;
    bool t = http_get(url, bearer, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) { http_free(&resp); return false; }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    parse_vm(root, out);
    json_decref(root);
    return out->id != NULL;
}

// ============================================================================
// shadow/vm/start
// ============================================================================

// A small helper: POST with an extra X-Vm-Id.
// libcurl supports it through a custom header - we extend http_post_json inline
// here to keep http.c minimal.
#include <curl/curl.h>

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

// Debug callback - logs the sent headers to a debug file
static FILE *g_curl_debug_file = NULL;
static int curl_debug_cb(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    (void)handle; (void)userptr;
    if (!g_curl_debug_file) return 0;
    const char *prefix = "?";
    switch (type) {
        case CURLINFO_TEXT: prefix = "[curl] "; break;
        case CURLINFO_HEADER_OUT: prefix = "[>>] "; break;
        case CURLINFO_HEADER_IN: prefix = "[<<] "; break;
        case CURLINFO_DATA_OUT: prefix = "[> body] "; break;
        case CURLINFO_DATA_IN: return 0; // skip body
        default: return 0;
    }
    fputs(prefix, g_curl_debug_file);
    fwrite(data, 1, size, g_curl_debug_file);
    fputc('\n', g_curl_debug_file);
    fflush(g_curl_debug_file);
    return 0;
}

static void enable_curl_debug(CURL *h) {
    if (!g_curl_debug_file) {
        g_curl_debug_file = fopen(SHADOW_DATA_DIR "curl_trace.log", "a");
    }
    if (g_curl_debug_file) {
        curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(h, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
    }
}

// Headers common to the shadow/* routes - without them Shadow's CDN/Envoy proxy
// silently strips X-Vm-Id and the server answers
//   "X-Vm-Id header or vmID path parameter is missing".
// Confirmed by the HAR: the web client always sends Origin/Referer/X-Shadow-*.
static struct curl_slist *append_shadow_headers(struct curl_slist *headers, const char *vm_id) {
    char xvm[512];      snprintf(xvm,    sizeof(xvm),    "X-Vm-Id: %s",       vm_id);             headers = curl_slist_append(headers, xvm);
    char xuuid[128];    snprintf(xuuid,  sizeof(xuuid),  "X-Shadow-Uuid: %s", shadow_device_uuid()); headers = curl_slist_append(headers, xuuid);
    headers = curl_slist_append(headers, "X-Shadow-Agent: " SHADOW_X_AGENT);
    headers = curl_slist_append(headers, "Origin: " SHADOW_ORIGIN);
    headers = curl_slist_append(headers, "Referer: " SHADOW_REFERER);
    return headers;
}

/* DIAG1: the last path segment of a URL, for a log label. Both helpers below
 * are shared by half a dozen endpoints, so naming the HELPER in the failure
 * line - "get_with_vmid FAILED" - would say nothing about WHICH call died;
 * "capabilities FAILED" is what a reader needs. Static buffer, single-threaded
 * use at one call per perform, and the pointer is consumed immediately. */
static const char *url_tail(const char *url)
{
    if (!url) return "request";
    const char *last = url;
    for (const char *p = url; *p; p++)
        if (*p == '/' && p[1]) last = p + 1;
    return last;
}

static bool post_with_vmid(const char *url, const char *bearer, const char *vm_id,
                           const char *body, http_response *out) {
    out->data = NULL; out->len = 0; out->status = 0;
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    shadow_curl_apply_share(h);   /* DNS + session TLS partagees */
    if (!h) return false;
    struct curl_slist *headers = NULL;
    // A generous buffer: the Bearer JWT can be 2-3 KB
    char auth[4096]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer); headers = curl_slist_append(headers, auth);
    headers = append_shadow_headers(headers, vm_id);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    enable_curl_debug(h);

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
    shadow_curl_report(h, rc, url_tail(url));   /* DIAG1 */
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return rc == CURLE_OK;
}

static bool get_with_vmid(const char *url, const char *bearer, const char *vm_id, http_response *out) {
    out->data = NULL; out->len = 0; out->status = 0;
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    shadow_curl_apply_share(h);   /* DNS + session TLS partagees */
    if (!h) return false;
    struct curl_slist *headers = NULL;
    char auth[4096]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer); headers = curl_slist_append(headers, auth);
    headers = append_shadow_headers(headers, vm_id);
    headers = curl_slist_append(headers, "Accept: application/json");
    enable_curl_debug(h);

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
    shadow_curl_report(h, rc, url_tail(url));   /* DIAG1 */
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return rc == CURLE_OK;
}

bool launcher_start_vm(const char *launcher_base, const char *bearer,
                       const char *vm_id, long *http_status) {
    if (http_status) *http_status = 0;
    // The official Java route - we rely on the X-Vm-Id header (to be debugged)
    char *url = make_url(launcher_base, "shadow/vm/start");
    if (!url) return false;

    http_response resp;
    bool t = post_with_vmid(url, bearer, vm_id, NULL, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    bool ok = t && resp.status >= 200 && resp.status < 300;
    if (!ok && resp.data) fprintf(stderr, "start_vm HTTP %ld body: %.300s\n", resp.status, resp.data);
    http_free(&resp);
    return ok;
}

void vmconn_free(VmConnectionInfo *c) {
    if (!c) return;
    free(c->ip); free(c->port); free(c->vm_session_id); free(c->messaging_url);
    free(c->proximus_url); free(c->alias); free(c->provider);
    memset(c, 0, sizeof(*c));
}

/* Decodes JWT.instance - delegated to the parser tested in
 * streaming/smoke_test.c. */
/* LIB2 2026-10-02 - was `extern int jwt_instance(...)`, a declaration with no
 * definition anywhere in this library: the symbol lived in `smoke_test.c`.
 * It is a pure header-only utility now, and a sibling. */
int launcher_jwt_instance(const char *jwt) {
    return jwt_instance(jwt);
}

/* Rewrites the last numeric segment of the URL `*url` (= `/N` or `/N/...`) with
 * `/<inst>`. Re-malloc'd. A no-op when the segment cannot be found. */
void launcher_rewrite_url_instance(char **url, int inst) {
    if (!url || !*url || inst <= 0) return;
    char *u = *url;
    /* Find the second-to-last `/` after the scheme. */
    char *slash_search = strstr(u, "://");
    if (!slash_search) return;
    slash_search += 3;
    /* Skip the host. Find the first `/` after the host. */
    char *seg_start = strchr(slash_search, '/');
    if (!seg_start) return;
    seg_start++;
    /* Check that the segment starts with a digit. */
    if (!isdigit((unsigned char)*seg_start)) return;
    /* Find the end of the numeric segment. */
    char *seg_end = seg_start;
    while (*seg_end && isdigit((unsigned char)*seg_end)) seg_end++;
    /* Reallocate with the new inst. */
    size_t prefix = (size_t)(seg_start - u);
    size_t suffix_len = strlen(seg_end);
    char inst_str[12];
    int inst_len = snprintf(inst_str, sizeof(inst_str), "%d", inst);
    size_t new_len = prefix + (size_t)inst_len + suffix_len + 1;
    char *nu = malloc(new_len);
    if (!nu) return;
    memcpy(nu, u, prefix);
    memcpy(nu + prefix, inst_str, (size_t)inst_len);
    memcpy(nu + prefix + inst_len, seg_end, suffix_len + 1);
    free(u);
    *url = nu;
}

bool launcher_get_vm_ip(const char *launcher_base, const char *bearer,
                         const char *vm_id, VmConnectionInfo *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;
    char *url = make_url(launcher_base, "shadow/vm/ip");
    if (!url) return false;

    http_response resp;
    bool t = get_with_vmid(url, bearer, vm_id, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    // Always dumped for RE.
    if (resp.data) {
        FILE *fdump = fopen(SHADOW_DATA_DIR "last_vm_ip_response.txt", "w");
        if (fdump) { fwrite(resp.data, 1, resp.len, fdump); fclose(fdump); }
    }
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    // Schema confirmed by the official web client's HAR (snake_case):
    // {ip, port, offset, slot_number, alias, proximus_url, messaging_url, provider, vm_session_id}
    out->ip            = jstrdup(root, "ip");
    out->port          = jstrdup(root, "port");
    out->vm_session_id = jstrdup(root, "vm_session_id");
    out->messaging_url = jstrdup(root, "messaging_url");
    out->proximus_url  = jstrdup(root, "proximus_url");
    out->alias         = jstrdup(root, "alias");
    out->provider      = jstrdup(root, "provider");
    json_t *jslot = json_object_get(root, "slot_number");
    out->slot_number = (jslot && json_is_integer(jslot)) ? (int)json_integer_value(jslot) : -1;

    json_decref(root);

    /* === VMIP-1 2026-09-26 - THE SERVER STOPPED SENDING THE URLS ===========
     *
     * Observed the same day on PS Vita AND Linux, two TLS stacks, same build
     * and the build before it: `/vm/ip` answers `{ip, port, slot_number,
     * alias}` only - no `proximus_url`, `messaging_url`, `provider`,
     * `vm_session_id`. The connection then stopped at step 3/7 with "Service
     * address missing". A server-side change, not ours.
     *
     * The HAR's form was `proximus_url = https://<host>/<slot>` and
     * `messaging_url = .../<slot>/stream`, and this code already talks to the
     * VmProxy at `https://<ip>/<instance>/` (ctrl_rest.c). So when the fields
     * are absent they are rebuilt from `ip` and `slot_number`; the caller then
     * swaps the slot for JWT.instance exactly as before. Logged, because a
     * rebuilt URL is an assumption until a session proves it.
     * `SHADOW_VMIP_REBUILD_URL=0` restores the old refusal. */
    {
        static int rebuild = -1;
        if (rebuild < 0) {
            const char *e = getenv("SHADOW_VMIP_REBUILD_URL");
            rebuild = e ? atoi(e) : 1;
        }
        if (rebuild && !out->proximus_url && out->ip && out->slot_number > 0) {
            char buf[512];
            snprintf(buf, sizeof buf, "https://%s/%d", out->ip, out->slot_number);
            out->proximus_url = strdup(buf);
            if (!out->messaging_url) {
                snprintf(buf, sizeof buf, "https://%s/%d/stream", out->ip, out->slot_number);
                out->messaging_url = strdup(buf);
            }
            JOURNAL_INFO_(JOURNAL_CAT_NETWORK,
                          "[VMIP-1] /vm/ip carries no proximus_url: rebuilt from "
                          "ip + slot_number (slot %d)", out->slot_number);
        }
    }

    /* Override the URLs' `/N/` segment with JWT.instance.
     *
     * RE 2026-05-09 (LD_PRELOAD hook on the official desktop app): the server
     * returns `proximus_url=".../<slot_number>"` (= /3 for slot=3, sometimes
     * stale or sticky), BUT the official desktop app always uses
     * `/<jwt.instance>/` (= /6 on the tested user). The mismatch means /3/clients
     * probably answers OK but does not bind the streaming ports, whereas
     * /6/clients triggers the binding. */
    /* Note: the bearer here is the global OAuth access_token, not a VM session
     * JWT with an "instance" field. The /N/ rewrite with JWT.instance happens in
     * the caller (cf. connecting_activity.cpp) AFTER proximus_get_credentials,
     * which returns a main_jwt carrying the instance. */
    return out->ip != NULL && out->port != NULL;
}

void launcher_token_free(LauncherSessionToken *t) {
    if (!t) return;
    free(t->token); t->token = NULL;
}

// Exposed for proximus.c, which needs it (same API host, same Shadow-* header
// constraints).
const char *shadow_device_uuid_public(void) { return shadow_device_uuid(); }

void proximus_credentials_free(ProximusCredentials *c) {
    if (!c) return;
    free(c->launcher_jwt); free(c->main_jwt); free(c->usb_jwt);
    memset(c, 0, sizeof(*c));
}

bool launcher_proximus_credentials(const char *launcher_base, const char *bearer,
                                   const char *vm_id, ProximusCredentials *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;
    char *url = make_url(launcher_base, "shadow/vm/proximus-credentials");
    if (!url) return false;

    // Body aligned exactly on the browser's (HAR 2026-05-06):
    //   [{"client_type":"launcher"},{"client_type":"main"}]
    // Variants tried before and abandoned:
    //  - `ports:[...]` on main to force direct TCP mode -> ignored server-side
    //    for legacy accounts (memory `Shadow JWT.ports pivot`); at worst it can
    //    signal "non-conforming client" and trigger QoS throttling.
    //  - a `usb` entry -> the browser never asks for one. The gamepad tunnel goes
    //    through the separate shadowusb daemon (memory `Shadow USB-over-WSS`), not
    //    through this credentials call. usb_jwt will stay NULL -> connecting_activity
    //    skip proximus_create_usb_client (cf. cpp:312 graceful fallback).
    const char *body =
        "[{\"client_type\":\"launcher\"},{\"client_type\":\"main\"}]";
    http_response resp;
    bool t = post_with_vmid(url, bearer, vm_id, body, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        if (resp.data) fprintf(stderr, "proximus_credentials HTTP %ld body: %.300s\n", resp.status, resp.data);
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root || !json_is_array(root)) { if (root) json_decref(root); return false; }

    size_t n = json_array_size(root);
    for (size_t i = 0; i < n; i++) {
        json_t *item = json_array_get(root, i);
        if (!json_is_object(item)) continue;
        // The server may return client_type or clientType depending on the body sent
        const char *type = json_string_value(json_object_get(item, "client_type"));
        if (!type) type = json_string_value(json_object_get(item, "clientType"));
        char *tok = jstrdup(item, "token");
        if (!type || !tok) { free(tok); continue; }
        if      (strcmp(type, "launcher") == 0) { free(out->launcher_jwt); out->launcher_jwt = tok; }
        else if (strcmp(type, "main")     == 0) { free(out->main_jwt);     out->main_jwt     = tok; }
        else if (strcmp(type, "usb")      == 0) { free(out->usb_jwt);      out->usb_jwt      = tok; }
        else                                     { free(tok); }
    }
    json_decref(root);
    return out->launcher_jwt != NULL && out->main_jwt != NULL;
}

bool launcher_auth_login(const char *launcher_base, const char *bearer,
                         const char *vm_id, LauncherSessionToken *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;
    char *url = make_url(launcher_base, "shadow/auth_login");
    if (!url) return false;

    // Body confirmed by the web client's HAR: {"cbp_token":"<access_token>"}
    // The server compares it against the header's Bearer - otherwise a 401.
    json_t *jb = json_object();
    json_object_set_new(jb, "cbp_token", json_string(bearer));
    char *body = json_dumps(jb, JSON_COMPACT);
    json_decref(jb);
    if (!body) { free(url); return false; }

    http_response resp;
    bool t = post_with_vmid(url, bearer, vm_id, body, &resp);
    free(body);
    free(url);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        if (resp.data) fprintf(stderr, "auth_login HTTP %ld body: %.300s\n", resp.status, resp.data);
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    out->token = jstrdup(root, "token");
    json_decref(root);
    return out->token != NULL;
}

// ============================================================================
// shadow GET /v3/vms/{vm_id}/capabilities
// ============================================================================

void vmcaps_free(VmCapabilities *c) {
    if (!c) return;
    free(c->raw_json); free(c->video_codecs); free(c->video_chroma);
    free(c->audio_codecs);
    free(c->usage.fair_use_renew_date); free(c->usage.end_of_streaming_session);
    free(c->usage.time_slots_timezone);
    memset(c, 0, sizeof(*c));
}

// Joins a json_array of strings into "a,b,c". The caller frees it.
static char *json_array_join(json_t *arr) {
    if (!arr || !json_is_array(arr)) return NULL;
    size_t total = 1;
    size_t n = json_array_size(arr);
    for (size_t i = 0; i < n; i++) {
        json_t *it = json_array_get(arr, i);
        if (json_is_string(it)) total += strlen(json_string_value(it)) + 1;
    }
    char *s = (char *)malloc(total);
    if (!s) return NULL;
    s[0] = 0;
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        json_t *it = json_array_get(arr, i);
        if (!json_is_string(it)) continue;
        const char *v = json_string_value(it);
        size_t vl = strlen(v);
        if (pos > 0) { s[pos++] = ','; }
        memcpy(s + pos, v, vl); pos += vl;
        s[pos] = 0;
    }
    return s;
}

/* === CAPS2 2026-10-03 - THE PARSER, SEPARATE FROM THE FETCH ===============
 *
 * Pulled out of `launcher_get_capabilities` so the offline suite can reach
 * it. Everything interesting about this reply is in the SHAPE of the JSON -
 * a `usage` block, per-channel permissions, a monitor count three levels
 * down - and until this was separable the only way to exercise it was to
 * have an account, a network and a machine.
 *
 * `body` is the raw reply. `out` must be zeroed by the caller; this function
 * fills what it finds and leaves the rest alone, so a server that stops
 * sending a key leaves a zero rather than a stale value.
 *
 * Returns false only when the body is not JSON at all. A body that parses but
 * carries nothing we know is a SUCCESS with an empty struct: that is a server
 * that changed, not a client that failed, and the difference matters to the
 * caller deciding whether to retry.
 */
bool launcher_parse_capabilities(const char *body, VmCapabilities *out) {
    if (!body || !out) return false;
    json_error_t err;
    json_t *root = json_loads(body, 0, &err);
    if (!root) return false;

    json_t *streaming = json_object_get(root, "streaming");
    if (streaming) {
        json_t *channels = json_object_get(streaming, "channels");
        if (channels) {
            json_t *video = json_object_get(channels, "video");
            if (video) {
                json_t *allowed = json_object_get(video, "allowed");
                out->video_allowed = (allowed && json_is_true(allowed));
                json_t *fr = json_object_get(video, "frame_rate");
                if (fr && json_is_integer(fr)) out->max_frame_rate = (int)json_integer_value(fr);
                json_t *res = json_object_get(video, "max_resolution");
                if (res) {
                    json_t *w = json_object_get(res, "width");
                    json_t *h = json_object_get(res, "height");
                    if (w && json_is_integer(w)) out->max_width = (int)json_integer_value(w);
                    if (h && json_is_integer(h)) out->max_height = (int)json_integer_value(h);
                }
                out->video_codecs = json_array_join(json_object_get(video, "codec"));
                /* CAPS2 - the three keys this parser used to walk past. */
                out->video_chroma = json_array_join(json_object_get(video, "chroma"));
                json_t *mc = json_object_get(video, "max_monitor_count");
                if (mc && json_is_integer(mc))
                    out->max_monitor_count = (int)json_integer_value(mc);
            }
            json_t *audio = json_object_get(channels, "audio");
            if (audio) {
                json_t *allowed = json_object_get(audio, "allowed");
                out->audio_allowed = (allowed && json_is_true(allowed));
                out->audio_codecs = json_array_join(json_object_get(audio, "codec"));
            }
            /* CAPS2 - which channels the ACCOUNT may open at all. Distinct
             * from whether the VM grants them later: a channel refused here
             * will never be announced, and a client that knows can grey the
             * menu entry instead of letting someone discover it by failing. */
            #define CHAN_ALLOWED(field, key)                                                   do {                                                                               json_t *ch = json_object_get(channels, key);                                   if (ch) {                                                                          json_t *al = json_object_get(ch, "allowed");                                   out->field = (al && json_is_true(al));                                     }                                                                          } while (0)
            CHAN_ALLOWED(micro_allowed,        "micro");
            CHAN_ALLOWED(clipboard_allowed,    "clipboard");
            CHAN_ALLOWED(filetransfer_allowed, "filetransfer");
            CHAN_ALLOWED(gamepad_allowed,      "gamepad");
            #undef CHAN_ALLOWED
        }
    }

    /* === CAPS2 - the usage block, which is the session countdown ==========
     *
     * `max_session_length` is the per-session ceiling the official client
     * counts down from ("5h55m restantes" at five minutes into a 21600 s
     * allowance). The server pushes no remaining time at any point, so this
     * is the only place the figure can come from. */
    json_t *usage = json_object_get(root, "usage");
    if (usage) {
        json_t *v;
        if ((v = json_object_get(usage, "max_session_length")) && json_is_integer(v))
            out->usage.max_session_length = (int)json_integer_value(v);
        if ((v = json_object_get(usage, "max_duration")) && json_is_integer(v))
            out->usage.max_duration = (int)json_integer_value(v);
        if ((v = json_object_get(usage, "fair_use_usage")) && json_is_integer(v))
            out->usage.fair_use_usage = (int)json_integer_value(v);
        if ((v = json_object_get(usage, "fair_use_alert_threshold")) && json_is_number(v))
            out->usage.fair_use_alert_threshold = json_number_value(v);
        if ((v = json_object_get(usage, "fair_use_renew_date")) && json_is_string(v))
            out->usage.fair_use_renew_date = strdup(json_string_value(v));
        /* Null in every sample so far; kept because a hard stop is exactly
         * the kind of field that is null until the day it is not. */
        if ((v = json_object_get(usage, "end_of_streaming_session")) && json_is_string(v))
            out->usage.end_of_streaming_session = strdup(json_string_value(v));
        if ((v = json_object_get(usage, "time_slots_enabled")))
            out->usage.time_slots_enabled = json_is_true(v);
        if ((v = json_object_get(usage, "time_slots_timezone")) && json_is_string(v))
            out->usage.time_slots_timezone = strdup(json_string_value(v));
    }

    json_decref(root);
    return true;
}

bool launcher_get_capabilities(const char *launcher_base, const char *bearer,
                                const char *vm_id, VmCapabilities *out,
                                long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    // The endpoint is under /v3/vms/{id}/capabilities - launcher_base already
    // includes ".../launcher-api/v3/", so we assemble "vms/{id}/capabilities".
    size_t pn = strlen("vms/") + strlen(vm_id) + strlen("/capabilities") + 1;
    char *path = (char *)malloc(pn);
    if (!path) return false;
    snprintf(path, pn, "vms/%s/capabilities", vm_id);
    char *url = make_url(launcher_base, path);
    free(path);
    if (!url) return false;

    http_response resp;
    bool t = get_with_vmid(url, bearer, vm_id, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        if (resp.data) fprintf(stderr, "capabilities HTTP %ld body: %.300s\n",
                                resp.status, resp.data);
        http_free(&resp);
        return false;
    }

    out->raw_json = strndup(resp.data, resp.len);

    /* === DISP1 2026-10-03 - PRINT THE BODY WE ALREADY HAVE =================
     *
     * The parser below takes four values out of this reply and the rest is
     * kept in `raw_json` and never looked at. The RE of ShadowStreamer 6.3.1
     * says the server enforces a per-client MAXIMUM NUMBER OF DISPLAYS
     * ("VM does not support more than %d display(s)") from a limit block that
     * also holds the resolution and frame-rate ceilings - and those two
     * demonstrably come from this endpoint. So the display count plausibly
     * arrives here too, in a key nobody has read.
     *
     * Guessing the key name and parsing it would be writing code against a
     * hypothesis. Printing the body is one line and answers it.
     *
     * Off by default and not merely quiet: this is a whole HTTP body in the
     * log, and a body is exactly the kind of thing that turns out to hold an
     * identifier when someone attaches the log to an issue. */
    if (getenv("SHADOW_LOG_CAPS_JSON")) {
        JOURNAL_INFO_(JOURNAL_CAT_NETWORK,
                      "[DISP1] /vms/*/capabilities body: %.1500s", out->raw_json);
    }

    /* Parsed from OUR copy, so the response can be released first: the
     * original freed `resp` between `json_loads` and the walk, and pulling
     * the parser out would otherwise have left that free with nowhere to go
     * (it was briefly dropped, which is a leak on every call). */
    http_free(&resp);
    if (!launcher_parse_capabilities(out->raw_json, out))
        return out->raw_json != NULL;
    return true;
}

// ============================================================================
// shadow GET /v3/shadow/turn-servers
// ============================================================================

void turn_servers_free(TurnServers *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) {
        free(t->items[i].url); free(t->items[i].host);
        free(t->items[i].transport); free(t->items[i].username);
        free(t->items[i].credential);
    }
    free(t->items);
    memset(t, 0, sizeof(*t));
}

// Parse "turn:host:port?transport=tcp" → host/port/transport.
static void parse_turn_url(const char *url, char **host, int *port, char **transport) {
    *host = NULL; *port = 0; *transport = NULL;
    if (!url) return;
    const char *p = url;
    /* skip scheme: turn: or turns: */
    if (strncmp(p, "turns:", 6) == 0) p += 6;
    else if (strncmp(p, "turn:", 5) == 0) p += 5;
    /* host until ':' or '?' */
    const char *colon = strchr(p, ':');
    const char *qmark = strchr(p, '?');
    const char *host_end = colon ? colon : (qmark ? qmark : p + strlen(p));
    *host = strndup(p, (size_t)(host_end - p));
    if (colon) {
        *port = (int)strtol(colon + 1, NULL, 10);
    }
    if (qmark) {
        const char *q = qmark + 1;
        const char *eq = strchr(q, '=');
        if (eq && strncmp(q, "transport", (size_t)(eq - q)) == 0) {
            const char *amp = strchr(eq + 1, '&');
            const char *te = amp ? amp : (eq + 1 + strlen(eq + 1));
            *transport = strndup(eq + 1, (size_t)(te - (eq + 1)));
        }
    }
}

bool launcher_get_turn_servers(const char *launcher_base, const char *bearer,
                                const char *vm_id, TurnServers *out,
                                long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char *url = make_url(launcher_base, "shadow/turn-servers");
    if (!url) return false;
    http_response resp;
    bool t = get_with_vmid(url, bearer, vm_id, &resp);
    free(url);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        if (resp.data) fprintf(stderr, "turn-servers HTTP %ld body: %.300s\n",
                                resp.status, resp.data);
        http_free(&resp);
        return false;
    }
    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;
    json_t *arr = json_object_get(root, "iceServers");
    if (!arr || !json_is_array(arr)) { json_decref(root); return false; }

    size_t n = json_array_size(arr);
    out->items = (TurnServerEntry *)calloc(n, sizeof(TurnServerEntry));
    if (!out->items) { json_decref(root); return false; }
    for (size_t i = 0; i < n; i++) {
        json_t *it = json_array_get(arr, i);
        if (!json_is_object(it)) continue;
        TurnServerEntry *e = &out->items[out->count];
        json_t *urls = json_object_get(it, "urls");
        const char *url_str = NULL;
        if (json_is_string(urls)) url_str = json_string_value(urls);
        else if (json_is_array(urls) && json_array_size(urls) > 0) {
            json_t *u0 = json_array_get(urls, 0);
            if (json_is_string(u0)) url_str = json_string_value(u0);
        }
        if (!url_str) continue;
        e->url = strdup(url_str);
        parse_turn_url(url_str, &e->host, &e->port, &e->transport);
        e->username = jstrdup(it, "username");
        e->credential = jstrdup(it, "credential");
        out->count++;
    }
    json_decref(root);
    return out->count > 0;
}

// ============================================================================
// GET https://api.eu.shadow.tech/v1/subscription/status
// ============================================================================

void subscription_free(Subscription *s) {
    if (!s) return;
    free(s->id); free(s->plan_id); free(s->plan_short); free(s->status);
    free(s->last_payment_status); free(s->on_hold_reason); free(s->product_family);
    memset(s, 0, sizeof(*s));
}

// Derives a short readable name from plan_id.
// "cloudpc-b2c-power2023-EUR-Monthly" -> "Power 2023"
// "cloudpc-b2c-boost2023-EUR-Monthly" -> "Boost 2023"
// "cloudpc-b2c-ultra2023-EUR-Monthly" -> "Ultra 2023"
static char *derive_plan_short(const char *plan_id) {
    if (!plan_id) return strdup("Unknown");
    const char *tiers[] = {"power", "boost", "ultra", "starter"};
    const char *tier_caps[] = {"Power", "Boost", "Ultra", "Starter"};
    for (size_t i = 0; i < sizeof(tiers)/sizeof(tiers[0]); i++) {
        const char *p = strstr(plan_id, tiers[i]);
        if (p) {
            // Extract year suffix if present (4 digits after tier name)
            size_t tlen = strlen(tiers[i]);
            char year[8] = {0};
            if (isdigit((unsigned char)p[tlen])) {
                size_t y = 0;
                while (y < 4 && isdigit((unsigned char)p[tlen + y])) {
                    year[y] = p[tlen + y]; y++;
                }
                year[y] = 0;
            }
            char buf[64];
            if (year[0]) snprintf(buf, sizeof(buf), "%s %s", tier_caps[i], year);
            else         snprintf(buf, sizeof(buf), "%s", tier_caps[i]);
            return strdup(buf);
        }
    }
    return strdup(plan_id);
}

bool launcher_get_subscription_status(const char *bearer, Subscription *out,
                                       long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char url[256];
    snprintf(url, sizeof(url), "%ssubscription/status?product_family=cloudpc",
             SHADOW_API_V1);

    http_response resp = {0};
    bool t = http_get(url, bearer, &resp);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        if (resp.data) fprintf(stderr, "subscription HTTP %ld body: %.300s\n",
                                resp.status, resp.data);
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    out->id                 = jstrdup(root, "id");
    out->plan_id            = jstrdup(root, "plan_id");
    out->status             = jstrdup(root, "status");
    out->last_payment_status= jstrdup(root, "last_payment_status");
    out->on_hold_reason     = jstrdup(root, "on_hold_reason");
    out->product_family     = jstrdup(root, "product_family");
    json_t *oh = json_object_get(root, "on_hold");
    out->on_hold = (oh && json_is_true(oh));
    json_t *sa = json_object_get(root, "started_at");
    if (sa && json_is_integer(sa)) out->started_at = (long)json_integer_value(sa);
    json_t *pd = json_object_get(root, "last_payment_date");
    if (pd && json_is_integer(pd)) out->last_payment_date = (long)json_integer_value(pd);

    out->plan_short = derive_plan_short(out->plan_id);
    json_decref(root);
    return out->id != NULL || out->plan_id != NULL;
}

// ============================================================================
// GET https://api.eu.shadow.tech/v1/pu/shadow-drive/user/token
// ============================================================================

void drive_token_free(DriveToken *d) {
    if (!d) return;
    free(d->login_name); free(d->token); free(d->message);
    memset(d, 0, sizeof(*d));
}

bool launcher_get_drive_token(const char *bearer, DriveToken *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char url[256];
    snprintf(url, sizeof(url),
             "%spu/shadow-drive/user/token?token_name=shadow-web-launcher-token&force=1",
             SHADOW_API_V1);

    http_response resp = {0};
    bool t = http_get(url, bearer, &resp);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }
    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    /* The fields can be JSON null when the user has not enabled Shadow Drive -
     * we only copy them when they are non-null strings. */
    json_t *ln = json_object_get(root, "login_name");
    if (json_is_string(ln)) out->login_name = strdup(json_string_value(ln));
    json_t *tk = json_object_get(root, "token");
    if (json_is_string(tk)) out->token = strdup(json_string_value(tk));
    json_t *msg = json_object_get(root, "message");
    if (json_is_string(msg)) out->message = strdup(json_string_value(msg));
    json_decref(root);
    return true;
}
