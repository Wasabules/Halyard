#include "telemetry.h"
#include "config.h"
#include "http.h"
#include "launcher.h"   /* shadow_device_uuid_public */

#include <curl/curl.h>
#include <jansson.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__SWITCH__) && !defined(_WIN32)
#include <sys/utsname.h>
#endif
#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

extern const char *shadow_device_uuid_public(void);

/* Generates a pseudo-random UUID v4 for the `session` field. Not
 * cryptographically strong - srand(time)+rand() is enough, it is only a
 * session-lifetime identifier. */
static void gen_session_uuid(char *out, size_t cap) {
    static int seeded = 0;
    if (!seeded) { srand((unsigned)(time(NULL) ^ (long)pthread_self())); seeded = 1; }
    unsigned char b[16];
    for (int i = 0; i < 16; i++) b[i] = (unsigned char)(rand() & 0xff);
    b[6] = (b[6] & 0x0f) | 0x40;   /* version 4 */
    b[8] = (b[8] & 0x3f) | 0x80;   /* RFC 4122 variant */
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
             b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}

static char *g_session_uuid_alloc = NULL;
static const char *session_uuid(void) {
    if (!g_session_uuid_alloc) {
        char buf[40];
        gen_session_uuid(buf, sizeof(buf));
        g_session_uuid_alloc = strdup(buf);
    }
    return g_session_uuid_alloc;
}

static char *build_body(const char *event_name) {
    json_t *meta = json_object();
    json_object_set_new(meta, "user-uuid", json_string(shadow_device_uuid_public()));
    json_object_set_new(meta, "session",   json_string(session_uuid()));
    json_object_set_new(meta, "launcher-version", json_string(SHADOW_LAUNCHER_VERSION));
    json_object_set_new(meta, "renderer-version", json_string(SHADOW_RENDERER_VERSION));
    json_object_set_new(meta, "user-environment", json_string("prod"));

    /* We always report os-family="Linux" and the UA from the LD_PRELOAD capture
     * of the official desktop client. That is what the server expects; lying
     * about Windows would break the session. The arch may vary. */
    json_object_set_new(meta, "os-family",  json_string("Linux"));
    json_object_set_new(meta, "os-version", json_string(SHADOW_USER_AGENT));
#ifdef __SWITCH__
    json_object_set_new(meta, "arch",       json_string("aarch64"));
#elif defined(_WIN32)
    /* Win64, but we report x86_64 to stay aligned with the Linux UA. */
    json_object_set_new(meta, "arch",       json_string("x86_64"));
#else
    struct utsname un = {0};
    uname(&un);
    json_object_set_new(meta, "arch",       json_string(un.machine[0] ? un.machine : "x86_64"));
#endif

    json_t *root = json_object();
    json_object_set_new(root, "version",   json_integer(1));
    json_object_set_new(root, "name",      json_string(event_name));
    /* timestamp : ms since epoch */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    long long ts_ms = (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    json_object_set_new(root, "timestamp", json_integer(ts_ms));
    json_object_set_new(root, "privacy",   json_string("PUBLIC"));
    json_object_set_new(root, "metadata",  meta);
    char *s = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return s;
}

static bool post_telemetry(const char *bearer, const char *event_name) {
    char *body = build_body(event_name);
    if (!body) return false;
    http_response resp = {0};
    bool ok = http_post_json(SHADOW_TELEMETRY_URL, bearer, body, strlen(body), &resp);
    long st = resp.status;
    http_free(&resp);
    free(body);
    return ok && st >= 200 && st < 300;
}

bool telemetry_post_one(const char *bearer, const char *event_name) {
    return post_telemetry(bearer, event_name);
}

/* Background poster - emits launcher.status every 3 s while stop != true. */
static atomic_int  g_stop = 0;
static atomic_int  g_running = 0;
static pthread_t   g_thread;
static char       *g_bearer_copy = NULL;
/* If the endpoint refuses (self-signed SSL, 401, etc.) we stop, to avoid
 * spamming the logs every 3 s. Video throughput does not depend on it - it is
 * only an optional QoS signal. */
static atomic_int  g_disabled = 0;
static int         g_consecutive_failures = 0;

static void *poster_loop(void *arg) {
    (void)arg;
    /* A 100 ms tick so g_stop is observed quickly. Emits every 30 ticks (3 s). */
    int tick = 0;
    while (!atomic_load(&g_stop)) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (++tick >= 30) {
            tick = 0;
            if (atomic_load(&g_disabled)) continue;
            if (g_bearer_copy) {
                bool ok = post_telemetry(g_bearer_copy, "launcher.status");
                if (!ok) {
                    g_consecutive_failures++;
                    if (g_consecutive_failures >= 3) {
                        atomic_store(&g_disabled, 1);
                        fprintf(stderr,
                            "telemetry: disabled after 3 consecutive failures "
                            "(non-fatal, server cert/auth issue)\n");
                    }
                } else {
                    g_consecutive_failures = 0;
                }
            }
        }
    }
    return NULL;
}

bool telemetry_start(const char *bearer) {
    if (atomic_load(&g_running)) return true;
    if (!bearer) return false;
    free(g_bearer_copy);
    g_bearer_copy = strdup(bearer);
    if (!g_bearer_copy) return false;
    atomic_store(&g_stop, 0);
    if (pthread_create(&g_thread, NULL, poster_loop, NULL) != 0) {
        free(g_bearer_copy); g_bearer_copy = NULL;
        return false;
    }
    atomic_store(&g_running, 1);
    /* Emits a first event immediately, without waiting the 3 s. */
    (void)post_telemetry(g_bearer_copy, "launcher.status");
    return true;
}

void telemetry_stop(void) {
    if (!atomic_load(&g_running)) return;
    atomic_store(&g_stop, 1);
    pthread_join(g_thread, NULL);
    atomic_store(&g_running, 0);
    free(g_bearer_copy); g_bearer_copy = NULL;
    free(g_session_uuid_alloc); g_session_uuid_alloc = NULL;
}
