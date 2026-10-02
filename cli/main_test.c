/* halyard-cli — Headless bench/test for Linux desktop.
 *
 * 3 supported modes (= the --mode arg):
 *   native-stream (default)  Full native ctrl_session + H.264 decode + NAL stats
 *   native-smoke              Native M32 bootstrap only (= TLS+Auth+Encryption validation)
 *   health-check              = native-stream 30 s + PASS/FAIL verdict against thresholds (A2 2026-05-18)
 *
 * Common bootstrap = OAuth -> TINAG -> list VMs -> start -> SSE -> then branch by mode.
 * Sampling at 1 Hz for --duration, JSON dumped for bench_runner.py.
 *
 * Usage: ./halyard-cli [options]
 *   --duration=N       (default 60)  sampling duration
 *   --output=PATH      (default -)   path of the metrics JSON (- = stdout)
 *   --label=NAME       (default "")  label added to the JSON
 *   --activity         (default off) inject mouse-moves every 2 s (forces the server to push video)
 *   --vm-id=ID         (default auto)
 *   --mode=MODE        (default native-stream) = native-stream|native-smoke|health-check
 *   --threshold-fps=N      (default 20)  health-check: min fps (frames/sec)
 *   --threshold-decrypt=N  (default 95)  health-check: min decrypt success % (x100)
 *   --threshold-bottom=N   (default 30)  health-check: min bottom NAL ratio % (x100)
 *   --quiet
 *
 * Exit codes:
 *   0  OK / health PASS
 *   1  bootstrap fail (oauth/tinag/vm setup)
 *   2  native stream fail / no VMs
 *   5  user abort (SIGINT)
 *   6  health-check FAIL (= a threshold was not reached)
 */

#define _GNU_SOURCE  /* pthread_timedjoin_np */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>

#include "../core/services/oauth.h"
#include "../core/services/launcher.h"
#include "../core/services/proximus.h"
#include "../core/services/http.h"
#include "../core/services/config.h"
#include "../core/services/tinag.h"
#include "../core/services/time_sync.h"
#include "../core/services/telemetry.h"
#include "../core/session/ctrl_session_glue.h"
#include "../core/protocol/vid_reasm.h"   /* V9: the short-packet counter on :base+10 */
#include "../core/services/log.h"
#include "../core/services/stats.h"
#include "../core/input/shadow_input.h"

static volatile int g_abort = 0;
static void on_sigint(int _) { (void)_; g_abort = 1; }

/* === args === */
typedef struct {
    int   duration_sec;
    char  output[512];
    char  label[128];
    char  vm_id_arg[128];
    char  mode[32];     /* "native-stream" (default) | "native-smoke" | "health-check" */
    bool  activity;
    bool  quiet;
    /* A2 2026-05-18: thresholds for health-check mode (x100 for the ratios) */
    int   threshold_fps;        /* min frames/sec */
    int   threshold_decrypt;    /* min decrypt % (= ok/(ok+fail)) */
    int   threshold_bottom;     /* min NAL bottom ratio % (= bottom/(top+bottom)) */
    /* Q1 2026-05-18 : quality params override (0 = use desktop defaults) */
    int   bitrate_mbps;
    float fps_target;
    int   width;
    int   height;
} cli_args_t;

static void parse_args(int argc, char **argv, cli_args_t *a) {
    a->duration_sec = 60;
    snprintf(a->output, sizeof(a->output), "-");
    a->label[0] = 0;
    a->vm_id_arg[0] = 0;
    /* S112 - the default mode used to be "webrtc", a stack that has not been
     * compiled since the May pivot: it only ran against empty stubs, and a
     * measurement launched without `--mode` measured nothing. The default is now
     * the only path that exists. */
    snprintf(a->mode, sizeof(a->mode), "native-stream");
    a->activity = false;
    a->quiet = false;
    a->threshold_fps = 20;
    a->threshold_decrypt = 95;
    a->threshold_bottom = 30;
    a->bitrate_mbps = 0;
    a->fps_target = 0;
    a->width = 0;
    a->height = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = argv[i];
        if (strncmp(v, "--duration=", 11) == 0) a->duration_sec = atoi(v + 11);
        else if (strncmp(v, "--output=", 9) == 0) snprintf(a->output, sizeof(a->output), "%s", v + 9);
        else if (strncmp(v, "--label=", 8) == 0) snprintf(a->label, sizeof(a->label), "%s", v + 8);
        else if (strncmp(v, "--vm-id=", 8) == 0) snprintf(a->vm_id_arg, sizeof(a->vm_id_arg), "%s", v + 8);
        else if (strncmp(v, "--mode=", 7) == 0) snprintf(a->mode, sizeof(a->mode), "%s", v + 7);
        else if (strncmp(v, "--threshold-fps=", 16) == 0) a->threshold_fps = atoi(v + 16);
        else if (strncmp(v, "--threshold-decrypt=", 20) == 0) a->threshold_decrypt = atoi(v + 20);
        else if (strncmp(v, "--threshold-bottom=", 19) == 0) a->threshold_bottom = atoi(v + 19);
        else if (strncmp(v, "--bitrate=", 10) == 0) a->bitrate_mbps = atoi(v + 10);
        else if (strncmp(v, "--fps=", 6) == 0) a->fps_target = (float)atof(v + 6);
        else if (strncmp(v, "--width=", 8) == 0) a->width = atoi(v + 8);
        else if (strncmp(v, "--height=", 9) == 0) a->height = atoi(v + 9);
        else if (strcmp(v, "--activity") == 0) a->activity = true;
        else if (strcmp(v, "--quiet") == 0) a->quiet = true;
        else if (isdigit((unsigned char)v[0])) a->duration_sec = atoi(v);  /* legacy positional */
    }
    if (a->duration_sec < 5) a->duration_sec = 60;
    /* health-check default = 30s rapide */
    if (strcmp(a->mode, "health-check") == 0 && a->duration_sec == 60) {
        a->duration_sec = 30;
    }
}

#define LOG(fmt, ...) do { \
    if (!g_args.quiet) { \
        fprintf(stderr, "[test] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
    } \
} while(0)

static cli_args_t g_args;

/* === metric sampling === */

#define MAX_SAMPLES 7200   /* 2h @ 1Hz */
typedef struct {
    long      ts_ms;
    int       elapsed_s;
    uint64_t  video_bytes;
    uint64_t  audio_bytes;
    uint32_t  video_packets;
    uint32_t  audio_packets;
    uint32_t  frames_decoded;
    uint32_t  decode_errors;
    int       width;
    int       height;
    int       stuck_secs;
} sample_t;

static sample_t g_samples[MAX_SAMPLES];
static int      g_n_samples = 0;

static long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

/* Compute aggregate metrics over the sample series and dump JSON. */
static void dump_metrics(FILE *out, double bootstrap_ms, bool stream_started, int exit_reason) {
    /* Compute per-second video/audio bitrate (kbps) from cumulative bytes. */
    double video_kbps[MAX_SAMPLES], audio_kbps[MAX_SAMPLES];
    int n_kbps = 0;
    for (int i = 1; i < g_n_samples; i++) {
        long dt_ms = g_samples[i].ts_ms - g_samples[i-1].ts_ms;
        if (dt_ms <= 0) continue;
        long dvb = (long)(g_samples[i].video_bytes - g_samples[i-1].video_bytes);
        long dab = (long)(g_samples[i].audio_bytes - g_samples[i-1].audio_bytes);
        video_kbps[n_kbps] = (dvb * 8.0) / dt_ms;  /* bits/ms = kbps */
        audio_kbps[n_kbps] = (dab * 8.0) / dt_ms;
        n_kbps++;
    }
    double v_avg = 0, v_peak = 0, v_p50 = 0, v_p95 = 0;
    double a_avg = 0, a_peak = 0;
    if (n_kbps > 0) {
        for (int i = 0; i < n_kbps; i++) {
            v_avg += video_kbps[i];
            a_avg += audio_kbps[i];
            if (video_kbps[i] > v_peak) v_peak = video_kbps[i];
            if (audio_kbps[i] > a_peak) a_peak = audio_kbps[i];
        }
        v_avg /= n_kbps;
        a_avg /= n_kbps;
        double sorted[MAX_SAMPLES];
        memcpy(sorted, video_kbps, sizeof(double) * n_kbps);
        qsort(sorted, n_kbps, sizeof(double), cmp_double);
        v_p50 = sorted[n_kbps / 2];
        v_p95 = sorted[(int)(n_kbps * 0.95)];
    }

    int last = g_n_samples - 1;
    uint32_t total_v_pkts = (last >= 0) ? g_samples[last].video_packets : 0;
    uint32_t total_a_pkts = (last >= 0) ? g_samples[last].audio_packets : 0;
    uint64_t total_v_bytes = (last >= 0) ? g_samples[last].video_bytes : 0;
    uint64_t total_a_bytes = (last >= 0) ? g_samples[last].audio_bytes : 0;
    uint32_t total_frames = (last >= 0) ? g_samples[last].frames_decoded : 0;
    int last_w = (last >= 0) ? g_samples[last].width : 0;
    int last_h = (last >= 0) ? g_samples[last].height : 0;
    int max_stuck = 0;
    for (int i = 0; i < g_n_samples; i++) {
        if (g_samples[i].stuck_secs > max_stuck) max_stuck = g_samples[i].stuck_secs;
    }

    fprintf(out, "{\n");
    fprintf(out, "  \"label\": \"%s\",\n", g_args.label);
    fprintf(out, "  \"duration_requested_s\": %d,\n", g_args.duration_sec);
    fprintf(out, "  \"activity_enabled\": %s,\n", g_args.activity ? "true" : "false");
    fprintf(out, "  \"bootstrap_ms\": %.0f,\n", bootstrap_ms);
    fprintf(out, "  \"stream_started\": %s,\n", stream_started ? "true" : "false");
    fprintf(out, "  \"exit_reason\": %d,\n", exit_reason);
    fprintf(out, "  \"samples\": %d,\n", g_n_samples);
    fprintf(out, "  \"video\": {\n");
    fprintf(out, "    \"avg_kbps\": %.1f,\n", v_avg);
    fprintf(out, "    \"peak_kbps\": %.1f,\n", v_peak);
    fprintf(out, "    \"p50_kbps\": %.1f,\n", v_p50);
    fprintf(out, "    \"p95_kbps\": %.1f,\n", v_p95);
    fprintf(out, "    \"total_packets\": %u,\n", total_v_pkts);
    fprintf(out, "    \"total_bytes\": %lu,\n", (unsigned long)total_v_bytes);
    fprintf(out, "    \"frames_decoded\": %u,\n", total_frames);
    fprintf(out, "    \"resolution\": \"%dx%d\",\n", last_w, last_h);
    fprintf(out, "    \"max_stuck_secs\": %d\n", max_stuck);
    fprintf(out, "  },\n");
    fprintf(out, "  \"audio\": {\n");
    fprintf(out, "    \"avg_kbps\": %.1f,\n", a_avg);
    fprintf(out, "    \"peak_kbps\": %.1f,\n", a_peak);
    fprintf(out, "    \"total_packets\": %u,\n", total_a_pkts);
    fprintf(out, "    \"total_bytes\": %lu\n", (unsigned long)total_a_bytes);
    fprintf(out, "  },\n");
    fprintf(out, "  \"timeline\": [\n");
    for (int i = 0; i < g_n_samples; i++) {
        fprintf(out, "    {\"t\":%d,\"v_kbps\":%.1f,\"a_kbps\":%.1f,\"frames\":%u,\"stuck\":%d}%s\n",
                g_samples[i].elapsed_s,
                (i > 0 && g_samples[i].ts_ms > g_samples[i-1].ts_ms)
                    ? ((double)(g_samples[i].video_bytes - g_samples[i-1].video_bytes) * 8.0) /
                      (g_samples[i].ts_ms - g_samples[i-1].ts_ms)
                    : 0.0,
                (i > 0 && g_samples[i].ts_ms > g_samples[i-1].ts_ms)
                    ? ((double)(g_samples[i].audio_bytes - g_samples[i-1].audio_bytes) * 8.0) /
                      (g_samples[i].ts_ms - g_samples[i-1].ts_ms)
                    : 0.0,
                g_samples[i].frames_decoded,
                g_samples[i].stuck_secs,
                (i + 1 < g_n_samples) ? "," : "");
    }
    fprintf(out, "  ]\n");
    fprintf(out, "}\n");
}

/* K14 2026-08-21 - delete our clients at the end of the session, like the
 * official client (`DELETE /N/clients/{id}`). Without that they pile up on the VM:
 * the list returned at bootstrap showed 2 clients `"active": true` inherited
 * from the previous run. SHADOW_CLIENT_DELETE=0 to skip it. */
static void shadow_delete_clients(VmConnectionInfo *conn, ProximusCredentials *creds,
                                   ProximusMainSession *msess,
                                   ProximusLauncherSession *lsess) {
    static int g_del = -1;
    if (g_del < 0) {
        const char *e = getenv("SHADOW_CLIENT_DELETE");
        g_del = e ? atoi(e) : 1;
    }
    if (!g_del || !conn || !conn->proximus_url || !creds) return;
    long st = 0;
    if (msess && msess->id && creds->main_jwt) {
        bool ok = proximus_delete_client(conn->proximus_url, creds->main_jwt,
                                          msess->id, &st);
        LOG("[K14] DELETE client main -> HTTP %ld (%s)", st, ok ? "ok" : "echec");
    }
    if (lsess && lsess->id && creds->launcher_jwt) {
        bool ok = proximus_delete_client(conn->proximus_url, creds->launcher_jwt,
                                          lsess->id, &st);
        LOG("[K14] DELETE client launcher -> HTTP %ld (%s)", st, ok ? "ok" : "echec");
    }
}

int main(int argc, char *argv[]) {
    /* S112 - declared HERE and not further down: several `goto cleanup`s leave from
     * above, and the exit block reads it. Declaring it after them would leave it
     * uninitialised on those paths - an exit code drawn at random, which would
     * make every campaign that reads it lie. */
    int exit_reason = 2;

    parse_args(argc, argv, &g_args);
    LOG("halyard-cli starting, duration=%ds output=%s label=%s activity=%d",
        g_args.duration_sec, g_args.output, g_args.label, (int)g_args.activity);

    signal(SIGINT, on_sigint);
    /* A2 fix 2026-05-18: SIGALRM must set g_abort (= not the default kill).
     * Used by native-stream/health-check mode for the duration timeout. */
    signal(SIGALRM, on_sigint);
    long t_start_ms = now_ms();

    if (!http_global_init()) { LOG("http_global_init FAIL"); return 1; }

    char *rt = NULL;
    if (!oauth_load_refresh(&rt) || !rt) {
        LOG("oauth_load_refresh FAIL — token missing in %s", SHADOW_TOKEN_PATH);
        return 1;
    }

    OidcDiscovery disc = {0};
    long st = 0;
    if (!oauth_discover(&disc, &st)) {
        LOG("oauth_discover FAIL HTTP %ld", st); free(rt); return 1;
    }

    ShadowAuthState auth = {0};
    auth.refresh_token = strdup(rt);
    free(rt);
    if (!oauth_refresh(&disc, SHADOW_OAUTH_CLIENT_ID, &auth)) {
        LOG("oauth_refresh FAIL"); oauth_state_free(&auth); oauth_discovery_free(&disc); return 1;
    }
    oauth_save_refresh(&auth);
    oauth_discovery_free(&disc);
    LOG("oauth_refresh OK");
    /* Decode the access_token JWT to see the identity. */
    if (auth.access_token) {
        const char *p = strchr(auth.access_token, '.');
        const char *p2 = p ? strchr(p + 1, '.') : NULL;
        if (p && p2) {
            size_t plen = (size_t)(p2 - p - 1);
            char b64[4200] = {0};
            memcpy(b64, p + 1, plen > 4096 ? 4096 : plen);
            for (size_t i = 0; i < plen && i < 4096; i++) {
                if (b64[i] == '-') b64[i] = '+';
                else if (b64[i] == '_') b64[i] = '/';
            }
            while (strlen(b64) % 4) strcat(b64, "=");
            FILE *pi = popen("base64 -d 2>/dev/null", "w");
            if (pi) {
                FILE *po = popen("base64 -d 2>/dev/null", "r");  /* dummy, just for env */
                fwrite(b64, 1, strlen(b64), pi); pclose(pi); if (po) pclose(po);
            }
            /* Plus simple : appel direct base64 -d. */
            char cmd[5000];
            snprintf(cmd, sizeof(cmd), "echo '%s' | base64 -d 2>/dev/null", b64);
            FILE *f = popen(cmd, "r");
            if (f) {
                char json[4096] = {0};
                fread(json, 1, sizeof(json) - 1, f);
                pclose(f);
                LOG("access_token JWT payload: %.500s", json);
            }
        }
    }

    /* Starts telemetry - matches the browser bootstrap exactly. */
    telemetry_start(auth.access_token);

    GapInfo gi = {0};
    if (!tinag_get_datacenter("test@example.com", &gi, &st)) {
        LOG("TINAG FAIL HTTP %ld", st); oauth_state_free(&auth); return 1;
    }
    LOG("TINAG OK launcher_api_url=%s", gi.launcher_api_url);
    const char *launcher_base = gi.launcher_api_url;

    char vm_id[128] = {0};
    if (g_args.vm_id_arg[0]) {
        snprintf(vm_id, sizeof(vm_id), "%s", g_args.vm_id_arg);
        LOG("using VM from --vm-id=%s", vm_id);
    } else {
        VmPage vms = {0};
        if (!launcher_list_vms(launcher_base, auth.access_token, 0, 10, &vms, &st)) {
            LOG("launcher_list_vms FAIL HTTP %ld", st); oauth_state_free(&auth); return 1;
        }
        if (vms.count == 0) {
            LOG("No VMs available"); vmpage_free(&vms); oauth_state_free(&auth); return 2;
        }
        snprintf(vm_id, sizeof(vm_id), "%s", vms.items[0].id);
        LOG("picked VM: id=%s state=%s", vm_id, vms.items[0].state ? vms.items[0].state : "?");
        vmpage_free(&vms);
    }

    long http = 0;
    if (!launcher_start_vm(launcher_base, auth.access_token, vm_id, &http)) {
        LOG("launcher_start_vm FAIL HTTP %ld", http); oauth_state_free(&auth); return 1;
    }

    VmConnectionInfo conn = {0};
    bool got_conn = false;
    for (int i = 0; i < 30 && !g_abort; i++) {
        long stip = 0;
        if (launcher_get_vm_ip(launcher_base, auth.access_token, vm_id, &conn, &stip)
            && conn.ip && conn.port) { got_conn = true; break; }
        vmconn_free(&conn);
        sleep(2);
    }
    if (!got_conn) { LOG("VM IP never available"); oauth_state_free(&auth); return 1; }
    LOG("VM IP=%s:%s", conn.ip, conn.port);

    LauncherSessionToken tok = {0};
    if (!launcher_auth_login(launcher_base, auth.access_token, vm_id, &tok, &st)) {
        LOG("auth_login FAIL HTTP %ld", st);
        vmconn_free(&conn); oauth_state_free(&auth); return 1;
    }

    ProximusCredentials creds = {0};
    if (!launcher_proximus_credentials(launcher_base, auth.access_token, vm_id, &creds, &st)) {
        LOG("proximus_credentials FAIL HTTP %ld", st);
        launcher_token_free(&tok); vmconn_free(&conn); oauth_state_free(&auth); return 1;
    }

    /* K14 2026-08-21 - list the clients BEFORE creating ours.
     * The official client does this GET, then a DELETE of its own client at the
     * end of the session. We never deleted ours: after dozens of test sessions
     * the VM may have accumulated phantom clients, and a newcomer facing
     * already-registered clients could be treated as secondary - which would fit
     * "the server tracks our cursor but injects nothing" (KB §3.21). */
    {
        char *lst = NULL; long lst_st = 0;
        if (proximus_list_clients(conn.proximus_url, creds.main_jwt, &lst, &lst_st) && lst) {
            LOG("[K14] GET /clients HTTP %ld : %.700s", lst_st, lst);
            free(lst);
        } else {
            LOG("[K14] GET /clients HTTP %ld (failed)", lst_st);
        }
    }

    /* AF13 2026-09-10 - declared BEFORE the `goto cleanup` below: that goto
     * jumped over its initialisation, and the cleanup's
     * `shadow_delete_clients(&msess)` then freed indeterminate pointers every
     * time the launcher client could not be created. */
    ProximusMainSession msess = {0};
    ProximusLauncherSession lsess = {0};
    if (!proximus_create_launcher_client(conn.proximus_url, creds.launcher_jwt, &lsess, &st)) {
        LOG("create_launcher_client FAIL HTTP %ld", st); goto cleanup;
    }
    if (!proximus_create_main_client(conn.proximus_url, creds.main_jwt, &msess, &st)) {
        LOG("create_main_client FAIL HTTP %ld", st); goto cleanup;
    }

    /* P1 (2026-05-08): 2 simultaneous SSE, like the official desktop app (the
     * pcap confirms it: the Electron launcher pid and the ShadowPCDisplay pid
     * each have their own /N/stream long-poll). Test whether this unblocks the
     * :13011 binding. */
    proximus_sse_keepalive *sse_ka  = proximus_sse_start(conn.proximus_url, creds.launcher_jwt);
    proximus_sse_keepalive *sse_ka2 = proximus_sse_start(conn.proximus_url, creds.main_jwt);
    LOG("=== Dual SSE started : launcher + main ===");

    /* === native-smoke mode: native protocol bootstrap test (not WebRTC) === */
    if (strcmp(g_args.mode, "native-smoke") == 0) {
        extern bool streaming_smoke_test_m32(const char *vm_host,
                                              const char *streaming_token,
                                              const char *client_id,
                                              const char *bearer_jwt);
        LOG("=== NATIVE SMOKE MODE — calling streaming_smoke_test_m32 ===");
        bool ok = streaming_smoke_test_m32(conn.ip, msess.streaming_token,
                                            msess.id, creds.main_jwt);
        LOG("=== NATIVE SMOKE result = %s ===", ok ? "OK" : "FAIL");
        if (sse_ka) proximus_sse_stop(sse_ka);
    if (sse_ka2) proximus_sse_stop(sse_ka2);
        proximus_main_session_free(&msess);
        proximus_launcher_session_free(&lsess);
        proximus_credentials_free(&creds);
        launcher_token_free(&tok);
        vmconn_free(&conn);
        oauth_state_free(&auth);
        telemetry_stop();          /* AF13 */
        http_global_cleanup();
        journal_close();
        return ok ? 0 : 2;
    }

    /* === Mode native-stream OU health-check === */
    bool is_native_stream = (strcmp(g_args.mode, "native-stream") == 0);
    bool is_health_check  = (strcmp(g_args.mode, "health-check")  == 0);
    if (is_native_stream || is_health_check) {
        LOG("=== %s MODE — full pipeline (ctrl_session + h264) ===",
            is_health_check ? "HEALTH-CHECK" : "NATIVE STREAM");

        /* Compute port_base from vm.port (= formula observed 2026-05-09: +7000). */
        int port_base = 0;
        if (conn.port) {
            int vm_port = atoi(conn.port);
            if (vm_port > 0) port_base = vm_port + 7000;
        }
        if (port_base <= 0) {
            LOG("=== NATIVE: port_base invalid (conn.port=%s) ===", conn.port ? conn.port : "?");
            if (sse_ka)  proximus_sse_stop(sse_ka);
            if (sse_ka2) proximus_sse_stop(sse_ka2);
            proximus_main_session_free(&msess);
            proximus_launcher_session_free(&lsess);
            proximus_credentials_free(&creds);
            launcher_token_free(&tok);
            vmconn_free(&conn);
            oauth_state_free(&auth);
            telemetry_stop();          /* AF13 */
            http_global_cleanup();
            journal_close();
            return 1;
        }
        LOG("=== NATIVE: port_base=%d (= vm.port=%s + 7000) ===", port_base, conn.port);

        ctrl_session_glue_params np = {0};
        np.vm_host         = conn.ip;
        np.streaming_token = msess.streaming_token;
        np.client_id       = msess.id;
        np.bearer_jwt      = creds.main_jwt;
        np.display_width   = g_args.width  > 0 ? g_args.width  : 1920;
        np.display_height  = g_args.height > 0 ? g_args.height : 1080;
        np.port_base       = port_base;
        np.max_bitrate_mbps = (uint32_t)g_args.bitrate_mbps;  /* Q1 2026-05-18 */
        np.target_fps       = g_args.fps_target;
        np.abort_flag      = (volatile int *)&g_abort;

        ctrl_session_glue_stats nstats = {0};
        long t_native_start = now_ms();
        double native_bootstrap_ms = (double)(t_native_start - t_start_ms);

        alarm((unsigned int)g_args.duration_sec);
        bool ok = ctrl_session_glue_run(&np, &nstats);
        alarm(0);

        /* Derived metrics */
        double fps = (nstats.session_seconds > 0)
                     ? ((double)nstats.frames_decoded / nstats.session_seconds) : 0.0;
        uint32_t decrypt_total = nstats.decrypt_ok + nstats.decrypt_fail;
        double decrypt_pct = (decrypt_total > 0)
                             ? (100.0 * nstats.decrypt_ok / decrypt_total) : 0.0;
        uint32_t nal_total = nstats.nal_top + nstats.nal_bottom;
        double bottom_pct = (nal_total > 0)
                            ? (100.0 * nstats.nal_bottom / nal_total) : 0.0;
        double avg_kbps = (nstats.session_seconds > 0)
                          ? (nstats.udp_video_bytes * 8.0 / 1000.0 / nstats.session_seconds) : 0.0;

        LOG("=== NATIVE result : ok=%d bootstrap=%d frames=%u disp=%u "
            "fps=%.1f decrypt=%.1f%% (%u/%u) NAL top=%u bot=%u (bot=%.1f%%) "
            "avg_kbps=%.1f sec=%d ===",
            ok, nstats.bootstrap_ok, nstats.frames_decoded, nstats.frames_displayed,
            fps, decrypt_pct, nstats.decrypt_ok, decrypt_total,
            nstats.nal_top, nstats.nal_bottom, bottom_pct,
            avg_kbps, nstats.session_seconds);

        /* === A2 health-check verdict === */
        bool pass = true;
        const char *fail_reason = "";
        if (is_health_check) {
            if (!nstats.bootstrap_ok) { pass = false; fail_reason = "bootstrap_ko"; }
            else if (fps < g_args.threshold_fps) { pass = false; fail_reason = "fps_low"; }
            else if (decrypt_pct < g_args.threshold_decrypt) { pass = false; fail_reason = "decrypt_low"; }
            else if (bottom_pct < g_args.threshold_bottom) { pass = false; fail_reason = "bottom_low"; }
            LOG("=== HEALTH-CHECK: %s%s%s ===",
                pass ? "PASS" : "FAIL",
                pass ? "" : " (reason=", pass ? "" : fail_reason);
            if (!pass) LOG("=== HEALTH-CHECK fail thresholds : fps≥%d? %.1f | decrypt≥%d? %.1f | bottom≥%d? %.1f ===",
                            g_args.threshold_fps, fps,
                            g_args.threshold_decrypt, decrypt_pct,
                            g_args.threshold_bottom, bottom_pct);
        }

        /* === A1 JSON dump natif === */
        {
            FILE *out = stdout;
            if (g_args.output[0] && strcmp(g_args.output, "-") != 0) {
                out = fopen(g_args.output, "w");
                if (!out) { LOG("Cannot open %s for write", g_args.output); out = stdout; }
            }
            fprintf(out, "{\n");
            fprintf(out, "  \"label\": \"%s\",\n", g_args.label);
            fprintf(out, "  \"mode\": \"%s\",\n", g_args.mode);
            fprintf(out, "  \"duration_requested_s\": %d,\n", g_args.duration_sec);
            fprintf(out, "  \"bootstrap_ms\": %.0f,\n", native_bootstrap_ms);
            fprintf(out, "  \"bootstrap_ok\": %s,\n", nstats.bootstrap_ok ? "true" : "false");
            fprintf(out, "  \"stream_started\": %s,\n", (nstats.frames_decoded > 0) ? "true" : "false");
            fprintf(out, "  \"exit_reason\": %d,\n", nstats.exit_reason);
            fprintf(out, "  \"session_seconds\": %d,\n", nstats.session_seconds);
            fprintf(out, "  \"video\": {\n");
            fprintf(out, "    \"avg_kbps\": %.1f,\n", avg_kbps);
            fprintf(out, "    \"total_packets\": %u,\n", nstats.udp_video_pkts);
            fprintf(out, "    \"total_bytes\": %lu,\n", (unsigned long)nstats.udp_video_bytes);
            fprintf(out, "    \"frames_decoded\": %u,\n", nstats.frames_decoded);
            fprintf(out, "    \"frames_displayed\": %u,\n", nstats.frames_displayed);
            fprintf(out, "    \"fps\": %.2f,\n", fps);
            fprintf(out, "    \"resolution\": \"%dx%d\"\n", np.display_width, np.display_height);
            fprintf(out, "  },\n");
            fprintf(out, "  \"crypto\": {\n");
            fprintf(out, "    \"decrypt_ok\": %u,\n", nstats.decrypt_ok);
            fprintf(out, "    \"decrypt_fail\": %u,\n", nstats.decrypt_fail);
            fprintf(out, "    \"decrypt_pct\": %.2f\n", decrypt_pct);
            fprintf(out, "  },\n");
            fprintf(out, "  \"nal\": {\n");
            fprintf(out, "    \"top\": %u,\n", nstats.nal_top);
            fprintf(out, "    \"bottom\": %u,\n", nstats.nal_bottom);
            fprintf(out, "    \"idr_top\": %u,\n", nstats.nal_idr_top);
            fprintf(out, "    \"idr_bottom\": %u,\n", nstats.nal_idr_bottom);
            fprintf(out, "    \"bottom_pct\": %.2f\n", bottom_pct);
            fprintf(out, "  },\n");
            fprintf(out, "  \"reasm\": {\n");
            fprintf(out, "    \"parity_skip\": %u,\n", nstats.parity_skip);
            fprintf(out, "    \"parity_decrypt_ok\": %u,\n", nstats.parity_decrypt_ok);
            fprintf(out, "    \"parity_decrypt_fail\": %u,\n", nstats.parity_decrypt_fail);
            fprintf(out, "    \"abandoned\": %u\n", nstats.reasm_abandoned);
            fprintf(out, "  },\n");
            /* === V10 2026-08-28 - WHAT WAS MISSING TO MEASURE THE LOSS ===
             * The JSON only carried `abandoned`, which stays at zero on a clean
             * link: no way to read an A/B of the retransmission from it. These
             * counters all existed in ctrl_session_stats and simply were not
             * exposed - so bench_runner.py compared sessions on numbers that
             * never moved. */
            fprintf(out, "  \"perte\": {\n");
            fprintf(out, "    \"chunks_missing\": %u,\n", nstats.chunks_missing);
            fprintf(out, "    \"chunks_expected\": %u,\n", nstats.chunks_expected);
            fprintf(out, "    \"frames_miss1\": %u,\n", nstats.frames_miss1);
            fprintf(out, "    \"chunks_orphan\": %u,\n", nstats.chunks_orphan);
            fprintf(out, "    \"chunks_orphan_dup\": %u,\n", nstats.chunks_orphan_dup);
            fprintf(out, "    \"chunks_orphan_lost\": %u,\n", nstats.chunks_orphan_lost);
            fprintf(out, "    \"chunks_orphan_stale\": %u,\n", nstats.chunks_orphan_stale);
            fprintf(out, "    \"frames_dropped_trunc\": %u,\n", nstats.frames_dropped_trunc);
            fprintf(out, "    \"incomplete_at_flush\": %u,\n", nstats.incomplete_at_flush);
            fprintf(out, "    \"nack_sent\": %u,\n", nstats.nack_sent);
            fprintf(out, "    \"chunks_redundant\": %u,\n", nstats.chunks_redundant);
            fprintf(out, "    \"paquets_courts_base10\": %u\n", g_vid_short_pkts);
            fprintf(out, "  }");
            if (is_health_check) {
                fprintf(out, ",\n  \"health\": {\n");
                fprintf(out, "    \"pass\": %s,\n", pass ? "true" : "false");
                fprintf(out, "    \"reason\": \"%s\",\n", pass ? "ok" : fail_reason);
                fprintf(out, "    \"threshold_fps\": %d,\n", g_args.threshold_fps);
                fprintf(out, "    \"threshold_decrypt\": %d,\n", g_args.threshold_decrypt);
                fprintf(out, "    \"threshold_bottom\": %d\n", g_args.threshold_bottom);
                fprintf(out, "  }\n");
            } else {
                fprintf(out, "\n");
            }
            fprintf(out, "}\n");
            if (out != stdout) fclose(out);
            LOG("metrics dumped to %s", g_args.output);
        }

        if (sse_ka)  proximus_sse_stop(sse_ka);
        if (sse_ka2) proximus_sse_stop(sse_ka2);
        /* K14 - delete our clients, like the official client. native-stream mode
         * exits here, not through the `cleanup:` label. */
        shadow_delete_clients(&conn, &creds, &msess, &lsess);
        proximus_main_session_free(&msess);
        proximus_launcher_session_free(&lsess);
        proximus_credentials_free(&creds);
        launcher_token_free(&tok);
        vmconn_free(&conn);
        oauth_state_free(&auth);
        telemetry_stop();          /* AF13 */
        http_global_cleanup();
        journal_close();
        if (is_health_check) return pass ? 0 : 6;
        return ok ? 0 : 2;
    }

    /* === S112 2026-08-29 - THE "webrtc" MODE IS GONE ===
     *
     * A hundred and forty lines here opened an in-house WebRTC session: building
     * the signalling URL, a blocking thread, statistics sampling. They served
     * ONLY this binary - the stack has not been compiled into the application
     * since the May 2026 pivot - and yet that mode was the DEFAULT: a measurement
     * launched without `--mode` went down a path the product does not take.
     *
     * The three modes that remain are the ones we actually measure. An unknown
     * mode says so rather than falling into a default path: that is what stops a
     * typo from returning a result someone will believe. */
    fprintf(stderr, "unknown mode: %s\n"
                    "available modes: native-stream, native-smoke, health-check\n",
            g_args.mode);

cleanup:
    shadow_delete_clients(&conn, &creds, &msess, &lsess);
    proximus_main_session_free(&msess);
    proximus_launcher_session_free(&lsess);
    proximus_credentials_free(&creds);
    launcher_token_free(&tok);
    vmconn_free(&conn);
    gapinfo_free(&gi);
    oauth_state_free(&auth);
    /* AF13 - main.cpp's order: the telemetry thread first (it may be in the
     * middle of a POST), then curl, then the log LAST so its own line gets out.
     * `http_global_cleanup` was missing here, and the early returns above had
     * it WITHOUT stopping the telemetry thread first - freeing curl under a
     * live transfer, undefined by libcurl's contract. */
    telemetry_stop();
    http_global_cleanup();

    LOG("test done (exit_reason=%d)", exit_reason);
    journal_close();
    return exit_reason;
}
