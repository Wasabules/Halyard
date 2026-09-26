// TINAG ("This Is Not A Game") - Shadow's data-centre routing service.
// Only one route is used: GET /datacenter?email=<user_email>
#pragma once
#include <stdbool.h>

typedef struct {
    char *gap_url;             // URL of the GAP portal (account)
    char *name;                // human-readable data-centre name (e.g. "Europe")
    char *speed_test_url;      // speedtest endpoint
    char *launcher_api_url;    // base URL for LauncherApiService - the key to everything that follows!
    char *launcher_api_version;
} GapInfo;

// Frees the allocated fields. Safe to call on a zero-initialised GapInfo.
void gapinfo_free(GapInfo *g);

// Calls TINAG. `email` may be NULL (equivalent to an empty string server-side;
// it generally returns the default EU data centre).
// Returns true when the request succeeded AND the required fields (at minimum
// launcher_api_url) are present.
// `http_status` receives the returned HTTP code (useful to tell a 4xx/5xx from a
// network error).
bool tinag_get_datacenter(const char *email, GapInfo *out, long *http_status);
