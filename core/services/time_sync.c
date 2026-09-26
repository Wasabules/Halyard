#include "time_sync.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static int64_t g_offset_ms = 0;
static bool    g_synced    = false;

static const char *MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";

static int parse_month(const char *m3) {
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(MONTHS + i*3, m3, 3) == 0) return i;
    }
    return -1;
}

// "Wed, 02 May 2026 18:39:05 GMT" → seconds since epoch. UTC.
// We avoid timegm() for portability across newlib variants.
static bool parse_imf_fixdate(const char *s, time_t *out) {
    if (!s) return false;
    int day = 0, year = 0, h = 0, m = 0, sec = 0;
    char mon[4] = {0};
    // Skip "Wed, "
    const char *p = strchr(s, ',');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    if (sscanf(p, "%d %3s %d %d:%d:%d", &day, mon, &year, &h, &m, &sec) != 6) return false;
    int month = parse_month(mon);
    if (month < 0 || day < 1 || day > 31 || year < 1970 || h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 60) return false;

    // days_from_civil: Howard Hinnant's algorithm.
    int y = (month <= 1) ? year - 1 : year;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (month + (month > 1 ? -2 : 10)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    *out = (time_t)(days * 86400LL + h * 3600 + m * 60 + sec);
    return true;
}

void time_sync_observe_http_date(const char *date_str) {
    if (!date_str || !*date_str) return;
    char buf[128];
    size_t n = strlen(date_str);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, date_str, n);
    buf[n] = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') { buf[i] = 0; break; }
    }
    time_t srv;
    if (!parse_imf_fixdate(buf, &srv)) return;

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return;
    int64_t local_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    int64_t srv_ms   = (int64_t)srv * 1000;
    int64_t new_offset = srv_ms - local_ms;

    bool first = !g_synced;
    g_offset_ms = new_offset;
    g_synced = true;
    if (first) {
        fprintf(stderr, "time_sync: first sync, offset = %lld ms (%.1f min)\n",
                (long long)new_offset, (double)new_offset / 60000.0);
    }
}

uint64_t time_now_ms_synced(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    int64_t local_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    int64_t synced   = local_ms + g_offset_ms;
    return (uint64_t)synced;
}

bool time_sync_ok(void) { return g_synced; }
int64_t time_sync_offset_ms(void) { return g_offset_ms; }
