#include "ctrl_rest.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../services/http.h"
#include "../common/log.h"

/* S81 - the category is DECLARED here, not guessed from the message text.
 * `rlog` stays at INFO, so the existing calls do not disappear. `rdbg` is
 * there for the high-volume lines, which move over to it one at a time. */
#define rlog(...) JOURNAL_INFO_(JOURNAL_CAT_NETWORK, __VA_ARGS__)
#define rdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_NETWORK, __VA_ARGS__)
/* Strip the "ipv6-" prefix from the hostname to get the dual-stack name
 * (our http.c forces CURL_IPRESOLVE_V4, so we need the hostname that has
 * A records). */
static void strip_ipv6_prefix(const char *src, char *dst, size_t cap) {
    if (!src || !dst || cap == 0) return;
    if (strncmp(src, "ipv6-", 5) == 0) {
        snprintf(dst, cap, "%s", src + 5);
    } else {
        snprintf(dst, cap, "%s", src);
    }
}

/* The JSON extractor now lives in shadow/http.c: it used to be copied
 * verbatim here and in the other REST caller. */


/* Same thing, for a numeric value (long). */
static bool json_extract_long(const char *json, const char *key, long *out) {
    if (!json || !key || !out) return false;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t')) p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

bool ctrl_rest_register_client(const char *vm_host, const char *bearer,
                                int client_type, const char *opaque_json,
                                shadow_session_creds *out_creds) {
    if (!vm_host || !bearer || !out_creds) return false;
    memset(out_creds, 0, sizeof(*out_creds));

    char host_v4[256];
    strip_ipv6_prefix(vm_host, host_v4, sizeof(host_v4));

    char url[512];
    snprintf(url, sizeof(url), "https://%s/3/clients", host_v4);

    /* Body: {"type":<int>,"opaque":"<escaped json>"} */
    char body[1024];
    if (opaque_json && *opaque_json) {
        /* No clever escaping here: the opaque strings only hold plain
         * characters. The caller is expected to have escaped its own inner
         * quotes as \". */
        snprintf(body, sizeof(body), "{\"type\":%d,\"opaque\":\"%s\"}",
                 client_type, opaque_json);
    } else {
        snprintf(body, sizeof(body), "{\"type\":%d}", client_type);
    }

    http_response resp = {0};
    bool ok = http_post_json(url, bearer, body, strlen(body), &resp);
    if (!ok || resp.status < 200 || resp.status >= 300) {
        rlog("ctrl_rest_register_client: status=%ld ok=%d", resp.status, (int)ok);
        if (resp.data && resp.len > 0) {
            size_t prev = resp.len > 400 ? 400 : resp.len;
            rlog("ctrl_rest_register_client: ERR body[0..%zu]=%.*s",
                 prev, (int)prev, resp.data);
        }
        http_free(&resp);
        return false;
    }
    rlog("ctrl_rest_register_client: status=%ld ok=%d body_len=%zu",
         resp.status, (int)ok, resp.len);
    if (resp.data && resp.len > 0) {
        size_t prev = resp.len > 400 ? 400 : resp.len;
        rlog("ctrl_rest_register_client: body[0..%zu]=%.*s",
             prev, (int)prev, resp.data);
    }

    /* Parse data.{id,streamingtoken,streamingtoken_expiry,remote} */
    bool got_id = http_json_get_string(resp.data, "id",
                                        out_creds->client_id, sizeof(out_creds->client_id));
    http_json_get_string(resp.data, "streamingtoken",
                         out_creds->streamingtoken, sizeof(out_creds->streamingtoken));
    json_extract_long(resp.data, "streamingtoken_expiry",
                       &out_creds->token_expiry_unix);
    http_json_get_string(resp.data, "remote",
                         out_creds->remote_ip, sizeof(out_creds->remote_ip));

    http_free(&resp);

    if (!got_id) {
        rlog("ctrl_rest_register_client: no client id in reply");
        return false;
    }
    rlog("ctrl_rest_register_client: id=%s tok=%.20s... expiry=%ld",
         out_creds->client_id, out_creds->streamingtoken,
         out_creds->token_expiry_unix);
    return true;
}

bool ctrl_rest_forward(const char *vm_host, const char *bearer,
                        const char *command_data_json,
                        char **out_resp, size_t *out_len) {
    if (!vm_host || !bearer || !command_data_json) return false;

    char host_v4[256];
    strip_ipv6_prefix(vm_host, host_v4, sizeof(host_v4));

    char url[512];
    snprintf(url, sizeof(url), "https://%s/3/forward", host_v4);

    char body[2048];
    snprintf(body, sizeof(body),
             "{\"command\":\"forward\",\"data\":%s}", command_data_json);

    http_response resp = {0};
    bool ok = http_post_json(url, bearer, body, strlen(body), &resp);
    if (!ok || resp.status < 200 || resp.status >= 300) {
        rlog("ctrl_rest_forward: status=%ld ok=%d", resp.status, (int)ok);
        http_free(&resp);
        if (out_resp) *out_resp = NULL;
        if (out_len)  *out_len = 0;
        return false;
    }

    if (out_resp) {
        /* Transfer ownership of resp.data to the caller */
        *out_resp = resp.data;
        if (out_len) *out_len = resp.len;
        resp.data = NULL;
    }
    http_free(&resp);
    return true;
}

bool ctrl_rest_unregister(const char *vm_host, const char *bearer,
                           const char *client_id) {
    if (!vm_host || !bearer || !client_id) return false;

    char host_v4[256];
    strip_ipv6_prefix(vm_host, host_v4, sizeof(host_v4));

    char url[640];
    snprintf(url, sizeof(url), "https://%s/3/clients/%s", host_v4, client_id);

    /* libcurl DELETE - http.c carries no http_delete helper, so this one
     * goes straight through curl_easy. */
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    if (!h) return false;

    char auth[1024];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
    /* Discard response body */
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, NULL);

    CURLcode rc = curl_easy_perform(h);
    shadow_curl_report(h, rc, "ctrl_rest_unregister");   /* DIAG1 */
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);

    bool ok = (rc == CURLE_OK) && (status >= 200 && status < 300);
    rlog("ctrl_rest_unregister: status=%ld ok=%d", status, (int)ok);
    return ok;
}

/* Helper: DELETE /<instance>/clients/<id>, with a dynamic instance number. */
static bool delete_client_by_id(const char *host_v4, const char *bearer,
                                  int instance, const char *client_id) {
    char url[640];
    snprintf(url, sizeof(url), "https://%s/%d/clients/%s", host_v4, instance, client_id);
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    if (!h) return false;
    char auth[1024];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, NULL);
    CURLcode rc = curl_easy_perform(h);
    shadow_curl_report(h, rc, "delete_client_by_id");   /* DIAG1 */
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    bool ok = (rc == CURLE_OK) && (status >= 200 && status < 300);
    rlog("clean_my_zombies: DELETE /%d/clients/%s status=%ld ok=%d",
         instance, client_id, status, (int)ok);
    return ok;
}

int ctrl_rest_clean_my_zombies(const char *vm_host, const char *bearer,
                                  int instance, const char *device_uuid) {
    if (!vm_host || !bearer || !device_uuid || instance <= 0) return -1;

    char host_v4[256];
    strip_ipv6_prefix(vm_host, host_v4, sizeof(host_v4));

    /* Step 1: GET /<instance>/clients */
    char url[640];
    snprintf(url, sizeof(url), "https://%s/%d/clients", host_v4, instance);

    http_response resp = {0};
    bool got = http_get(url, bearer, &resp);
    if (!got || resp.status < 200 || resp.status >= 300 || !resp.data) {
        rlog("clean_my_zombies: GET /%d/clients FAIL status=%ld", instance, resp.status);
        http_free(&resp);
        return -1;
    }
    /* Step 2: parse the data array, find the clients carrying our device-id
     * in their opaque field, and DELETE each of them. The JSON reads
     * `{"data": [{"id": "...", "opaque": "..."},...]}`, and opaque is an
     * escaped string containing `\"device-id\":\"<uuid>\"`. */
    int deleted = 0;
    char needle[80];
    snprintf(needle, sizeof(needle), "\\\"device-id\\\":\\\"%s\\\"", device_uuid);

    /* Walk the `"id": "..."` occurrences in the body and, for each one, check
     * that the sub-object contains our needle. Crude, but sound here: the
     * opaque fields are inline JSON-encoded (escaped quotes), never nested
     * objects. */
    const char *p = resp.data;
    while ((p = strstr(p, "\"id\": \"")) != NULL) {
        p += 7;
        const char *end = strchr(p, '"');
        if (!end || end - p > 200) break;
        char client_id[256];
        size_t idl = (size_t)(end - p);
        if (idl >= sizeof(client_id)) { p = end + 1; continue; }
        memcpy(client_id, p, idl);
        client_id[idl] = '\0';
        /* Find the matching opaque field: search forward to the next `"id":`
         * or to the end of the body */
        const char *next_id = strstr(end, "\"id\": \"");
        const char *region_end = next_id ? next_id : (resp.data + resp.len);
        char tmp[16384];
        size_t reg_len = (size_t)(region_end - end);
        if (reg_len >= sizeof(tmp)) reg_len = sizeof(tmp) - 1;
        memcpy(tmp, end, reg_len);
        tmp[reg_len] = '\0';
        if (strstr(tmp, needle)) {
            if (delete_client_by_id(host_v4, bearer, instance, client_id))
                deleted++;
        }
        p = end + 1;
    }
    http_free(&resp);
    rlog("clean_my_zombies: deleted=%d clients matching device-id=%s",
         deleted, device_uuid);
    return deleted;
}

int ctrl_rest_clean_all_clients(const char *vm_host, const char *bearer,
                                   int instance) {
    if (!vm_host || !bearer || instance <= 0) return -1;
    char host_v4[256];
    strip_ipv6_prefix(vm_host, host_v4, sizeof(host_v4));
    char url[640];
    snprintf(url, sizeof(url), "https://%s/%d/clients", host_v4, instance);
    http_response resp = {0};
    if (!http_get(url, bearer, &resp) || resp.status < 200 || resp.status >= 300
        || !resp.data) {
        rlog("clean_all_clients: GET /%d/clients FAIL status=%ld", instance, resp.status);
        http_free(&resp);
        return -1;
    }
    int deleted = 0;
    const char *p = resp.data;
    while ((p = strstr(p, "\"id\": \"")) != NULL) {
        p += 7;
        const char *end = strchr(p, '"');
        if (!end || end - p > 200) break;
        char client_id[256];
        size_t idl = (size_t)(end - p);
        if (idl >= sizeof(client_id)) { p = end + 1; continue; }
        memcpy(client_id, p, idl);
        client_id[idl] = '\0';
        if (delete_client_by_id(host_v4, bearer, instance, client_id))
            deleted++;
        p = end + 1;
    }
    http_free(&resp);
    rlog("clean_all_clients: deleted=%d total clients (force mode)", deleted);
    return deleted;
}
