// LauncherApiService - authenticated REST calls on the URL TINAG returned.
// Every route under "shadow/..." uses the Bearer access_token.

#pragma once
#include <stdbool.h>
#include <stddef.h>

/* === VMK1 2026-10-03 - THE KEYS /vms SENDS AND WE DROPPED ================
 *
 * The server's reply carries ten keys (census in
 * `clients/borealis/ui/VM_LIST_PORT.md`: id, name, hwconfig, datacenter,
 * maintenance, tags, provider, status, siberia_disabled,
 * graphic_driver_reinstall) and this struct carried four of them. The rest
 * went into `raw_json` and were never looked at.
 *
 * Three of the dropped ones belong on a machine card and nowhere else:
 *
 *  - `maintenance` is the one that costs something to miss. A machine under
 *    maintenance accepts a connect and fails seven steps later; a card that
 *    says so stops the attempt before it starts.
 *  - `datacenter` is PER MACHINE. The Qt client was painting the
 *    account-wide name from TINAG onto every card, which is right only while
 *    an account has its machines in one place.
 *  - `hwconfig` is the hardware tier ("power", "boost", "Neo"). It was being
 *    used as a fallback for an empty `name` and so could never be shown as
 *    what it is.
 */
typedef struct {
    char *id;          // identifiant interne (UUID)
    char *alias;       // user-customised name (may be NULL)
    char *name;        // model / plan name (may be NULL)
    char *state;       // "running" / "stopped" / etc. (may be NULL)
    char *hwconfig;    // VMK1: the hardware tier, e.g. "Neo" (may be NULL)
    char *datacenter;  // VMK1: THIS machine's data centre NAME (may be NULL)
    char *speedtest_url;  // VMK1: from the same object; the "re-run the test"
                          //       endpoint the official client offers
    char *provider;    // VMK1: may be NULL
    char *tags;        // VMK1: joined "a,b" (may be NULL)
    bool  maintenance; // VMK1: the server says this machine is unavailable
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
/* === CAPS2 2026-10-03 - WHAT THIS REPLY ACTUALLY CARRIES ==================
 *
 * We read four values out of this body for months and kept the rest in
 * `raw_json` without ever printing it. Printed once (`halyard-qt --probe
 * caps`), it answered two questions the protocol work had open:
 *
 *  - `streaming.channels.video.max_monitor_count` IS the per-client display
 *    ceiling the server enforces. ShadowStreamer 6.3.1 refuses an outputId
 *    past a number it keeps at `client+0x238` and says "VM does not support
 *    more than %d display(s)"; this is where that number comes from (DISP1).
 *    Measured 2 on a Neo.
 *
 *  - the whole `usage` block, which is the "5h55m restantes" the official
 *    client shows. It is NOT a remaining time pushed by the server - there is
 *    no runtime endpoint at all - it is `max_session_length` counted down
 *    locally from the start of the session. 21600 s on this account, and the
 *    official client's "5h55m" at five minutes in matches exactly.
 *
 * The fair-use figures are the ACCOUNT's, not the machine's, and they are
 * personal: a monthly allowance, how much of it is spent, and when it renews.
 * They are parsed because the client has a legitimate use for them (telling
 * someone where they stand) and for no other reason; nothing here is logged.
 */
typedef struct {
    // Seconds. 0 = the server did not say.
    int    max_session_length;   // one session's ceiling - the countdown
    int    max_duration;         // the period's allowance (fair use)
    int    fair_use_usage;       // spent so far in the period
    double fair_use_alert_threshold;  // ex 0.8 = warn at 80 %
    char  *fair_use_renew_date;  // ISO 8601, when `fair_use_usage` resets
    char  *end_of_streaming_session;  // ISO 8601 or NULL - a hard stop
    bool   time_slots_enabled;
    char  *time_slots_timezone;
} VmUsage;

typedef struct {
    char *raw_json;       // copy of the parsed body, for debugging
    bool  video_allowed;
    int   max_frame_rate; // ex 240
    int   max_width;
    int   max_height;
    int   max_monitor_count;  // CAPS2/DISP1 - the display ceiling. 0 = unsaid
    char *video_codecs;   // joined "h264,h265,av1"
    char *video_chroma;   // joined "444,420" - which subsamplings are allowed
    bool  audio_allowed;
    char *audio_codecs;   // joined "opus,flac"
    bool  micro_allowed;  // the channels the account may open at all
    bool  clipboard_allowed;
    bool  filetransfer_allowed;
    bool  gamepad_allowed;
    VmUsage usage;
} VmCapabilities;

void vmcaps_free(VmCapabilities *c);

bool launcher_get_capabilities(const char *launcher_base, const char *bearer,
                                const char *vm_id, VmCapabilities *out,
                                long *http_status);

/* CAPS2 - the parser alone, for a body already in hand. Separate so the
 * offline suite can exercise the shape of this reply without an account, a
 * network or a machine; `tests/test_caps_parse.c` does. `out` must be zeroed
 * by the caller. False only when `body` is not JSON. */
bool launcher_parse_capabilities(const char *body, VmCapabilities *out);

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
