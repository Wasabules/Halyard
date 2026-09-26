// CtrlChanV2 REST API wrapper - endpoints `/3/clients`, `/3/forward` and
// `/3/status` on Shadow's streaming server (compute.shadow.tech).
//
// This is the HTTP/2+JSON HALF of the CtrlChanV2 protocol. The encrypted
// streaming control plane is the other half and rides a separate TCP/TLS
// connection, on the "controlchan" port advertised by /3/clients.
//
// Shape discovered from a decrypted pcap plus Ghidra RE:
//   POST /3/clients       - registers a client (launcher/main/usb)
//                           reply = { data: { id, streamingtoken, expiry, remote } }
//   POST /3/forward       - forwards one command to the VM
//                           body = { command:"forward", data:{ type, sender, ... } }
//   GET  /3/status        - SSE long-poll of status changes
//   DELETE /3/clients/<id> - clean unregister

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHADOW_CLIENT_TYPE_LAUNCHER 0
#define SHADOW_CLIENT_TYPE_MAIN     1
#define SHADOW_CLIENT_TYPE_SPICE    2
#define SHADOW_CLIENT_TYPE_USB      3

/* Session credentials returned by POST /3/clients. */
typedef struct {
    char     client_id[80];        /* ex: "<user-id-A>-...-launcher" */
    char     streamingtoken[256];  /* opaque, used in the streaming flow's Authenticate */
    long     token_expiry_unix;    /* unix ts, may be absent (=0) */
    char     remote_ip[64];        /* public IP as the server sees it */
} shadow_session_creds;

/* POST /3/clients - registration with the VM streaming server.
 * `vm_host` : GPU server hostname (e.g. "gpu-revel-can-X.frsbg01.compute.shadow.tech",
 *              WITHOUT the "ipv6-" prefix so it resolves A+AAAA; http.c then
 *              forces IPv4)
 * `bearer`  : OAuth JWT (the one oauth.c already gave us)
 * `client_type` : SHADOW_CLIENT_TYPE_*
 * `opaque_json` : inline JSON payload, e.g. `"{"os":"Switch","arch":"aarch64","platform-type":"console"}"`
 *
 * Fills out_creds and returns true on success.
 */
bool ctrl_rest_register_client(const char *vm_host, const char *bearer,
                                int client_type, const char *opaque_json,
                                shadow_session_creds *out_creds);

/* POST /3/forward - forwards one typed command to the VM.
 *  body sent : {"command":"forward","data":{...command_data...}}
 *  out_resp : malloc'd JSON response, the caller must free() it. NULL is OK.
 */
bool ctrl_rest_forward(const char *vm_host, const char *bearer,
                        const char *command_data_json,
                        char **out_resp, size_t *out_len);

/* DELETE /3/clients/<client_id> - clean unregister at end of session. */
bool ctrl_rest_unregister(const char *vm_host, const char *bearer,
                           const char *client_id);

/* GET /<instance>/clients, then DELETE every client whose `opaque` JSON holds
 * `device-id` == device_uuid. This clears the zombie clients that stop the
 * server from binding :port_base+11 (observed 2026-05-09: the VM refuses to
 * re-bind while a previous client is still registered).
 *
 * Returns the number of clients DELETEd, or -1 if the listing failed.
 */
int ctrl_rest_clean_my_zombies(const char *vm_host, const char *bearer,
                                  int instance, const char *device_uuid);

/* DELETE EVERY client of the VM: a forced cleanup that also kills the other
 * live sessions, the official desktop app included. Dev/debug only.
 * Returns the number DELETEd, or -1 if the listing failed. */
int ctrl_rest_clean_all_clients(const char *vm_host, const char *bearer,
                                   int instance);

#ifdef __cplusplus
}
#endif
