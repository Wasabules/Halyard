// Sync local clock against server time scraped from HTTP Date headers.
// The Switch HOS RTC may drift by minutes/hours; Shadow's anti-replay rejects
// stale timestamps. We observe Date: from every HTTPS response and maintain
// a single offset (server_ms - local_ms).

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Feed a raw `Date:` header value (without "Date: " prefix, may contain CRLF).
// Updates the offset if the date parses successfully. Idempotent.
void time_sync_observe_http_date(const char *date_str);

// Returns the current best-estimate epoch ms. Falls back to local
// CLOCK_REALTIME if no sync has occurred.
uint64_t time_now_ms_synced(void);

// True if at least one Date header has been observed.
bool time_sync_ok(void);

// Current offset in ms (server - local). Useful for logging.
int64_t time_sync_offset_ms(void);

#ifdef __cplusplus
}
#endif
