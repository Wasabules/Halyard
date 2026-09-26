#include "shadowusb.h"
#include "ctrl_tcp.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../services/http.h"
#include "../common/log.h"
#include "../services/log_mask.h"   /* SEC2: secrets in the log, start and end only */

/* S81 - the category is DECLARED here, not guessed from the message text.
 * `ulog` stays at INFO, so the existing calls do not disappear. `udbg` is
 * there for the high-volume lines, which move over to it one at a time. */
#define ulog(...) JOURNAL_INFO_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)
#define udbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)
/* Strip the "ipv6-" prefix, to get the dual-stack DNS name. */
static void strip_ipv6_prefix(const char *src, char *dst, size_t cap) {
    if (!src || !dst || cap == 0) return;
    if (strncmp(src, "ipv6-", 5) == 0) snprintf(dst, cap, "%s", src + 5);
    else                                 snprintf(dst, cap, "%s", src);
}

/* The JSON extractor now lives in shadow/http.c: it used to be copied
 * verbatim here and in the other REST caller. */


bool shadowusb_register_device(const char *vm_host, const char *usb_token,
                                int instance, const char *usb_client_id,
                                char *out_id, size_t out_id_cap,
                                char *out_spice_url, size_t out_url_cap,
                                char *out_spice_secret, size_t out_secret_cap) {
    if (!vm_host || !usb_token || !usb_client_id || !out_id || !out_spice_url || !out_spice_secret) return false;
    if (instance < 1 || instance > 99) return false;

    char host_v4[256];
    strip_ipv6_prefix(vm_host, host_v4, sizeof(host_v4));

    /* Ghidra RE: the Mac daemon POSTs the Spice body to "{}/{}/remote-consoles".
     * What fills the `{}/{}` depends on the caller, so we try the candidate
     * paths in turn. */
    const char *body = "{\"protocol\":\"spice\",\"type\":\"spice-html5\"}";
    const char *path_templates[] = {
        "https://%s/%d/clients/%s/remote-consoles",   /* /N/clients/<id>/remote-consoles */
        "https://%s/clients/%s/remote-consoles",       /* /clients/<id>/remote-consoles (no instance) */
        "https://%s/%d/devices/%s/remote-consoles",    /* sub-resource of devices */
        "https://%s/%d/remote-consoles",               /* simple /N/remote-consoles */
        "https://%s/%d/devices",                       /* legacy XHCI fallback */
        NULL
    };

    char url[1024];
    http_response resp = {0};
    long final_status = 0;
    bool ok = false;
    int tried = 0;

    for (int i = 0; path_templates[i]; i++) {
        if (strstr(path_templates[i], "%d/clients/%s/remote-consoles") != NULL) {
            snprintf(url, sizeof(url), path_templates[i], host_v4, instance, usb_client_id);
        } else if (strstr(path_templates[i], "/clients/%s/remote-consoles") != NULL) {
            snprintf(url, sizeof(url), path_templates[i], host_v4, usb_client_id);
        } else if (strstr(path_templates[i], "%d/devices/%s/remote-consoles") != NULL) {
            snprintf(url, sizeof(url), path_templates[i], host_v4, instance, usb_client_id);
        } else if (strstr(path_templates[i], "%d/remote-consoles") != NULL) {
            snprintf(url, sizeof(url), path_templates[i], host_v4, instance);
        } else {
            snprintf(url, sizeof(url), path_templates[i], host_v4, instance);
        }
        ulog("shadowusb: try[%d] %s", i, url);
        memset(&resp, 0, sizeof(resp));
        /* The legacy /devices endpoint wants XHCI instead. */
        const char *try_body = (strstr(url, "/devices") && !strstr(url, "remote-consoles"))
            ? "{\"protocol\":\"XHCI\"}" : body;
        ok = http_post_json(url, usb_token, try_body, strlen(try_body), &resp);
        final_status = resp.status;
        tried++;
        ulog("shadowusb: try[%d] status=%ld len=%zu", i, resp.status, resp.len);
        if (resp.data && resp.len > 0) {
            size_t prev = resp.len > 400 ? 400 : resp.len;
            ulog("shadowusb: try[%d] body=%.*s", i, (int)prev, resp.data);
        }
        if (ok && resp.status >= 200 && resp.status < 300) {
            ulog("shadowusb: PATH WORKS = %s", url);
            break;
        }
        http_free(&resp);
        memset(&resp, 0, sizeof(resp));
    }
    ulog("shadowusb_register_device: final status=%ld after %d tries", final_status, tried);
    /* Log the WHOLE response, to expose every field we do not know about. */
    if (resp.data && resp.len > 0) {
        size_t prev = resp.len > 1000 ? 1000 : resp.len;
        ulog("shadowusb: FULL_RESP[0..%zu]=%.*s", prev, (int)prev, resp.data);
    }
    if (!ok || resp.status < 200 || resp.status >= 300) {
        http_free(&resp);
        return false;
    }

    bool got_id = http_json_get_string(resp.data, "id", out_id, out_id_cap);
    /* Try every candidate field; whatever is absent simply stays empty. */
    bool got_url = http_json_get_string(resp.data, "spice_url", out_spice_url, out_url_cap);
    bool got_secret = http_json_get_string(resp.data, "spice_secret", out_spice_secret, out_secret_cap);
    /* Then try the other plausible names for the same values. */
    if (!got_url) got_url = http_json_get_string(resp.data, "wss_url", out_spice_url, out_url_cap);
    if (!got_url) got_url = http_json_get_string(resp.data, "url", out_spice_url, out_url_cap);
    if (!got_secret) got_secret = http_json_get_string(resp.data, "secret", out_spice_secret, out_secret_cap);
    if (!got_secret) got_secret = http_json_get_string(resp.data, "ticket", out_spice_secret, out_secret_cap);
    http_free(&resp);

    ulog("shadowusb: parsed id=%s url=%s secret_len=%zu",
         got_id ? out_id : "(none)",
         got_url ? out_spice_url : "(none)",
         got_secret ? strlen(out_spice_secret) : 0);
    return got_id;  /* On retourne true si on a au moins l'id. */
}

/* Cleanup DELETE for a remote-console created by /clients/<id>/remote-consoles. */
static bool shadowusb_delete_device(const char *vm_host, const char *usb_token,
                                      int instance, const char *usb_client_id,
                                      const char *console_id) {
    if (!vm_host || !usb_token || !usb_client_id || !console_id) return false;
    char host_v4[256];
    strip_ipv6_prefix(vm_host, host_v4, sizeof(host_v4));

    char url[1024];
    snprintf(url, sizeof(url),
             "https://%s/%d/clients/%s/remote-consoles/%s",
             host_v4, instance, usb_client_id, console_id);

    CURL *h = curl_easy_init();
    if (!h) return false;

    char auth[1024];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", usb_token);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth);

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, NULL);

    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);

    bool ok = (rc == CURLE_OK) && (status >= 200 && status < 300);
    ulog("shadowusb_delete_device: status=%ld ok=%d", status, (int)ok);
    return ok;
}

bool shadowusb_probe_remote_consoles(const char *vm_host, int instance,
                                       const char *launcher_id, const char *launcher_jwt,
                                       const char *main_id, const char *main_jwt,
                                       const char *usb_id, const char *usb_jwt) {
    ulog("=========================================================");
    ulog("SHADOWUSB PROBE remote-consoles : 3 client-types × 2 paths");
    ulog("=========================================================");
    char host_v4[256];
    strip_ipv6_prefix(vm_host, host_v4, sizeof(host_v4));
    const char *body = "{\"protocol\":\"spice\",\"type\":\"spice-html5\"}";

    struct {
        const char *label;
        const char *cid;
        const char *jwt;
    } combos[] = {
        {"launcher", launcher_id, launcher_jwt},
        {"main",     main_id,     main_jwt},
        {"usb",      usb_id,      usb_jwt},
    };
    const char *path_fmts[] = {
        "https://%s/%d/clients/%s/remote-consoles",
        "https://%s/clients/%s/remote-consoles",
    };

    for (size_t i = 0; i < sizeof(combos)/sizeof(combos[0]); i++) {
        if (!combos[i].cid || !combos[i].jwt) {
            ulog("probe: %s skip (missing cid or jwt)", combos[i].label);
            continue;
        }
        for (size_t p = 0; p < sizeof(path_fmts)/sizeof(path_fmts[0]); p++) {
            char url[1024];
            if (strstr(path_fmts[p], "%d/")) {
                snprintf(url, sizeof(url), path_fmts[p], host_v4, instance, combos[i].cid);
            } else {
                snprintf(url, sizeof(url), path_fmts[p], host_v4, combos[i].cid);
            }
            http_response resp = {0};
            ulog("probe[%s/p%zu]: %s", combos[i].label, p, url);
            bool ok = http_post_json(url, combos[i].jwt, body, strlen(body), &resp);
            ulog("probe[%s/p%zu]: status=%ld ok=%d len=%zu",
                 combos[i].label, p, resp.status, (int)ok, resp.len);
            if (resp.data && resp.len > 0) {
                size_t prev = resp.len > 400 ? 400 : resp.len;
                ulog("probe[%s/p%zu]: body=%.*s", combos[i].label, p, (int)prev, resp.data);
            }
            http_free(&resp);
            if (ok && resp.status >= 200 && resp.status < 300) {
                ulog("=== PROBE WIN : combo=%s path=p%zu ===", combos[i].label, p);
                return true;
            }
        }
    }
    ulog("=== PROBE: no combo worked ===");
    return false;
}

bool shadowusb_smoke_test(const char *vm_host, const char *usb_token,
                            int instance, const char *usb_client_id) {
    ulog("=========================================================");
    ulog("SHADOWUSB SMOKE TEST - start");
    ulog("  vm_host       = %s", vm_host ? vm_host : "(NULL)");
    ulog("  usb_token     = %s len=%zu",
         usb_token ? "(present)" : "(NULL)",
         usb_token ? strlen(usb_token) : 0);
    ulog("  instance      = %d", instance);
    ulog("  usb_client_id = %s", usb_client_id ? usb_client_id : "(NULL)");
    ulog("=========================================================");

    char id[128] = {0};
    char spice_url[512] = {0};
    char spice_secret[256] = {0};
    bool ok = shadowusb_register_device(vm_host, usb_token, instance, usb_client_id,
                                          id, sizeof(id),
                                          spice_url, sizeof(spice_url),
                                          spice_secret, sizeof(spice_secret));
    if (!ok) {
        ulog("shadowusb: REGISTER FAIL");
        return false;
    }
    ulog("shadowusb: REGISTER OK ✓");
    ulog("  id           = %s", id);
    ulog("  spice_url    = %s", spice_url);
    {   /* SEC2 2026-09-11 - it was written in full: the start and end only. */
        char m[48];
        ulog("  spice_secret = %s", log_mask_text(spice_secret, m, sizeof m));
    }
    ulog("shadowusb: TODO connect WSS spice_url + Spice link auth + usbredir");

    bool del = shadowusb_delete_device(vm_host, usb_token, instance, usb_client_id, id);
    ulog("shadowusb: DELETE = %s", del ? "OK" : "FAIL");

    ulog("=========================================================");
    ulog("SHADOWUSB SMOKE TEST — fin (success=%d)", (int)ok);
    ulog("=========================================================");
    return ok;
}

bool shadowusb_smoke_test_tunnel(const char *vm_host, const char *usb_token,
                                    int instance, const char *usb_client_id) {
    ulog("=========================================================");
    ulog("SHADOWUSB TUNNEL TEST - start (WSS+Spice path)");
    ulog("=========================================================");

    char id[128] = {0};
    char spice_url[512] = {0};
    char spice_secret[256] = {0};
    if (!shadowusb_register_device(vm_host, usb_token, instance, usb_client_id,
                                     id, sizeof(id),
                                     spice_url, sizeof(spice_url),
                                     spice_secret, sizeof(spice_secret))) {
        ulog("tunnel: REGISTER FAIL");
        return false;
    }
    ulog("tunnel: registered id=%s", id);
    ulog("tunnel: spice_url=%s", spice_url);
    ulog("tunnel: spice_secret_len=%zu", strlen(spice_secret));
    ulog("tunnel: WSS+Spice link not yet implemented — REST validated");

    bool del = shadowusb_delete_device(vm_host, usb_token, instance, usb_client_id, id);
    ulog("tunnel: DELETE = %s", del ? "OK" : "FAIL");

    ulog("=========================================================");
    ulog("SHADOWUSB TUNNEL TEST — fin");
    ulog("=========================================================");
    return true;
}
