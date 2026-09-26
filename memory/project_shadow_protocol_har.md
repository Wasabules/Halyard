---
name: Shadow API protocol confirmed via web HAR
description: Endpoints, headers, JSON keys and custom codes of the Shadow Launcher API, confirmed from a HAR of the pc.shadow.tech web client (April 2026)
type: project
---
On 2026-04-30, the maintainer captured a HAR (`pc.shadow.tech_Archive [26-04-30 23-23-00].har`, 3 MB, at the root of ~/shadow2switch/) of the login + start VM flow from the official web client. It confirms the real API contract (which differs from the reversed Java APK, for instance on naming conventions).

**Why:** it serves as the source of truth for aligning the Switch homebrew client (`05-shadow-client-borealis`). We found it after M3 (list VMs) worked but before M4 (start + IP) was fully validated on the device.

**How to apply:** when tackling a new Shadow endpoint, look at the HAR first before inventing the format; the conventions are snake_case (not camelCase as some Java symbols suggested).

### Auth (api.eu.shadow.tech / launcher-api/v3)
- Every route uses `Authorization: Bearer <ory_at_…>` (an OAuth/Ory token)
- Headers mandatory on the web side: `X-Vm-Id`, `X-Shadow-Uuid`, `X-Shadow-Agent`. The Switch showed that `X-Vm-Id` + Bearer are enough (X-Shadow-* probably not required, but to be confirmed).

### The validated flow (M3→M5)
1. `GET /vms?offset=&limit=` → a paginated list (`{"pagination":{total_count,…},"entries":[…]}`)
2. `GET /vms/{vm_id}` → the VM's details
3. `GET /vms/{vm_id}/capabilities` → the streaming flags (max_monitor_count, frame_rate, …)
4. `POST /shadow/auth_login` with the body `{"cbp_token":"<bearer>"}` → `{"token":"<opaque>"}` (NOT the streaming JWT, just a short token)
5. `GET /shadow/turn-servers` → `{"iceServers":[{urls,username,credential}, …]}` (TURN for WebRTC)
6. `POST /shadow/vm/start` (an empty body, the X-Vm-Id header) → 2xx, starts the VM
7. `GET /shadow/vm/ip` (header X-Vm-Id) → polling: **HTTP 470** = "vm not started yet" (a custom Shadow code, not an error), retry. When it returns 200:
   ```json
   {"ip":"…","port":"6000","offset":0,"slot_number":6,"alias":"…",
    "proximus_url":"https://…/<slot>","messaging_url":"https://…/<slot>/stream",
    "provider":"legacy","vm_session_id":""}
   ```
8. `POST /shadow/vm/proximus-credentials` body `[{"client_type":"launcher"},{"client_type":"main"}]`
   → an array of the 2 JWTs (launcher + main). The launcher JWT has the permissions `["stream","create","delete","forward","list","refresh"]`. **THOSE are the JWTs to use for proximus_url, not the Ory bearer.**

### Streaming (proximus_url = `https://<host>/<slot>`)
- `POST /<slot>/clients` body `{"type":"launcher","opaque":"<json>"}` (Bearer = launcher JWT) → `{"data":{… ,"spice_secret":"…"}}`
- `GET  /<slot>/clients?type=main&active=true` → checks that no main session is active
- `POST /<slot>/clients` body `{"type":"main","opaque":"<json device-id>"}` (Bearer = main JWT) → `{"data":{…,"streamingtoken":"…","streamingtoken_expiry":…}}`
- `GET  /<slot>/stream` → **SSE** (`text/event-stream`), not a WebSocket. Lines `: ping\r\n\r\n` (keep-alive) + events `data: {"type":"event","event":"<name>","data":…}\r\n\r\n` (vm_reachable / status_changed / encoding_is_ready / shadow-manager / …). A permanent connection — the client cuts it on a timeout.
- `GET  /<slot>/status` → JSON polling, `{meta:{version},data:{vm_status,reachable,streamer_up}}`. `streamer_up:true` is the go signal for starting the video stream (SPICE port 6000).
- `POST /<slot>/forward` → **NOT** keyboard/gamepad inputs (those go through the SPICE/WebRTC channel). It is a **client→VM RPC** channel for the launcher's events: body `{"command":"forward","data":{"type":"<event>","sender":"launcher",…}}` → `{"id":"<n>"}`. Events seen: get_version, is_streamer_reachable, subscribe_install_statuses, is_up_to_date.
- **An asymmetric pattern**: the VM answers the `/forward` POSTs **through the `/stream` SSE**, not in the POST's HTTP response. So `/stream` has to be opened FIRST (in a parallel thread) and only then can the `/forward`s be sent.

### Code 470
A custom Shadow HTTP code meaning "the resource is not ready yet" — always with an errored JSON body `{"error":{"code":409470,"message":"vm not started yet: …"}}`. To be treated as a retry, not a failure.

### Headers obligatoires pour `/shadow/*` (CRITIQUE)
Confirmed on the device on 2026-04-30: without `Origin: https://pc.shadow.tech` + `X-Shadow-Uuid` + `X-Shadow-Agent` (and a Referer for tidiness), the **Cloudflare/Envoy** proxy silently strips the `X-Vm-Id` header and the server answers `400 — "X-Vm-Id header or vmID path parameter is missing"`. The `/vms/...` routes (which put the vm_id in the path) work even without those headers — only the `shadow/*` routes that custom-route through X-Vm-Id are affected.

To be applied to EVERY new call to `api.eu.shadow.tech/v1/pu/virtual-desktop/...` that passes the vm_id as a header. See `demo/src/shadow/launcher.c::append_shadow_headers()` for the Switch implementation (a stable UUID v4 persisted in `/switch/shadow-client/device.uuid`).
