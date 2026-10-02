#include "tinag.h"
#include "http.h"
#include "config.h"

#include <jansson.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gapinfo_free(GapInfo *g) {
    if (!g) return;
    free(g->gap_url);             g->gap_url = NULL;
    free(g->name);                g->name = NULL;
    free(g->speed_test_url);      g->speed_test_url = NULL;
    free(g->launcher_api_url);    g->launcher_api_url = NULL;
    free(g->launcher_api_version); g->launcher_api_version = NULL;
}

static char *dup_json_string(json_t *root, const char *key) {
    json_t *v = json_object_get(root, key);
    if (!v || !json_is_string(v)) return NULL;
    const char *s = json_string_value(v);
    return s ? strdup(s) : NULL;
}

bool tinag_get_datacenter(const char *email, GapInfo *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    // Build the URL with a URL-encoded email
    char *url = NULL;
    {
        CURL *escaper = curl_easy_init();
        shadow_curl_apply_ca(escaper);   /* WIN2 - see http.h */
        if (!escaper) return false;
        char *esc = email && *email ? curl_easy_escape(escaper, email, 0) : strdup("");
        size_t n = strlen(SHADOW_TINAG_BASE) + strlen("datacenter?email=") + (esc ? strlen(esc) : 0) + 1;
        url = malloc(n);
        if (url) snprintf(url, n, "%sdatacenter?email=%s", SHADOW_TINAG_BASE, esc ? esc : "");
        if (esc) {
            // curl_easy_escape returned via curl_free; strdup'ed empty is plain free
            if (email && *email) curl_free(esc); else free(esc);
        }
        curl_easy_cleanup(escaper);
        if (!url) return false;
    }

    http_response resp;
    bool transport_ok = http_get(url, NULL, &resp);
    free(url);
    if (http_status) *http_status = resp.status;

    if (!transport_ok || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_error_t jerr;
    json_t *root = json_loads(resp.data, 0, &jerr);
    http_free(&resp);
    if (!root) {
        fprintf(stderr, "tinag: JSON parse error: %s (line %d)\n", jerr.text, jerr.line);
        return false;
    }

    out->gap_url             = dup_json_string(root, "gapUrl");
    out->name                = dup_json_string(root, "name");
    out->speed_test_url      = dup_json_string(root, "speedTestUrl");
    out->launcher_api_url    = dup_json_string(root, "launcherApiUrl");
    out->launcher_api_version = dup_json_string(root, "launcherApiVersion");

    json_decref(root);

    if (!out->launcher_api_url) {
        gapinfo_free(out);
        return false;
    }
    return true;
}
