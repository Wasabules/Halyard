// Telemetry POST -> https://prod.log.frsbg01.shadow.tech:2443/client
//
// The browser sends a `launcher.status` event every ~3 s for the whole session.
// JSON body:
//   {"version":1,"name":"launcher.status","timestamp":<ms>,"privacy":"PUBLIC",
//    "metadata":{"user-uuid":"<uuid>","session":"<uuid>","launcher-version":...,
//                "renderer-version":...,"user-environment":"prod","os-family":...,
//                "os-version":...,"arch":"x86_64",...}}
//
// Hypothesis: if the server-side QoS scoring tracks the absence of telemetry,
// sending it could unlock a better bitrate/quality preset. Risk: none
// (write-only, the server ignores the body when it is too verbose).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the poster in the background. Emits a launcher.status event every
// ~3 seconds until telemetry_stop(). The bearer must stay valid for the whole
// run (the caller frees it after the stop).
bool telemetry_start(const char *bearer);

// Stops the thread cleanly (join).
void telemetry_stop(void);

// Emits a one-shot event (synchronously). If the poster is not started, this
// becomes a direct POST on the caller's thread. Used for the milestones (login,
// vm_start, etc.).
bool telemetry_post_one(const char *bearer, const char *event_name);

#ifdef __cplusplus
}
#endif
