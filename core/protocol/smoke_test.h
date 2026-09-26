// Runtime smoke test for the streaming stack:
//   1. POST /3/clients on the VM streaming server (with forced IPv4)
//   2. Parse the streamingtoken
//   3. DELETE /3/clients/<id>
//   4. Log everything to the journal (shadow/journal.h)
//
// Proves at runtime that the REST stack + forced IPv4 + DNS strip work on the
// Switch before investing in what comes next (the M32+ state machine).

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the smoke test from the connecting activity, right after the VM
 * hostname has been fetched via /vm/ip.
 * Returns true if POST /3/clients = 200 and the streamingtoken was extracted.
 */
bool streaming_smoke_test(const char *vm_host, const char *bearer);

/* Triple variant: tries launcher_jwt+type=0, main_jwt+type=1, then
 * main_jwt+type=0 (cross-check). Returns true if at least one combination is
 * accepted. The logs say which one. */
bool streaming_smoke_test_full(const char *vm_host,
                                 const char *launcher_jwt,
                                 const char *main_jwt);

/* Decodes a JWT to extract the instance field (varint).
 * Returns the instance, or -1 on error. */
int jwt_instance(const char *jwt);

/* M32 bootstrap: SSL connect host:443 (no ALPN) -> Authentication protobuf ->
 * Encryption protobuf -> parse reply -> log key.
 *
 * The caller must supply:
 * - streaming_token: opaque token (vmRPfTl... 34 chars) from POST /clients
 * - client_id: the client id returned by /clients
 * - bearer_jwt: the main JWT (978 chars) for the Authorization: Bearer header
 *
 * Returns true if every step succeeds through to the extracted 32 B key. */
bool streaming_smoke_test_m32(const char *vm_host,
                                const char *streaming_token,
                                const char *client_id,
                                const char *bearer_jwt);

#ifdef __cplusplus
}
#endif
