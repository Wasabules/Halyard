#include "oauth.h"
#include "http.h"
#include "config.h"
#include "applock_store.h"
#include "atomic_file.h"   /* AF4 - the token is written beside, then moved over */

#include <jansson.h>
#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#if defined(_WIN32)
#  include <direct.h>     /* _mkdir */
#  define shadow_mkdir(p) _mkdir(p)
#else
#  define shadow_mkdir(p) mkdir((p), 0755)
#endif

// ============================================================================
// helpers
// ============================================================================

static char *jstrdup(json_t *root, const char *key) {
    json_t *v = json_object_get(root, key);
    if (!v || !json_is_string(v)) return NULL;
    const char *s = json_string_value(v);
    return s ? strdup(s) : NULL;
}

static int jint(json_t *root, const char *key, int defval) {
    json_t *v = json_object_get(root, key);
    if (!v || !json_is_integer(v)) return defval;
    return (int)json_integer_value(v);
}

static char *url_encode(const char *s) {
    if (!s) return strdup("");
    CURL *h = curl_easy_init();
    shadow_curl_apply_ca(h);   /* WIN2 - see http.h */
    shadow_curl_apply_share(h);   /* DNS + session TLS partagees */
    if (!h) return NULL;
    char *enc = curl_easy_escape(h, s, 0);
    char *out = enc ? strdup(enc) : NULL;
    if (enc) curl_free(enc);
    curl_easy_cleanup(h);
    return out;
}

// ============================================================================
// discovery
// ============================================================================

void oauth_discovery_free(OidcDiscovery *d) {
    if (!d) return;
    free(d->device_authorization_endpoint); d->device_authorization_endpoint = NULL;
    free(d->token_endpoint);                d->token_endpoint = NULL;
    free(d->revocation_endpoint);           d->revocation_endpoint = NULL;
    free(d->userinfo_endpoint);             d->userinfo_endpoint = NULL;
    free(d->issuer);                        d->issuer = NULL;
}

bool oauth_discover(OidcDiscovery *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    http_response resp;
    bool t = http_get(SHADOW_OIDC_DISCOVERY, NULL, &resp);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) {
        fprintf(stderr, "oauth_discover: JSON: %s\n", err.text);
        return false;
    }

    out->device_authorization_endpoint = jstrdup(root, "device_authorization_endpoint");
    out->token_endpoint                = jstrdup(root, "token_endpoint");
    out->revocation_endpoint           = jstrdup(root, "revocation_endpoint");
    out->userinfo_endpoint             = jstrdup(root, "userinfo_endpoint");
    out->issuer                        = jstrdup(root, "issuer");
    json_decref(root);

    if (!out->device_authorization_endpoint || !out->token_endpoint) {
        oauth_discovery_free(out);
        return false;
    }
    return true;
}

// ============================================================================
// device flow init
// ============================================================================

void oauth_device_init_free(DeviceGrantInit *g) {
    if (!g) return;
    free(g->device_code);                   g->device_code = NULL;
    free(g->user_code);                     g->user_code = NULL;
    free(g->verification_uri);              g->verification_uri = NULL;
    free(g->verification_uri_complete);     g->verification_uri_complete = NULL;
}

bool oauth_device_init(const OidcDiscovery *d, DeviceGrantInit *out, long *http_status) {
    memset(out, 0, sizeof(*out));
    if (http_status) *http_status = 0;

    char *cid = url_encode(SHADOW_OAUTH_CLIENT_ID);
    char *scp = url_encode(SHADOW_OAUTH_SCOPE);
    if (!cid || !scp) { free(cid); free(scp); return false; }

    char body[1024];
    int n = snprintf(body, sizeof(body), "client_id=%s&scope=%s", cid, scp);
    free(cid); free(scp);
    if (n <= 0 || n >= (int)sizeof(body)) return false;

    http_response resp;
    bool t = http_post_form(d->device_authorization_endpoint, body, &resp);
    if (http_status) *http_status = resp.status;
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        if (resp.data) fprintf(stderr, "device_init: HTTP %ld body: %.200s\n", resp.status, resp.data);
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    out->device_code              = jstrdup(root, "device_code");
    out->user_code                = jstrdup(root, "user_code");
    out->verification_uri         = jstrdup(root, "verification_uri");
    out->verification_uri_complete= jstrdup(root, "verification_uri_complete");
    out->expires_in               = jint(root, "expires_in", 600);
    out->interval                 = jint(root, "interval", 5);
    json_decref(root);

    if (!out->device_code || !out->user_code) {
        oauth_device_init_free(out);
        return false;
    }
    if (out->interval < 1) out->interval = 5;
    return true;
}

// ============================================================================
// poll for token
// ============================================================================

void oauth_state_free(ShadowAuthState *s) {
    if (!s) return;
    free(s->access_token);  s->access_token = NULL;
    free(s->refresh_token); s->refresh_token = NULL;
    free(s->id_token);      s->id_token = NULL;
    free(s->token_type);    s->token_type = NULL;
    s->expires_at = 0;
}

OAuthPollResult oauth_device_poll(const OidcDiscovery *d, const char *device_code,
                                   const char *client_id, ShadowAuthState *out) {
    char *enc_device = url_encode(device_code);
    char *enc_client = url_encode(client_id);
    if (!enc_device || !enc_client) {
        free(enc_device); free(enc_client);
        return OAUTH_POLL_OTHER_ERROR;
    }

    char body[2048];
    int n = snprintf(body, sizeof(body),
                     "grant_type=urn:ietf:params:oauth:grant-type:device_code"
                     "&device_code=%s&client_id=%s",
                     enc_device, enc_client);
    free(enc_device); free(enc_client);
    if (n <= 0 || n >= (int)sizeof(body)) return OAUTH_POLL_OTHER_ERROR;

    http_response resp;
    bool t = http_post_form(d->token_endpoint, body, &resp);
    if (!t) {
        http_free(&resp);
        return OAUTH_POLL_NETWORK_ERROR;
    }

    json_error_t err;
    json_t *root = resp.data ? json_loads(resp.data, 0, &err) : NULL;

    OAuthPollResult ret = OAUTH_POLL_OTHER_ERROR;

    if (resp.status >= 200 && resp.status < 300 && root) {
        // success path
        memset(out, 0, sizeof(*out));
        out->access_token  = jstrdup(root, "access_token");
        out->refresh_token = jstrdup(root, "refresh_token");
        out->id_token      = jstrdup(root, "id_token");
        out->token_type    = jstrdup(root, "token_type");
        int expires_in     = jint(root, "expires_in", 3600);
        out->expires_at    = time(NULL) + expires_in;
        if (out->access_token) ret = OAUTH_POLL_SUCCESS;
    } else if (root) {
        // error response — RFC 8628 returns error string
        const char *e = NULL;
        json_t *je = json_object_get(root, "error");
        if (je && json_is_string(je)) e = json_string_value(je);
        if (e) {
            if      (strcmp(e, "authorization_pending") == 0) ret = OAUTH_POLL_PENDING;
            else if (strcmp(e, "slow_down")             == 0) ret = OAUTH_POLL_SLOW_DOWN;
            else if (strcmp(e, "access_denied")         == 0) ret = OAUTH_POLL_DENIED;
            else if (strcmp(e, "expired_token")         == 0) ret = OAUTH_POLL_EXPIRED;
            else                                              ret = OAUTH_POLL_OTHER_ERROR;
        } else {
            ret = OAUTH_POLL_OTHER_ERROR;
        }
    } else {
        ret = OAUTH_POLL_NETWORK_ERROR;
    }

    if (root) json_decref(root);
    http_free(&resp);
    return ret;
}

// ============================================================================
// refresh
// ============================================================================

bool oauth_refresh(const OidcDiscovery *d, const char *client_id, ShadowAuthState *s) {
    if (!s->refresh_token) return false;
    char *enc_refresh = url_encode(s->refresh_token);
    char *enc_client  = url_encode(client_id);
    if (!enc_refresh || !enc_client) { free(enc_refresh); free(enc_client); return false; }

    char body[2048];
    int n = snprintf(body, sizeof(body),
                     "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                     enc_refresh, enc_client);
    free(enc_refresh); free(enc_client);
    if (n <= 0 || n >= (int)sizeof(body)) return false;

    http_response resp;
    bool t = http_post_form(d->token_endpoint, body, &resp);
    if (!t || resp.status < 200 || resp.status >= 300 || !resp.data) {
        http_free(&resp);
        return false;
    }

    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    http_free(&resp);
    if (!root) return false;

    char *new_at = jstrdup(root, "access_token");
    char *new_rt = jstrdup(root, "refresh_token");  // may rotate
    char *new_id = jstrdup(root, "id_token");
    char *new_tt = jstrdup(root, "token_type");
    int   exp    = jint(root, "expires_in", 3600);
    json_decref(root);

    if (!new_at) {
        free(new_at); free(new_rt); free(new_id); free(new_tt);
        return false;
    }

    free(s->access_token);  s->access_token = new_at;
    if (new_rt) { free(s->refresh_token); s->refresh_token = new_rt; }
    if (new_id) { free(s->id_token); s->id_token = new_id; }
    if (new_tt) { free(s->token_type); s->token_type = new_tt; }
    s->expires_at = time(NULL) + exp;
    return true;
}

// ============================================================================
// persistence (refresh_token only, on SD)
// ============================================================================

/* This used to carry its OWN copy of the platform ladder - a third one - and
 * the copy drifted: `#else` meant `/tmp/halyard`, so on a PS Vita the
 * token was written into a directory that could not exist while `main.cpp`
 * created the right one a few lines earlier. The only trace was one line on
 * stderr, `mkdir /tmp/halyard: No such file or directory`, and the
 * symptom was a console asking to be paired again at every single launch.
 *
 * `shadow_ensure_data_dir()` derives the path from SHADOW_DATA_DIR and is the
 * only implementation left. */
static bool ensure_token_dir(void) {
    if (shadow_ensure_data_dir()) return true;
    fprintf(stderr, "the data directory (%s) cannot be created - the token will "
                    "not persist\n", SHADOW_DATA_DIR);
    return false;
}

/* UX3 B1 2026-05-18 - Token obfuscation at rest.
 *
 * Not strong encryption (no TPM, no device-bound key), but it blocks a casual
 * read of the plaintext token if the SD card is read from a PC or by other
 * software. XOR keystream derived from a hardcoded secret.
 *
 * File format v2: `[u8 magic=0xS2][xored_bytes...]`
 * File format v1 (legacy): plain text (= a read fallback, kept for backward
 * compatibility with existing older tokens).
 */
#define TOKEN_MAGIC_V2  0x52  /* 'R' = obfuscated v2 */
/* === V3 - REALLY ENCRYPTED (2026-09-02) ===
 *
 * v2 above says of itself that it is "not strong crypto" and that real security
 * "would need a device-bound key". The application lock supplies one: a random
 * master key that only a PIN, a pattern or a password can unseal
 * (shadow/applock.h). When it is available the token is sealed with
 * chacha20-poly1305 under that key, and the SD card alone no longer yields it.
 *
 * The three formats coexist on purpose, and the magic byte tells them apart:
 *   v1  plain text                      (read only, from before any of this)
 *   v2  [0x52][xor]                     (no lock configured: unchanged)
 *   v3  [0x53][nonce 12][ct][tag 16]    (lock configured and open)
 *
 * A v3 file with no master key CANNOT be read - that is the whole point - so the
 * caller is told there is no token and the user logs in again. This is why
 * removing the lock must first read the token back out and rewrite it as v2:
 * otherwise turning the lock off would silently throw the session away. */
#define TOKEN_MAGIC_V3  0x53

static void xor_obfuscate(uint8_t *data, size_t len) {
    /* Keystream derived from a secret plus the position. Not strong crypto, but
     * it prevents a casual read. Real security would need a device-bound key
     * (future work). */
    static const char *secret = "Sh4dow2Sw1tchT0k3n0bfu5c2026!#$";
    size_t slen = strlen(secret);
    for (size_t i = 0; i < len; i++) {
        data[i] ^= (uint8_t)(secret[i % slen]) ^ (uint8_t)((i * 17) & 0xFF);
    }
}

bool oauth_save_refresh(const ShadowAuthState *s) {
    if (!s || !s->refresh_token) return false;
    if (!ensure_token_dir()) return false;

    /* v3 whenever the lock is open. Checked at every save rather than once:
     * the lock can be armed mid-session, and the very next token rotation is
     * then the moment the file stops being readable from a computer. */
    if (applock_master_ready()) {
        const size_t l3 = strlen(s->refresh_token);
        uint8_t *sealed = (uint8_t *)malloc(1 + l3 + APPLOCK_SEAL_OVERHEAD);
        size_t sealed_n = 0;
        bool ok3 = false;
        if (!sealed) return false;
        sealed[0] = TOKEN_MAGIC_V3;
        if (applock_master_seal((const uint8_t *)s->refresh_token, l3,
                                sealed + 1, l3 + APPLOCK_SEAL_OVERHEAD, &sealed_n)) {
            /* AF4 2026-09-10 - beside, then moved over (atomic_file.h). An
             * in-place "wb" truncated first: a cut in between left an empty
             * token, and an empty token means a new pairing. */
            char tmp3[sizeof SHADOW_TOKEN_PATH + 4];
            FILE *f3 = atomic_file_open(SHADOW_TOKEN_PATH, tmp3, sizeof tmp3);
            if (f3) {
                ok3 = fwrite(sealed, 1, 1 + sealed_n, f3) == 1 + sealed_n;
                ok3 = atomic_file_commit(f3, tmp3, SHADOW_TOKEN_PATH, ok3) != 0;
            }
        }
        memset(sealed, 0, 1 + l3 + APPLOCK_SEAL_OVERHEAD);
        free(sealed);
        /* No silent fall-back to v2 on failure: writing the token in a weaker
         * form because the strong one failed is exactly the downgrade an
         * attacker would want, and the user would never be told. */
        return ok3;
    }

    /* AF4 - beside, then moved over, like v3 above. */
    char tmp[sizeof SHADOW_TOKEN_PATH + 4];
    FILE *f = atomic_file_open(SHADOW_TOKEN_PATH, tmp, sizeof tmp);
    if (!f) return false;
    size_t l = strlen(s->refresh_token);
    /* Allocate buffer + magic byte */
    uint8_t *buf = (uint8_t *)malloc(l + 1);
    if (!buf) { atomic_file_commit(f, tmp, SHADOW_TOKEN_PATH, 0); return false; }
    buf[0] = TOKEN_MAGIC_V2;
    memcpy(buf + 1, s->refresh_token, l);
    xor_obfuscate(buf + 1, l);  /* obfuscate token bytes, NOT magic */
    bool ok = fwrite(buf, 1, l + 1, f) == l + 1;
    /* Best-effort secure erase buffer before free */
    memset(buf, 0, l + 1);
    free(buf);
    return atomic_file_commit(f, tmp, SHADOW_TOKEN_PATH, ok) != 0;
}

bool oauth_load_refresh(char **out) {
    *out = NULL;
    /* AF4 - falls back to "refresh_token.new" in the one window where only it
     * exists (see atomic_file.h). */
    FILE *f = atomic_file_open_read(SHADOW_TOKEN_PATH, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }
    uint8_t *raw = (uint8_t *)malloc(sz + 1);
    if (!raw) { fclose(f); return false; }
    size_t r = fread(raw, 1, sz, f);
    fclose(f);
    raw[r] = '\0';

    char *buf = NULL;
    if (r > 1 && raw[0] == TOKEN_MAGIC_V3) {
        /* v3: sealed under the application lock's master key. Unreadable
         * without it, by design. */
        const size_t body = r - 1;
        size_t got = 0;
        if (body <= APPLOCK_SEAL_OVERHEAD) { free(raw); return false; }
        buf = (char *)malloc(body - APPLOCK_SEAL_OVERHEAD + 1);
        if (!buf) { free(raw); return false; }
        if (!applock_master_open(raw + 1, body, (uint8_t *)buf,
                                 body - APPLOCK_SEAL_OVERHEAD, &got)) {
            /* Either the lock has not been opened this session, or the file
             * belongs to a lock that has since been removed. Both mean "no
             * token": the user logs in again, which is the cost this format
             * trades for the card being useless on its own. */
            memset(raw, 0, r); free(raw);
            free(buf);
            return false;
        }
        buf[got] = '\0';
        memset(raw, 0, r); free(raw);
        *out = buf;
        return true;
    } else if (r > 0 && raw[0] == TOKEN_MAGIC_V2) {
        /* v2 obfuscated : skip magic, xor decode rest */
        size_t plen = r - 1;
        buf = (char *)malloc(plen + 1);
        if (!buf) { free(raw); return false; }
        memcpy(buf, raw + 1, plen);
        xor_obfuscate((uint8_t *)buf, plen);
        buf[plen] = '\0';
        memset(raw, 0, r); free(raw);
        /* trim trailing whitespace (= safety) */
        while (plen > 0 && (buf[plen-1] == '\n' || buf[plen-1] == '\r'
                            || buf[plen-1] == ' ' || buf[plen-1] == '\0')) {
            buf[--plen] = '\0';
        }
        if (plen == 0) { free(buf); return false; }
    } else {
        /* v1 legacy plaintext : back-compat read */
        buf = (char *)malloc(r + 1);
        if (!buf) { free(raw); return false; }
        memcpy(buf, raw, r); buf[r] = '\0';
        free(raw);
        size_t plen = r;
        while (plen > 0 && (buf[plen-1] == '\n' || buf[plen-1] == '\r'
                            || buf[plen-1] == ' ')) {
            buf[--plen] = '\0';
        }
        if (plen == 0) { free(buf); return false; }
        /* Auto-upgrade : re-save in v2 format next time oauth_save_refresh called */
    }
    *out = buf;
    return true;
}

/* See oauth.h (AUTH9). */
bool oauth_forget_refresh(void) {
    FILE *f = fopen(SHADOW_TOKEN_PATH, "r+b");
    if (f) {
        /* Overwrite in place, then unlink. The length is read back rather than
         * assumed: the three formats have three different sizes and a short
         * write would leave the tail of the old token on the card. */
        long n = 0;
        if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
        if (n > 0 && fseek(f, 0, SEEK_SET) == 0) {
            unsigned char zero[256];
            memset(zero, 0, sizeof zero);
            while (n > 0) {
                size_t chunk = (size_t)(n < (long)sizeof zero ? n : (long)sizeof zero);
                if (fwrite(zero, 1, chunk, f) != chunk) break;
                n -= (long)chunk;
            }
            fflush(f);
        }
        fclose(f);
    }
    /* `remove` failing because the file was never there is the state asked
     * for, so it is not an error; anything else is. */
    if (remove(SHADOW_TOKEN_PATH) != 0 && errno != ENOENT) {
        /* stderr like the rest of this file: oauth.c deliberately does not
         * pull in the journal, which would make a bottom-layer service depend
         * on another one for a line printed twice a year. */
        fprintf(stderr, "the stored session could not be removed (%s)\n",
                strerror(errno));
        return false;
    }
    return true;
}

/* See oauth.h. */
bool oauth_reencrypt_refresh(void) {
    char *tok = NULL;
    ShadowAuthState tmp;
    bool ok;
    if (!oauth_load_refresh(&tok) || !tok) return false;
    memset(&tmp, 0, sizeof tmp);
    tmp.refresh_token = tok;
    ok = oauth_save_refresh(&tmp);
    /* Wiped before it is freed: this is the one place the token exists in the
     * clear outside the code that uses it. */
    memset(tok, 0, strlen(tok));
    free(tok);
    return ok;
}

/* See oauth.h. The order is the point; do not split this up. */
bool oauth_unseal_refresh(void) {
    char *tok = NULL;
    ShadowAuthState tmp;
    bool ok;
    /* 1. Read WHILE the master key is still loaded. */
    if (!oauth_load_refresh(&tok) || !tok) {
        /* No readable token. Forget the key anyway - the caller is removing the
         * lock, and leaving it loaded would keep sealing new tokens under a key
         * whose only copies are about to be deleted. */
        applock_master_forget();
        return false;
    }
    /* 2. Forget it, so the save below cannot choose the sealed form. */
    applock_master_forget();
    /* 3. Write back, now in the obfuscated form. */
    memset(&tmp, 0, sizeof tmp);
    tmp.refresh_token = tok;
    ok = oauth_save_refresh(&tmp);
    memset(tok, 0, strlen(tok));
    free(tok);
    return ok;
}

/* UX3 B5 2026-05-18 - Check whether the access_token expiry is approaching.
 * Returns true when expiry < now + threshold_sec (= a refresh is advised). */
bool oauth_token_needs_refresh(const ShadowAuthState *s, int threshold_sec) {
    if (!s || s->expires_at == 0) return false;
    time_t now = time(NULL);
    return (s->expires_at - now) < threshold_sec;
}
