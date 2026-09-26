#include "smoke_test.h"
#include "ctrl_rest.h"
#include "ctrl_tcp.h"
#include "ctrl_msgs.h"
#include "encryption.h"
#include "../services/http.h"
#include "../media/h264_decoder.h"
#include "../services/sockets_compat.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>   /* Sleep(ms) */
#  define shadow_sleep_s(s) Sleep((s) * 1000u)
#else
#  include <unistd.h>
#  define shadow_sleep_s(s) sleep(s)
#endif

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>

#include "../common/log.h"
#include "../services/log_mask.h"   /* SEC2: secrets in the log, start and end only */

/* S81 - the category is DECLARED here, not guessed from the message text.
 * `slog` stays at INFO, so the existing calls do not disappear. `sdbg` is
 * there for the high-volume lines, which move over to it one at a time. */
#define slog(...) JOURNAL_INFO_(JOURNAL_CAT_SESSION, __VA_ARGS__)
#define sdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SESSION, __VA_ARGS__)
/* Minimal JWT decode, just enough to pull out the "instance" field.
 * JWT = header.payload.signature, the payload being base64url-encoded JSON.
 * Returns the instance number (1..99), or -1 on error. */
int jwt_instance(const char *jwt);

static int jwt_extract_instance(const char *jwt) {
    if (!jwt) return -1;
    /* Find first '.' (header end) */
    const char *dot1 = strchr(jwt, '.');
    if (!dot1) return -1;
    const char *payload = dot1 + 1;
    const char *dot2 = strchr(payload, '.');
    size_t plen = dot2 ? (size_t)(dot2 - payload) : strlen(payload);
    if (plen > 4096) return -1;
    /* base64url-decode minimal (we just look for "instance":N inside as
     * substring after replacing '-_' with '+/' and adding padding) */
    char b64[4200];
    size_t out = 0;
    for (size_t i = 0; i < plen && out < sizeof(b64) - 5; i++) {
        char c = payload[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        b64[out++] = c;
    }
    /* S49 2026-08-26: the bound used to be `out < sizeof(b64)`, so `out` could
     * reach sizeof(b64) and the `b64[out] = 0` right below then wrote ONE byte
     * past the array, onto the stack. The compiler only pointed it out once the
     * surrounding code moved - it never had before. We now keep room for the
     * terminating zero. */
    while (out % 4 != 0 && out < sizeof(b64) - 1) b64[out++] = '=';
    b64[out] = 0;
    /* base64 decode: standard, fits in plen bytes */
    static const int8_t dt[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,
        ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,['g']=32,['h']=33,
        ['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,
        ['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,
        ['y']=50,['z']=51,
        ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,
        ['8']=60,['9']=61,['+']=62,['/']=63,
    };
    char json[4200];
    size_t jl = 0;
    for (size_t i = 0; i + 4 <= out && jl + 3 <= sizeof(json); i += 4) {
        if (b64[i] == '=') break;
        int a = dt[(unsigned char)b64[i]];
        int b = dt[(unsigned char)b64[i+1]];
        int c = b64[i+2] == '=' ? -1 : dt[(unsigned char)b64[i+2]];
        int d = b64[i+3] == '=' ? -1 : dt[(unsigned char)b64[i+3]];
        json[jl++] = (a << 2) | (b >> 4);
        if (c >= 0) json[jl++] = ((b & 0xf) << 4) | (c >> 2);
        if (d >= 0) json[jl++] = ((c & 0x3) << 6) | d;
    }
    json[jl < sizeof(json) ? jl : sizeof(json)-1] = 0;
    /* Search for "instance":N */
    const char *inst = strstr(json, "\"instance\":");
    if (!inst) return -1;
    inst += 11;
    while (*inst == ' ') inst++;
    int v = atoi(inst);
    return (v >= 1 && v <= 99) ? v : -1;
}

/* Public wrapper for jwt_instance - used by other modules. */
int jwt_instance(const char *jwt) {
    return jwt_extract_instance(jwt);
}

/* Generates an ASCII UUID v4 (36 chars + \0) from the wolfSSL RNG. */
static bool gen_uuid_v4(char out[37]) {
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0) return false;
    uint8_t b[16];
    int rc = wc_RNG_GenerateBlock(&rng, b, 16);
    wc_FreeRng(&rng);
    if (rc != 0) return false;
    /* Set version 4 + variant */
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return true;
}

bool streaming_smoke_test_one(const char *vm_host, const char *bearer,
                                int client_type, const char *bearer_label) {
    slog("---");
    slog("smoke: try { type=%d, bearer=%s }", client_type, bearer_label);
    char opaque[256];
    snprintf(opaque, sizeof(opaque),
             "{\\\"os\\\":\\\"Switch\\\",\\\"arch\\\":\\\"aarch64\\\","
             "\\\"platform-type\\\":\\\"console\\\","
             "\\\"timestamp\\\":\\\"%ld\\\"}",
             (long)time(NULL));
    shadow_session_creds c = {0};
    bool ok = ctrl_rest_register_client(vm_host, bearer, client_type, opaque, &c);
    if (ok) {
        /* `tok=%.20s` showed twenty leading characters of a live token, and
         * `remote=%s` the account's public IP. Note that `redact()` in the
         * journal does not catch either: it keys off `token=`, not `tok=`.
         * The identical call 73 lines below already did this correctly. */
        char mt[64];
        slog("smoke: REGISTER OK ✓ id=%s tok=%s expiry=%ld",
             c.client_id, log_mask_text(c.streamingtoken, mt, sizeof mt),
             c.token_expiry_unix);
        slog("smoke: cleanup DELETE /3/clients/%s", c.client_id);
        bool del = ctrl_rest_unregister(vm_host, bearer, c.client_id);
        slog("smoke: DELETE = %s", del ? "OK" : "FAIL");
    } else {
        slog("smoke: FAILED (status visible in the ctrl_rest log above)");
    }
    return ok;
}

/* Triple variant: try launcher_jwt + type=0, main_jwt + type=1, and
 * main_jwt + type=launcher, so nothing is left untested. The logs are verbose
 * on purpose - the point is to learn which pair /3/clients accepts. */
bool streaming_smoke_test_full(const char *vm_host,
                                 const char *launcher_jwt,
                                 const char *main_jwt) {
    slog("=========================================================");
    slog("STREAMING SMOKE TEST - start (3 attempts)");
    slog("  vm_host       = %s", vm_host ? vm_host : "(NULL)");
    slog("  launcher_jwt  = %s len=%zu",
         launcher_jwt ? "(present)" : "(NULL)",
         launcher_jwt ? strlen(launcher_jwt) : 0);
    slog("  main_jwt      = %s len=%zu",
         main_jwt ? "(present)" : "(NULL)",
         main_jwt ? strlen(main_jwt) : 0);
    slog("=========================================================");

    if (!vm_host) { slog("smoke: missing vm_host, abort"); return false; }

    bool any_ok = false;
    if (launcher_jwt && *launcher_jwt) {
        any_ok |= streaming_smoke_test_one(vm_host, launcher_jwt,
                                             SHADOW_CLIENT_TYPE_LAUNCHER,
                                             "launcher_jwt");
    }
    if (main_jwt && *main_jwt) {
        any_ok |= streaming_smoke_test_one(vm_host, main_jwt,
                                             SHADOW_CLIENT_TYPE_MAIN,
                                             "main_jwt");
    }
    /* Cross-check: main_jwt with type=launcher (seen in some pcaps) */
    if (main_jwt && *main_jwt) {
        any_ok |= streaming_smoke_test_one(vm_host, main_jwt,
                                             SHADOW_CLIENT_TYPE_LAUNCHER,
                                             "main_jwt+type=launcher");
    }

    slog("=========================================================");
    slog("STREAMING SMOKE TEST — fin (any_success=%d)", (int)any_ok);
    slog("=========================================================");
    return any_ok;
}

/* Compat: the original signature, calling the simple one-bearer version. */
bool streaming_smoke_test(const char *vm_host, const char *bearer) {
    return streaming_smoke_test_one(vm_host, bearer,
                                       SHADOW_CLIENT_TYPE_LAUNCHER, "single");
}

/* M32 - direct TCP/TLS bootstrap + Authentication + Encryption.
 * Reuses the streaming_token and client_id the proximus flow already obtained
 * (POST <proximus_url>/clients, in proximus_create_main_client). */
bool streaming_smoke_test_m32(const char *vm_host,
                                const char *streaming_token,
                                const char *client_id,
                                const char *bearer_jwt) {
    slog("=========================================================");
    slog("M32 BOOTSTRAP TEST - start");
    slog("  vm_host          = %s", vm_host ? vm_host : "(NULL)");
    {   /* SEC2 2026-09-11 - it went out in full: the journal's `token=`
         * redaction does not match "token  = ", with its spaces. */
        char m[48];
        slog("  streaming_token  = %s", log_mask_text(streaming_token, m, sizeof m));
    }
    slog("  client_id        = %s", client_id ? client_id : "(NULL)");
    slog("=========================================================");

    if (!vm_host || !streaming_token || !*streaming_token
        || !client_id || !*client_id) {
        slog("M32: missing vm_host / streaming_token / client_id, abort");
        return false;
    }

    bool ok = false;
    ctrl_tcp_session *tcp = NULL;

    /* Step 1.5: replay the REST sequence of the official desktop app, so that
     * the VM brings its streaming ports up:
     *   GET /N/version    (poll the server capabilities)
     *   GET /N/status     (state probe)
     * Those two calls seem to be what tells the VM a client is coming - the
     * 2026-05-06 tcpdump shows :13011 only opening AFTER them, with the
     * SSE /N/stream also live. */
    int rest_instance = jwt_extract_instance(bearer_jwt);
    if (rest_instance <= 0) rest_instance = 6;
    /* Strip the ipv6-/ipv4- prefix for the HTTP calls: http.c forces
     * IPRESOLVE_V4 and the ipv6-... hostname has no A record. The bare gpu-*
     * hostname carries both A and AAAA, so IPv4 resolves there. */
    const char *http_host = vm_host;
    if (strncmp(vm_host, "ipv6-", 5) == 0) http_host = vm_host + 5;
    else if (strncmp(vm_host, "ipv4-", 5) == 0) http_host = vm_host + 5;
    char ver_url[512], status_url[512];
    snprintf(ver_url, sizeof(ver_url),
             "https://%s/%d/version", http_host, rest_instance);
    snprintf(status_url, sizeof(status_url),
             "https://%s/%d/status", http_host, rest_instance);

    {
        http_response v = {0};
        slog("M32: GET %s ...", ver_url);
        if (http_get(ver_url, bearer_jwt, &v)) {
            slog("M32: /version → status=%ld len=%zu", v.status, v.len);
        } else {
            slog("M32: /version transport FAIL");
        }
        http_free(&v);
        /* Poll /N/status until streamer_up=true (30s max). Before that, the
         * streaming port :13011 is simply not bound. */
        int streamer_ready = 0;
        for (int i = 0; i < 15; i++) {
            http_response s = {0};
            if (http_get(status_url, bearer_jwt, &s) && s.data) {
                slog("M32: /status[%d] body=%.180s", i, s.data);
                if (strstr(s.data, "\"streamer_up\": true")
                    || strstr(s.data, "\"streamer_up\":true")) {
                    streamer_ready = 1;
                    http_free(&s);
                    break;
                }
            }
            http_free(&s);
            shadow_sleep_s(2);
        }
        if (!streamer_ready) {
            slog("M32: streamer_up=true never reached — abort");
            goto cleanup;
        }
        slog("M32: streamer_up=true ✓ — sleep 1s puis tente :13011");
        shadow_sleep_s(1);
    }

    /* Step 2: open TCP+TLS to host:13011, with retries - the port can take
     * 1-2s to open after the REST calls that trigger it. */
    slog("M32: opening ctrl_tcp to %s:13011 (no ALPN, no SNI)...", vm_host);
    for (int attempt = 1; attempt <= 6 && !tcp; attempt++) {
        if (attempt > 1) {
            slog("M32: retry %d/6 in 2s...", attempt);
            shadow_sleep_s(2);
        }
        tcp = ctrl_tcp_open_port(vm_host, 13011, 5000);
    }
    if (!tcp) {
        slog("M32: ctrl_tcp_open FAIL after 6 retries");
        goto cleanup;
    }
    slog("M32: ctrl_tcp_open OK ✓");
    ctrl_tcp_set_bearer(tcp, bearer_jwt);
    int instance = jwt_extract_instance(bearer_jwt);
    if (instance > 0) {
        slog("M32: JWT instance=%d → using path /%d/forward", instance, instance);
        ctrl_tcp_set_instance(tcp, instance);
    } else {
        slog("M32: JWT instance decode FAIL — using default /3/forward");
    }

    /* Step 3: generate sessionUniqueId + connectionUniqueId */
    char session_id[37], connection_id[37];
    if (!gen_uuid_v4(session_id) || !gen_uuid_v4(connection_id)) {
        slog("M32: gen_uuid_v4 FAIL");
        goto cleanup;
    }
    slog("M32: session_id    = %s", session_id);
    slog("M32: connection_id = %s", connection_id);

    /* Step 4: send Capabilities FIRST, not Authentication - that is the first
     * message SslCtrlChanV2 expects, per the LD_PRELOAD capture of the
     * official desktop client. */
    uint8_t cap_buf[256];
    int cap_len = ctrl_build_capabilities(
        cap_buf, sizeof(cap_buf),
        "12.3.3",
        "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (cap_len < 0) { slog("M32: ctrl_build_capabilities FAIL"); goto cleanup; }
    slog("M32: Capabilities protobuf built len=%d", cap_len);
    /* Hex dump, to diff against the capture (cf. memory/project_sslctrlchanv2_wire_format_FOUND.md: 87 bytes expected). */
    {
        char line[80];
        for (int i = 0; i < cap_len; i += 16) {
            int p = 0;
            line[0] = 0;
            for (int j = 0; j < 16 && i + j < cap_len; j++)
                p += snprintf(line + p, sizeof(line) - p, "%02x ", cap_buf[i + j]);
            slog("  cap %04x: %s", i, line);
        }
    }
    if (!ctrl_tcp_send_cleartext(tcp, cap_buf, (size_t)cap_len)) {
        slog("M32: send Capabilities FAIL");
        goto cleanup;
    }
    slog("M32: Capabilities sent ✓");

    uint8_t reply_buf[8192];
    size_t reply_len = 0;
    if (!ctrl_tcp_recv_cleartext(tcp, reply_buf, sizeof(reply_buf),
                                   &reply_len, 8000)) {
        slog("M32: recv Capabilities reply FAIL — server didn't respond or wire format wrong");
        goto cleanup;
    }
    slog("M32: ✓✓✓ Capabilities reply received (len=%zu) ✓✓✓", reply_len);
    /* === A SERVER REPLY IS DUMPED SHORT, AND ONLY ON REQUEST ============
     *
     * This one carries a version string, not a secret - but SEC1 was exactly
     * a dump of "a reply that carries nothing sensitive", and the reply it
     * turned out to carry was a live ed25519 PRIVATE key, written into a log
     * that is mirrored over the network. The rule that follows from it is not
     * "dump every reply except that one", it is: a server reply is dumped
     * SHORT by default, and in full only when somebody asks.
     *
     * Same shape as `ctrl_session.c`'s M13 dump: 24 bytes is enough to see the
     * framing and the type, and `SHADOW_DUMP_M32=1` restores the full dump for
     * a reverse-engineering session, knowingly. */
    {
        static int g_dump_m32 = -1;
        if (g_dump_m32 < 0) {
            const char *e = getenv("SHADOW_DUMP_M32");
            g_dump_m32 = e ? atoi(e) : 0;
        }
        const size_t cap_dump = g_dump_m32 ? (reply_len < 256 ? reply_len : 256)
                                           : (reply_len < 24 ? reply_len : 24);
        for (size_t i = 0; i < cap_dump; i += 16) {
            char line[80] = {0}; int p = 0;
            for (size_t j = 0; j < 16 && i + j < cap_dump; j++)
                p += snprintf(line + p, sizeof(line) - p, "%02x ", reply_buf[i + j]);
            slog("  reply %04zx: %s", i, line);
        }
        if (cap_dump < reply_len)
            slog("  (+%zu B not journalled - SHADOW_DUMP_M32=1 for the rest)",
                 reply_len - cap_dump);
    }

    /* Step 5 : Authentication protobuf */
    uint8_t auth_buf[2048];
    int auth_len = ctrl_build_authentication(
        auth_buf, sizeof(auth_buf),
        false,
        SHADOW_PERM_VIDEO | SHADOW_PERM_AUDIO_OUT
            | SHADOW_PERM_CURSOR | SHADOW_PERM_INPUT
            | SHADOW_PERM_GAMEPAD | SHADOW_PERM_CLIPBOARD,
        streaming_token,
        client_id,
        session_id,
        connection_id);
    if (auth_len < 0) {
        slog("M32: ctrl_build_authentication FAIL");
        goto cleanup;
    }
    slog("M32: Authentication protobuf built len=%d", auth_len);
    if (!ctrl_tcp_send_cleartext(tcp, auth_buf, (size_t)auth_len)) {
        slog("M32: send Authentication FAIL");
        goto cleanup;
    }
    slog("M32: Authentication sent ✓");
    if (!ctrl_tcp_recv_cleartext(tcp, reply_buf, sizeof(reply_buf),
                                   &reply_len, 8000)) {
        slog("M32: recv Authentication reply FAIL");
        goto cleanup;
    }
    slog("M32: ✓✓✓ Authentication reply received (len=%zu) ✓✓✓", reply_len);
    /* Parse v2 - extracts the server-side 20B hash. */
    shadow_auth_reply auth_reply = {0};
    if (ctrl_parse_authentication_reply_v2(reply_buf, reply_len, &auth_reply)
        && auth_reply.has_hash) {
        /* SEC2, and this file is where SEC2 did NOT reach. The hash is the
         * identifier every UDP side channel registers with; printing all
         * twenty bytes hands a log reader the session. The masking helper
         * shows two bytes at each end, which is enough to tell two sessions
         * apart and useless to anyone else. */
        char m[64];
        slog("M32: auth hash extracted: %s",
             log_mask_bytes(auth_reply.hash, 20, m, sizeof m));
    } else {
        slog("M32: Auth hash NOT found in reply — UDP register impossible");
    }

    /* Step 6: build + send the Encryption request */
    uint32_t algos[] = { SHADOW_ALG_NONE, 2, 4, SHADOW_ALG_CHACHA20_POLY1305 };
    uint8_t enc_buf[256];
    int enc_len = ctrl_build_encryption_request(enc_buf, sizeof(enc_buf),
                                                  algos, 4, NULL);
    if (enc_len < 0) {
        slog("M32: ctrl_build_encryption_request FAIL");
        goto cleanup;
    }
    slog("M32: Encryption request built len=%d", enc_len);
    if (!ctrl_tcp_send_cleartext(tcp, enc_buf, (size_t)enc_len)) {
        slog("M32: send Encryption FAIL");
        goto cleanup;
    }
    slog("M32: Encryption request SENT ✓");

    /* Step 7: receive + parse the Encryption reply -> 32B key */
    if (!ctrl_tcp_recv_cleartext(tcp, reply_buf, sizeof(reply_buf),
                                  &reply_len, 10000)) {
        slog("M32: recv Encryption reply FAIL");
        goto cleanup;
    }
    slog("M32: Encryption reply received len=%zu", reply_len);
    shadow_encryption_reply enc_reply = {0};
    if (!ctrl_parse_encryption_reply(reply_buf, reply_len, &enc_reply)) {
        /* === NO HEX DUMP HERE, AND THAT IS THE POINT =====================
         *
         * This is the reply that CARRIES the 32-byte chacha20 key. Dumping its
         * first 80 bytes on a parse failure meant that any failure for a
         * reason other than a missing key - a reordered field, an added one -
         * wrote the key into the log in the clear. That is the exact shape of
         * SEC1, where a hex dump of a channel-announcement reply wrote a live
         * ed25519 PRIVATE key into a log that was mirrored over the network.
         *
         * What a diagnosis actually needs is the SHAPE of the message, not its
         * contents: the length, and whether the field was found at all. */
        slog("M32: parse Encryption reply FAIL - %u B received, no usable key "
             "field. Set SHADOW_DUMP_CHUNKS=1 to capture the bytes offline "
             "rather than journalling them.", (unsigned)reply_len);
        goto cleanup;
    }
    slog("M32: Encryption reply parsed ✓");
    slog("  chosen_algorithm  = %u", enc_reply.chosen_algorithm);
    {   /* Eight contiguous bytes is a quarter of the key in one run - past
         * what SEC2 allows, and directly useful to anyone holding the pcap. */
        char m[64];
        slog("  key               = %s",
             log_mask_bytes(enc_reply.key, 32, m, sizeof m));
    }
    slog("  nonce_extra_len   = %zu", enc_reply.nonce_extra_len);
    slog("  nonce_counter     = %llu", (unsigned long long)enc_reply.nonce_counter);
    slog("  has_tls_capability= %d", (int)enc_reply.has_tls_capability);

    /* Step 8: RegisterSession (display_info + resolution + scale) */
    uint8_t reg_buf[512];
    int reg_len = ctrl_build_register_session(reg_buf, sizeof(reg_buf), 1920, 1080);
    if (reg_len < 0) {
        slog("M32: ctrl_build_register_session FAIL");
        goto cleanup;
    }
    slog("M32: RegisterSession protobuf built len=%d", reg_len);
    if (!ctrl_tcp_send_cleartext(tcp, reg_buf, (size_t)reg_len)) {
        slog("M32: send RegisterSession FAIL");
        goto cleanup;
    }
    slog("M32: RegisterSession sent ✓");
    if (!ctrl_tcp_recv_cleartext(tcp, reply_buf, sizeof(reply_buf),
                                   &reply_len, 8000)) {
        slog("M32: recv RegisterSession reply FAIL");
        goto cleanup;
    }
    slog("M32: ✓✓✓ RegisterSession reply received (len=%zu) ✓✓✓", reply_len);

    /* Step 9: create the chacha20-poly1305 shadow_cipher from the extracted
     * 32B key. It decrypts the video/audio UDP frames (slot*1000+10 / +12).
     * The TCP control channel stays in cleartext. */
    shadow_cipher *cipher = shadow_cipher_create(enc_reply.key, enc_reply.key);
    if (!cipher) {
        slog("M32: shadow_cipher_create FAIL");
        goto cleanup;
    }
    slog("M32: chacha20-poly1305 cipher created ✓");

    /* Step 10: send the 8 channel announcements (f1=5..12) to activate the
     * video/audio/cursor/input channels server-side. Without them the server
     * pushes no UDP frame at all. */
    for (int seq = 5; seq <= 12; seq++) {
        uint8_t ann_buf[256];
        int ann_len = ctrl_build_channel_announcement(ann_buf, sizeof(ann_buf),
                                                       seq, seq - 5);
        if (ann_len < 0) {
            slog("M32: build channel announcement seq=%d FAIL", seq);
            shadow_cipher_destroy(cipher);
            goto cleanup;
        }
        if (!ctrl_tcp_send_cleartext(tcp, ann_buf, (size_t)ann_len)) {
            slog("M32: send channel announcement seq=%d FAIL", seq);
            shadow_cipher_destroy(cipher);
            goto cleanup;
        }
        slog("M32: channel announcement seq=%d sent (%dB)", seq, ann_len);
    }
    /* Drain any reply (server may answer once for the batch) */
    if (ctrl_tcp_recv_cleartext(tcp, reply_buf, sizeof(reply_buf),
                                  &reply_len, 3000)) {
        slog("M32: announcements reply received len=%zu", reply_len);
    }

    shadow_cipher_destroy(cipher);

    ok = true;
    slog("M32: BOOTSTRAP COMPLETE — control encrypted + session registered + cipher ready ✓✓✓");

    /* P13 (2026-05-08): UDP register, byte-exact with the desktop app.
     * FIXED ports here: :13010 video / :13012 audio / :13013 input.
     * The 25B register packet = `[0x41 0x01 0x00 0x14 0x00][20B hash from the
     * Auth reply]`. */
    int udp_video = -1, udp_audio = -1, udp_input = -1, udp_cursor = -1;
    if (auth_reply.has_hash) {
        const struct { int port; int *fd; const char *name; } ports[] = {
            {13010, &udp_video,  "video"},
            {13012, &udp_audio,  "audio (DTLS 1.2)"},
            {13013, &udp_input,  "input"},
            {13030, &udp_cursor, "cursor"},
        };
        uint8_t reg_pkt[32];
        int reg_len = ctrl_build_udp_register(reg_pkt, sizeof(reg_pkt), auth_reply.hash);
        for (size_t k = 0; k < sizeof(ports)/sizeof(ports[0]); k++) {
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            char port_s[8]; snprintf(port_s, sizeof(port_s), "%d", ports[k].port);
            if (getaddrinfo(vm_host, port_s, &hints, &res) != 0 || !res) continue;
            int s = socket(res->ai_family, SOCK_DGRAM, 0);
            if (s < 0) { freeaddrinfo(res); continue; }
            connect(s, res->ai_addr, res->ai_addrlen);
            ssize_t sent = send(s, reg_pkt, reg_len, 0);
            slog("M32: UDP register :%d (%s) → %zd bytes", ports[k].port, ports[k].name, sent);
            shadow_set_nonblocking(s, 1);
            *ports[k].fd = s;
            freeaddrinfo(res);
        }
    }

    /* Video cipher, used to decrypt the UDP frames. */
    shadow_cipher *vid_cipher = shadow_cipher_create(enc_reply.key, enc_reply.key);
    if (!vid_cipher) slog("M32: video cipher create FAIL");

    /* h264_decoder, to wire chacha20 through to libavcodec. */
    /* Dead variables removed on 2026-08-26: nothing read them any more. */
    extern void smoke_h264_cb(int w, int h, const uint8_t *y, int ys,
                                const uint8_t *u, int us, const uint8_t *v, int vs,
                                int fmt, int64_t pts, void *user);
    h264_decoder *h264 = h264_decoder_create(smoke_h264_cb, NULL);
    if (!h264) slog("M32: h264_decoder_create FAIL");

    slog("M32: observing UDP for 25s (after register)...");
    int video_pkts = 0, audio_pkts = 0, input_pkts = 0, cursor_pkts = 0;
    int decrypt_ok = 0, decrypt_fail = 0, cursor_ok = 0, cursor_fail = 0;
    uint64_t video_bytes = 0, cursor_bytes = 0;
    for (int i = 0; i < 25; i++) {
        for (int j = 0; j < 200; j++) {
            uint8_t pkt[2048];
            ssize_t n;
            int got = 0;
            if (udp_video >= 0) {
                n = recv(udp_video, pkt, sizeof(pkt), 0);
                if (n > 0) {
                    video_pkts++; video_bytes += n; got = 1;
                    if (video_pkts <= 3) {
                        slog("M32: VIDEO #%d len=%zd first=%02x %02x %02x %02x %02x %02x %02x %02x",
                             video_pkts, n, pkt[0], pkt[1], pkt[2], pkt[3],
                             pkt[4], pkt[5], pkt[6], pkt[7]);
                    }
                    /* Video wire format: `[11B SUFP header][ct][nonce 12B][tag 16B]`.
                     * Byte 10 of the header is the encryption flag: 0x01 means
                     * encrypted (decrypt it), 0x00 means plaintext (skip, or
                     * pass through). */
                    if (vid_cipher && n > 11 + 28 && pkt[10] == 0x01) {
                        uint8_t scratch[2048];
                        memcpy(scratch, pkt + 11, n - 11 - 28);
                        const uint8_t *nonce = pkt + n - 28;
                        const uint8_t *tag   = pkt + n - 16;
                        if (shadow_cipher_decrypt_unsafe(vid_cipher, scratch, n - 11 - 28, nonce, tag)) {
                            decrypt_ok++;
                            /* Dump every decrypted plaintext to a file, for
                             * offline analysis (ffprobe, hex search for NALs...). */
                            static FILE *dump_f = NULL;
                            if (!dump_f) dump_f = fopen("/tmp/decrypted_video.bin", "wb");
                            if (dump_f) {
                                /* Write a 4-byte LE length then the plain bytes, so the file parses easily. */
                                int pl = n - 11 - 28;
                                fwrite(&pl, 1, 4, dump_f);
                                fwrite(scratch, 1, pl, dump_f);
                            }
                            if (decrypt_ok <= 5) {
                                slog("M32: ✓ #%d hdr=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x  plain=%02x %02x %02x %02x",
                                     decrypt_ok,
                                     pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5],
                                     pkt[6], pkt[7], pkt[8], pkt[9], pkt[10],
                                     scratch[0], scratch[1], scratch[2], scratch[3]);
                            }
                            /* Decode the Shadow VideoFrame header:
                             *   byte 0    = 0x02 (version | codec H.264)
                             *   bytes 1-4 = timestamp, uint32 LE
                             *   byte 5    = flag (0x00 normal, 0x01 keyframe,
                             *               followed by a 13B extension)
                             *   then the NAL data (00 00 00 01 + NAL bytes)
                             * Dump the raw NAL bytestream to /tmp/raw.h264 for ffprobe. */
                            int pl = n - 11 - 28;
                            if (pl >= 6 && scratch[0] == 0x02) {
                                int hdr = (scratch[5] == 0x01) ? 19 : 6;
                                if (pl > hdr) {
                                    static FILE *h264_f = NULL;
                                    if (!h264_f) h264_f = fopen("/tmp/raw.h264", "wb");
                                    if (h264_f) fwrite(scratch + hdr, 1, pl - hdr, h264_f);
                                }
                            }
                        } else {
                            decrypt_fail++;
                            if (decrypt_fail <= 5) {
                                slog("M32: ✗ #%d hdr=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x  (n=%zd)",
                                     decrypt_fail,
                                     pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5],
                                     pkt[6], pkt[7], pkt[8], pkt[9], pkt[10], n);
                            }
                        }
                    }
                }
            }
            if (udp_audio >= 0) {
                n = recv(udp_audio, pkt, sizeof(pkt), 0);
                if (n > 0) {
                    audio_pkts++; got = 1;
                    if (audio_pkts <= 3)
                        slog("M32: AUDIO #%d len=%zd", audio_pkts, n);
                }
            }
            if (udp_input >= 0) {
                n = recv(udp_input, pkt, sizeof(pkt), 0);
                if (n > 0) {
                    input_pkts++; got = 1;
                    if (input_pkts <= 3)
                        slog("M32: INPUT #%d len=%zd", input_pkts, n);
                }
            }
            if (udp_cursor >= 0) {
                n = recv(udp_cursor, pkt, sizeof(pkt), 0);
                if (n > 0) {
                    cursor_pkts++; cursor_bytes += n; got = 1;
                    /* Same wire format as :13010 (chacha20-poly1305 + 11B header) */
                    if (vid_cipher && n > 11 + 28 && pkt[10] == 0x01) {
                        uint8_t scratch[2048];
                        memcpy(scratch, pkt + 11, n - 11 - 28);
                        const uint8_t *nonce = pkt + n - 28;
                        const uint8_t *tag   = pkt + n - 16;
                        if (shadow_cipher_decrypt_unsafe(vid_cipher, scratch, n - 11 - 28, nonce, tag)) {
                            cursor_ok++;
                            if (cursor_ok <= 3) {
                                slog("M32: CURSOR ✓ #%d len=%zd plain first=%02x %02x %02x %02x %02x %02x %02x %02x",
                                     cursor_ok, n, scratch[0], scratch[1], scratch[2], scratch[3],
                                     scratch[4], scratch[5], scratch[6], scratch[7]);
                            }
                        } else {
                            cursor_fail++;
                        }
                    } else if (cursor_pkts <= 3) {
                        slog("M32: CURSOR raw #%d len=%zd byte10=0x%02x", cursor_pkts, n, n > 10 ? pkt[10] : 0);
                    }
                }
            }
            if (!got) break;
        }
        if (i % 5 == 4)
            slog("M32: [t=%ds] video=%d (%llu B) audio=%d input=%d cursor=%d (%llu B)",
                 i+1, video_pkts, (unsigned long long)video_bytes,
                 audio_pkts, input_pkts, cursor_pkts, (unsigned long long)cursor_bytes);
        shadow_sleep_s(1);
    }
    slog("M32: UDP done — video=%d/%d ok cursor=%d/%d ok audio=%d input=%d",
         decrypt_ok, video_pkts, cursor_ok, cursor_pkts, audio_pkts, input_pkts);
    if (h264) {
        h264_decoder_stats_t hs;
        h264_decoder_get_stats(h264, &hs);
        slog("M32: h264 decoder stats: frames_decoded=%u nals=%u single=%u err=%u %dx%d",
             hs.frames_decoded, hs.nals_total, hs.single_nal_count,
             hs.decode_errors, hs.last_width, hs.last_height);
        h264_decoder_destroy(h264);
    }
    if (vid_cipher) shadow_cipher_destroy(vid_cipher);
    if (udp_video >= 0) shadow_closesocket(udp_video);
    if (udp_audio >= 0) shadow_closesocket(udp_audio);
    if (udp_input >= 0) shadow_closesocket(udp_input);
    if (udp_cursor >= 0) shadow_closesocket(udp_cursor);

cleanup:
    if (tcp) ctrl_tcp_close(tcp);
    /* No DELETE here: the client_id belongs to the caller's proximus flow,
     * which will delete it (or let it expire). */

    slog("=========================================================");
    slog("M32 BOOTSTRAP TEST — fin (success=%d)", (int)ok);
    slog("=========================================================");
    return ok;
}

/* h264_decoder callback - logs the dimensions, and only the first few frames,
 * so it does not flood the log. */
void smoke_h264_cb(int w, int h, const uint8_t *y, int ys,
                     const uint8_t *u, int us, const uint8_t *v, int vs,
                     int fmt, int64_t pts, void *user) {
    (void)y; (void)ys; (void)u; (void)us; (void)v; (void)vs; (void)fmt;
    (void)pts; (void)user;
    static int count = 0;
    count++;
    if (count <= 3 || count % 30 == 0)
        slog("M32: 🎬 H264 frame #%d %dx%d", count, w, h);
}
