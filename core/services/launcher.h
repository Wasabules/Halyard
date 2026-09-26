// LauncherApiService - authenticated REST calls on the URL TINAG returned.
// Every route under "shadow/..." uses the Bearer access_token.

#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *id;          // identifiant interne (UUID)
    char *alias;       // user-customised name (may be NULL)
    char *name;        // model / plan name (may be NULL)
    char *state;       // "running" / "stopped" / etc. (may be NULL)
    char *raw_json;    // copy of the raw JSON, useful for debugging
} VmInfo;

void vminfo_free(VmInfo *v);

typedef struct {
    VmInfo *items;
    size_t  count;
    size_t  total;     // total count server-side (may exceed `count` when there are several pages)
    int     offset;
    int     limit;
} VmPage;

void vmpage_free(VmPage *p);

// GET <launcher_api_url>vms?offset=&limit=
bool launcher_list_vms(const char *launcher_base, const char *bearer,
                        int offset, int limit, VmPage *out, long *http_status);

// GET <launcher_api_url>vms/{id}
bool launcher_get_vm(const char *launcher_base, const char *bearer,
                     const char *vm_id, VmInfo *out, long *http_status);

// POST <launcher_api_url>shadow/vm/start (header X-Vm-Id)
bool launcher_start_vm(const char *launcher_base, const char *bearer,
                       const char *vm_id, long *http_status);

// GET <launcher_api_url>shadow/vm/ip (header X-Vm-Id) → ip + port + sessionId
typedef struct {
    char *ip;
    char *port;          // a string (parse to uint16 when needed)
    char *vm_session_id;
    char *messaging_url;
    char *proximus_url;  // base URL for /clients, /stream, /forward (M5)
    char *alias;
    char *provider;
    int   slot_number;   // slot number (used in proximus_url/<slot>/...)
} VmConnectionInfo;

void vmconn_free(VmConnectionInfo *c);

bool launcher_get_vm_ip(const char *launcher_base, const char *bearer,
                         const char *vm_id, VmConnectionInfo *out, long *http_status);

// POST <launcher_api_url>shadow/auth_login (body JSON quelconque, header X-Vm-Id) → Token
typedef struct { char *token; } LauncherSessionToken;
void launcher_token_free(LauncherSessionToken *t);

bool launcher_auth_login(const char *launcher_base, const char *bearer,
                         const char *vm_id, LauncherSessionToken *out, long *http_status);

// POST <launcher_api_url>shadow/vm/proximus-credentials -> 3 JWTs
// (launcher + main + usb), used to talk to the VM's host (proximus_url) on the
// streaming side.
typedef struct {
    char *launcher_jwt;  // Bearer for POST proximus_url/clients type=launcher
    char *main_jwt;      // Bearer for POST proximus_url/clients type=main
    char *usb_jwt;       // Bearer for POST proximus_url/devices (shadowusb USB-over-TCP)
} ProximusCredentials;

void proximus_credentials_free(ProximusCredentials *c);

bool launcher_proximus_credentials(const char *launcher_base, const char *bearer,
                                   const char *vm_id, ProximusCredentials *out, long *http_status);

// GET <launcher_api_url>vms/{vm_id}/capabilities (X-Vm-Id header)
// The browser calls this endpoint before /vm/start. Observed reply
// (HAR 2026-05-06): { "version":"1.0", "streaming":{"channels":{"video":
// {"codec":["h264","h265","av1"],"frame_rate":240,"max_resolution":...}}} }.
// We call it purely to match the browser bootstrap; the result is mostly
// informative (logged) - the server-side QoS scoring may track the absence of
// this call as a "legacy client" signal.
typedef struct {
    char *raw_json;       // copy of the parsed body, for debugging
    bool  video_allowed;
    int   max_frame_rate; // ex 240
    int   max_width;
    int   max_height;
    char *video_codecs;   // joined "h264,h265,av1"
    bool  audio_allowed;
    char *audio_codecs;   // joined "opus,flac"
} VmCapabilities;

void vmcaps_free(VmCapabilities *c);

bool launcher_get_capabilities(const char *launcher_base, const char *bearer,
                                const char *vm_id, VmCapabilities *out,
                                long *http_status);

// GET <launcher_api_url>shadow/turn-servers (X-Vm-Id header)
// Browser HAR reply: {"iceServers":[{"urls":"turn:host:443?transport=tcp",
//   "username":"<expiry-ts>:<vm_id>","credential":"<base64>"}, ...]}
// Nothing here uses a TURN relay - the native protocol connects to the VM
// directly. The request is kept because the official clients make it before
// /vm/start (see connecting_activity.cpp); its reply is only logged.
typedef struct {
    char *url;        // ex "turn:host.compute.shadow.tech:443?transport=tcp"
    char *host;       // parsed from the url
    int   port;       // parsed (443 is typical)
    char *transport;  // "tcp" or "udp"
    char *username;
    char *credential;
} TurnServerEntry;

typedef struct {
    TurnServerEntry *items;
    size_t           count;
} TurnServers;

void turn_servers_free(TurnServers *t);

bool launcher_get_turn_servers(const char *launcher_base, const char *bearer,
                                const char *vm_id, TurnServers *out,
                                long *http_status);

// GET https://api.eu.shadow.tech/v1/subscription/status?product_family=cloudpc
// Browser HAR reply: {id, plan_id, status, started_at, last_payment_status,
//   last_payment_date, on_hold, on_hold_reason, product_family}
// plan_id e.g. "cloudpc-b2c-power2023-EUR-Monthly" -> used in the UI header.
typedef struct {
    char *id;
    char *plan_id;            // ex "cloudpc-b2c-power2023-EUR-Monthly"
    char *plan_short;         // derived from plan_id (e.g. "Power 2023")
    char *status;             // "active" / "expired" / etc.
    char *last_payment_status;
    bool  on_hold;
    char *on_hold_reason;
    char *product_family;
    long  started_at;         // unix ts seconds
    long  last_payment_date;
} Subscription;

void subscription_free(Subscription *s);

// API_BASE_V1 = "https://api.eu.shadow.tech/v1/" - different from
// launcher_base.
bool launcher_get_subscription_status(const char *bearer, Subscription *out,
                                       long *http_status);

// GET https://api.eu.shadow.tech/v1/pu/shadow-drive/user/token?token_name=shadow-web-launcher-token&force=1
// Browser HAR reply: {login_name:string|null, token:string|null, message:string}
// When the user has not enabled Shadow Drive the fields are null and
// message="User does not exist".
typedef struct {
    char *login_name;   // may be NULL
    char *token;        // may be NULL
    char *message;      // ex "User does not exist"
} DriveToken;

void drive_token_free(DriveToken *d);

bool launcher_get_drive_token(const char *bearer, DriveToken *out, long *http_status);

// Returns this installation's stable device UUID (40 hex chars + \0).
// Persisted to disk on the first launch.
const char *shadow_device_uuid_public(void);

// Decodes the JWT's `instance` field (= the API path number in /N/clients,
// /N/stream). The official desktop app always uses JWT.instance to build the
// URLs, even when the server returns a proximus_url with a different `/N/`
// (slot_number can be stale or sticky). Returns -1 when decoding is
// impossible.
int launcher_jwt_instance(const char *jwt);

// Rewrites the first numeric segment of an HTTPS URL (/<N> or /<N>/...) with
// /<inst>. Modifies *url in place (free + malloc). A no-op when the segment
// cannot be found.
void launcher_rewrite_url_instance(char **url, int inst);
