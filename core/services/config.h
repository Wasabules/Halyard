// Constants extracted from Shadow PC v3.33.2 (cf. 03-shadow-recon/PROTOCOL.md).
// These values are the public Android credentials found in
// com.blade.shadowcloudgaming.
#pragma once

// === Environment (prod by default, switchable through SHADOW_ENV) ===
#ifndef SHADOW_ENV
#define SHADOW_ENV_PROD
#endif

#ifdef SHADOW_ENV_PROD
#  define SHADOW_TINAG_BASE   "https://tinag.shadow.tech/"
#  define SHADOW_OIDC_DISCOVERY "https://auth.eu.shadow.tech/hydra/.well-known/openid-configuration"
/* OAuth client_id: 0c6ee748-... = the Linux desktop renderer (LD_PRELOAD
 * capture, 2026-05-06). We used to use the Android client_id `7aa43568-...`,
 * which made the server treat us as an APK client and blocked :13011. */
#  define SHADOW_OAUTH_CLIENT_ID "0c6ee748-5352-412c-944f-947e15df8bf0"
#  define SHADOW_LOGS_BASE    "https://api.eu.shadow.tech/v1/pu/virtual-desktop/"
#  define SHADOW_API_V1       "https://api.eu.shadow.tech/v1/"
#  define SHADOW_TELEMETRY_URL "https://prod.log.frsbg01.shadow.tech:2443/client"
#elif defined(SHADOW_ENV_PREPROD)
#  define SHADOW_TINAG_BASE   "https://tinag.shadow-preprod.tech/"
#  define SHADOW_OIDC_DISCOVERY "https://auth.eu.shadow-preprod.tech/hydra/.well-known/openid-configuration"
#  define SHADOW_OAUTH_CLIENT_ID "6b85cf59-047b-43f4-9bb7-f327ccdef507"
#  define SHADOW_LOGS_BASE    "https://api.eu.shadow-preprod.tech/v1/pu/virtual-desktop/"
#  define SHADOW_API_V1       "https://api.eu.shadow-preprod.tech/v1/"
#  define SHADOW_TELEMETRY_URL "https://preprod.log.frsbg01.shadow-preprod.tech:2443/client"
#elif defined(SHADOW_ENV_DEV)
#  define SHADOW_TINAG_BASE   "https://tinag.spectr.be/"
#  define SHADOW_OIDC_DISCOVERY "https://auth.spectr.be/hydra/.well-known/openid-configuration"
#  define SHADOW_OAUTH_CLIENT_ID "15701f0e-7463-426c-bb97-aef71d618bcd"
#  define SHADOW_LOGS_BASE    "https://api.spectr.be/v1/pu/virtual-desktop/"
#  define SHADOW_API_V1       "https://api.spectr.be/v1/"
#  define SHADOW_TELEMETRY_URL "https://dev.log.frsbg01.spectr.be:2443/client"
#endif

// Versions exposed to the launcher.status telemetry and to the opaque metadata.
#define SHADOW_LAUNCHER_VERSION "1.15.1"
#define SHADOW_RENDERER_VERSION "0.13.0"

#define SHADOW_OAUTH_SCOPE "openid email vm_access api profile offline"

/* User-Agent: LD_PRELOAD capture of the official desktop app, 2026-05-06:
 *   "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3"
 * (NOT a Chrome UA - that was misleading, it made them treat us as a web
 * client.) */
#define SHADOW_USER_AGENT "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3"

// Data dir: refresh_token, device.uuid, debug traces, log rotation.
//   - Switch  : /switch/halyard/   (SD card)
//   - PS Vita : ux0:data/halyard/  (the memory card / internal storage)
//   - Linux   : /tmp/halyard/      (ephemeral - bench / test-cli)
//   - Windows : ./halyard-data/    (relative to the CWD, created on the
//                                         first run)
// If you move the Windows binary elsewhere, the directory follows the CWD.
// Override with a build define: -DSHADOW_DATA_DIR=\"C:/path/of/yours/\".
//
// ON A CONSOLE THIS DIRECTORY IS THE INTERFACE, so getting it wrong costs more
// than a missing file. `env.txt` is the only way to set a toggle where there is
// no shell, `logsink.txt` is the only way to see a log at all, and
// `refresh_token` is the only way not to pair again at every launch. The Vita
// fell into the `#else` and looked in `/tmp/halyard/`, which does not
// exist there: the first .vpk could write nothing and report nothing.
#ifndef SHADOW_DATA_DIR
#  if defined(__SWITCH__)
#    define SHADOW_DATA_DIR "/switch/halyard/"
#  elif defined(__vita__) || defined(__psp2__)
#    define SHADOW_DATA_DIR "ux0:data/halyard/"
#  elif defined(_WIN32)
#    define SHADOW_DATA_DIR "./halyard-data/"
#  elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#    define SHADOW_DATA_DIR "/tmp/halyard/"
#  else
#    error "no data directory for this platform - see config.h"
#  endif
#endif

#define SHADOW_TOKEN_PATH        SHADOW_DATA_DIR "refresh_token"
#define SHADOW_DEVICE_UUID_PATH  SHADOW_DATA_DIR "device.uuid"

/* X-Shadow-Agent - desktop app capture:
 * "Renderer-Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3"
 * Different from the standard UA: prefixed "Renderer-" for the main client. */
#define SHADOW_X_AGENT "Renderer-Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3"

// Origin / Referer of the web client. The Shadow server probably checks Origin
// against a whitelist (cf. the CORS replies
// access-control-allow-origin: https://pc.shadow.tech), and a Cloudflare proxy
// strips the X-* headers when Origin is not recognised.
#define SHADOW_ORIGIN  "https://pc.shadow.tech"
#define SHADOW_REFERER "https://pc.shadow.tech/"

// Network timeouts (secondes)
#define SHADOW_HTTP_CONNECT_TIMEOUT 15L
#define SHADOW_HTTP_TOTAL_TIMEOUT   30L
