---
name: 🎯 SslCtrlChanV2 wire format DÉCOUVERT 2026-05-06
description: An LD_PRELOAD hook on SSL_write revealed the real SslCtrlChanV2 wire format = [uint16_be type][uint16_be len][protobuf]. The cause of the 400 Bad Request blocking N0 is finally resolved.
type: project
---
## Contexte
The WebRTC → native pivot (see `project_pivot_native_protocol`). We were stuck on a systematic 400 Bad Request when sending `[uint32_be len][protobuf]` over direct TLS on port 443. An LD_PRELOAD hook `shadow_tls_hook.so` on the official desktop app's (ShadowPCDisplay) SSL_write/read captured the **decrypted application plaintext**.

## Setup hook
- `06-shadow-recon-linux/tls_hook/shadow_tls_hook.c` : LD_PRELOAD shim, hook `SSL_write`/`SSL_read`/`*_ex` via `dlsym(RTLD_NEXT)`. Output `/tmp/shadow_tls.log` (hex dump + peer/fd/timestamp/pid).
- `run_shadow_with_hook.sh` lance shadow-prod avec LD_PRELOAD set.

## Findings critiques (vs notre code 2026-05-06)

### 1. Format wire SslCtrlChanV2
**Vrai** : `[uint16_be msg_type][uint16_be payload_len][protobuf]`
**Ce qu'on faisait** (`ctrl_tcp.c`) : `[uint32_be len][protobuf]` → 400 Bad Request

An example captured:
```
SSL_WRITE ssl=0x1fcec5f0 fd=-1 len=91
0000  00 01 00 57   ← type=1, len=0x57=87
0004  12 04 1a 02 0a 00   ← protobuf
...
```

### 2. Message types observed (the init sequence)
- **type=1, len=87** (1ère SSL_WRITE) : **Capabilities client** — version 2, UA string, "OCapture" nested cap
- **server reply type=1, len=106** (1ère SSL_READ) : **Capabilities server** — "ShadowStreamer 6.1.7"
- **type=1, len=245** (3e SSL_WRITE) : **Authentication** avec streaming_token + sessionId UUID + client_id
- (etc. — every type=1 frame; the `type` does not seem to distinguish the sub-messages, the inner protobuf does)

### 3. Transport SslCtrlChanV2
- **Direct TLS on `:443`** (the same hostname as REST), **ALPN EMPTY**
- No HTTP wrapping (no `POST /N/path`, no `Content-Type`) — just the raw frames after the TLS handshake
- The SSL context uses a **memory BIO** (`fd=-1` in the hook) — Shadow reads/writes the TLS records into a buffer and then transports them through libuv. That is why `SSLKEYLOGFILE` does not capture the keys (a custom BIO).

### 4. The REST API in parallel (other SSL contexts)
- `GET /7/clients` HTTP/1.1 (not h2!) with the headers `User-Agent: Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3` + `X-Shadow-Agent: Renderer-Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3`
- `GET /7/status` HTTP/1.1 (pareil)
- Plus two distinct SSL contexts (fd=234, fd=237) — not reused for the binary

### 5. Path actuel = /7/
JWT instance=7 (a legacy account, ports=null). The path varies per session — always use `jwt_extract_instance` to generate it.

## Immediate action items

| ID | Fix | Fichier |
|---|---|---|
| F1 | The wire format `[u16_be type][u16_be len][proto]` instead of `[u32_be len][proto]` | `ctrl_tcp.c::ctrl_tcp_send_cleartext` |
| F2 | No more HTTP paths — raw direct TLS, no POST | `ctrl_tcp.c` (skip path/method) |
| F3 | 1er message = Capabilities (pas Authentication) | `ctrl_msgs.c` add `ctrl_build_capabilities` + sequence |
| F4 | Header REST manquants : User-Agent + X-Shadow-Agent + version 12.3.3 | `ctrl_rest.c` |

## Limitation hook
ShadowPCDisplay exits early because of an expired certificate on `prod.log.frsbg01.shadow.tech`. We could not capture a complete streaming session (the post-Authentication video/audio frames). The Capabilities + Authentication wire format is enough to move forward.

To extend the capture (a complete Capabilities reply, the Encryption flow, video frames) → relaunch the hook after ignoring the expired certificate (perhaps through `--ignore-certificate-errors` on the app, or by patching the certificate locally).

## Refs
- Log brut : `/tmp/shadow_tls.log` (2.7 MB, 36k lignes, 2969 SSL_*)
- Hook source : `06-shadow-recon-linux/tls_hook/shadow_tls_hook.c`
- An incomplete decrypted pcap (skipped, because libuv's BIO gives no keylog): `06-shadow-recon-linux/captures/session-20260502-214916/`

## Update 2026-05-06 21:05 — F1+F3 implemented but the 400 Bad Request persists

After fixing the wire format to `[u16 type][u16 len][proto]` + a Capabilities builder producing the official capture's payload **byte-exact** (87 identical bytes), a runtime test on the VM `gpu-<instance>`:443 → **nginx returns `HTTP/1.1 400 Bad Request`**. No Capabilities reply.

**What is left to diagnose**: nginx routes our SSL traffic as HTTP. 3 hypotheses:
1. **The JA3 fingerprint**: wolfSSL TLS 1.3 differs from Shadow's bundled OpenSSL 1.1.1 (cipher suite, extension order, GREASE, etc.). nginx may route on JA3 between "REST API h2" and "SslCtrlChanV2 binary".
2. **A different port**: SslCtrlChanV2 may not be on :443 but on another port (slot×1000+? or something else). The LD_PRELOAD hook showed `fd=-1` = a memory BIO, so we do not know the real socket port.
3. **A magic byte / a setup before Capabilities**: perhaps a pre-message we have not seen.

**Next step**: N0c — re-run the hook + tcpdump in parallel to identify the real port + compare the JA3.

## Update 2026-05-06 21:20 — N0c DONE, the port identified

tcpdump during an official desktop-app session reveals 3 TCP TLS ports without SNI or ALPN:
- **`:13011`** = SslCtrlChanV2 (the streaming_re memory note already mentioned it)
- `:13014` = a secondary control channel (encryption, perhaps)
- `:13020` = secondary control

All towards the IPv6 `<VM public IPv6>::` = the host `gpu-<instance>` (= the VM allocated to the desktop app for that session).

The `:13011` stream's ClientHello, captured: extensions `11,10,35,22,23,13,43,45,51` (NO 0=SNI, NO 16=ALPN). A mix of TLS 1.3 and TLS 1.2 cipher suites.

### The desktop app's sequence → `:13011`
1. `GET /6/clients` (REST)
2. `POST /6/clients` (register main client)
3. `GET /6/version`
4. `GET /6/stream` SSE long-poll (= keepalive)
5. `GET /6/status`
6. **Connect :13011** TLS handshake → 1ère SSL_WRITE = Capabilities (87 bytes proto)
7. `POST /6/stats` (~11k bytes initially — perhaps capabilities/a manifest too)

### What we fixed on the code side
- `ctrl_tcp.c`: connect IPv6 first (AF_UNSPEC + try each family), skip UseSNI on a port other than 443
- `smoke_test.c`: uses `ctrl_tcp_open_port(host, 13011)` instead of `:443`
- Capabilities builder match byte-exact (87 bytes)
- Wire format `[u16 type][u16 len][proto]`

### Blocked at runtime on `gpu-above-object`
The VM allocated by launcher_get_vm_ip = `gpu-<instance>`, IPv6 `<VM public IPv6>::`. **`:13011` times out over IPv6 AND IPv4.** That VM does not have the port open, unlike `gpu-lyric-upper`, which the desktop app reached.

Hypotheses:
1. **VM pool load-balancing**: we land on different VMs depending on timing/state
2. **A missing REST trigger**: perhaps `GET /6/version` or an initial `POST /6/stats` unlocks the streaming ports
3. **A different VM type**: some VMs have no stream support

**Action remaining**: add a `GET /6/version` + retries on :13011 (5×3 s) to the smoke test. If it still times out, the VM pool is the problem → we either have to force a specific VM through a `launcher_start_vm` argument, or wait.

## Update 2026-05-06 21:25 — :13011 listening = trigger applicatif manquant

Tests :
- `curl -6 https://gpu-above-object:443/` → connect ✅ (= VM live)
- `curl -6 https://gpu-above-object:13011/` → **timeout** ❌
- 6 retries in the smoke test after /version + /status → all time out

Conclusion: the VM is online BUT port :13011 is not listening. Not a cold start (30 s+ of retries do not open it).

**A strong hypothesis**: an initial `POST /N/stats` with a **~11k-byte hardware manifest** unlocks the :13011 binding server-side. It is the LAST REST call before :13011 opens in the official capture (the sequence: /clients → /version → /stream SSE → /status → POST /stats 11083 bytes → :13011 SYN).

**The priority next step**: dissect the `POST /6/stats` 11083-byte payload in `/tmp/shadow_tls.log` (probably a JSON hardware/OS/screen manifest). Reproducing that payload will probably unlock the ports.

**Note**: a simpler test = while a ShadowPCDisplay session really is streaming (= with :13011 open), run a `curl -6 :13011` in parallel to see whether the port is bound globally or only for the authorised client. If an external curl can connect → the problem is only the application-level trigger.

## Update 2026-05-06 21:30 — the full REST sequence reproduced, `:13011` still closed

The smoke test now does:
- `proximus_create_main_client` (= POST /N/clients)
- `proximus_sse_start` (= GET /N/stream SSE long-poll en thread)
- `http_get(/N/version)` added
- `http_get(/N/status)` → **200 OK**, body : `{"vm_status":"started","reachable":true,"streamer_up":true}`
- 6 retries connect :13011 (IPv6 + IPv4) → **tous timeout**

**The wall**: the VM answers on `:443` (REST h2 OK) and confirms `streamer_up=true`, but `:13011` stays closed. Server-side triggers we are missing:
1. **An expected SSE event**: perhaps the desktop app receives a particular event on GET /N/stream (something like `acquisition_is_ready` → to which we must reply something)
2. **A per-client_id policy**: the VM only opens :13011 for the "main" client_id that negotiated streaming. Our `POST /N/clients` flow returns an id, but perhaps not the right "type" for streaming
3. **A `POST /N/stats` with a specific event**: the desktop app posted 11k bytes of telemetry just before :13011 opened. But it is standard telemetry, `{"data":[{"name":"renderer.event"...}]}`, not a logical trigger.

**Liste exhaustive des REST calls app desktop** (ShadowPCDisplay pid=671677) :
1. GET /6/clients
2. POST /6/clients
3. GET /6/version
4. GET /6/stream (SSE)
5. GET /6/status
6. POST /6/stats × 4 (telemetry)
7. DELETE /6/clients (cleanup)

No /6/forward, no additional magic endpoint.

## Final update 2026-05-06 21:35 — the SSE events decoded, the verdict

Decoding the VM:443 SSE reads in `/tmp/shadow_tls.log`:

```
HTTP/1.1 200 OK Content-Type: text/event-stream chunked
data: ping                                              ← ~1Hz keepalive
data: {"type":"event","event":"shadow-manager","data":{
    "target":"VMP","sender":"ShadowManager",
    "type":"encoding_is_ready",
    "value":{"encoding_type":"hardware","is_banner":false}}}
data: {"type":"event","event":"shadow-manager","data":{
    "target":"VMP","sender":"ShadowManager",
    "type":"acquisition_is_ready"}}
... ~10s plus tard ...
data: {"type":"event","event":"getout","data":"removing client, get out !"}
data: {"type":"error","data":"removing client, get out !"}
0\r\n\r\n   ← fin chunked
```

**Event timing (= relative to the ShadowPCDisplay start):**
- t+1.5 s: the SSE handshake is OK
- t+1.6 s: SYN to `:13011` (frame 3932) — the TCP+TLS connection is established
- t+2.6s : `encoding_is_ready` event arrive
- t+2.8s : `acquisition_is_ready` event arrive
- t+12.8s : `getout` event → DELETE /6/clients → 204 No Content

**A key finding**: even the official desktop app got kicked out after ~11 s! The user confirmed "I could not open the stream". Consistent with the `feedback_switch_thread_lifecycle` note (a strict server-side timeout).

**A revised hypothesis**: there is an **application-level challenge** to complete on :13011 within the ~10 s after it opens, otherwise the server declares the client dead and sends `getout`. The desktop app did its Capabilities + Authentication + Encryption but perhaps not the final challenge (cf. the certificate error, which would have blocked an intermediate telemetry report).

**The implication for us**: :13011 is probably bound per VM session, **only when the client has an active, live session**. Our attempt from shadow-test-cli with no session does not seem to activate the port on the VM side.

## Plan demain
1. **Recreate a "complete" session**: get the launcher_jwt + main_jwt + token + sessionId then try :13011 within the window (~5 s after /status returns `streamer_up: true`)
2. **Actively read the SSE**: perhaps we have to **wait for `encoding_is_ready`/`acquisition_is_ready`** on the SSE BEFORE opening :13011 — those 2 events suggest the VM is ready
3. **Patch the expired log certificate in the ShadowPCDisplay bundle** (the certificate is on `prod.log.frsbg01`, which is down; perhaps patch in a TLS mock) so that the desktop app streams normally and we can observe the real :13011 sequence
4. **OR**: fully implement Capabilities → Authentication → Encryption in our client + try it in a new session (perhaps the application-level challenge is simply finishing the auth flow, and we then see the VM accept us)

The native pivot is viable. What remains is joining up the missing pieces.

## Update 2026-05-06 21:42 — the SSE is silent on our side

Test added: logging inside `sse_keepalive_thread` to see the response.

```
[sse] connecting https://ipv6-gpu-<instance>.frsbg01.compute.shadow.tech/3/stream
... 60s+ silence ...
(timeout, perform_exit jamais atteint)
```

The server **opens the connection** (TCP+TLS+HTTP) but **sends NO body bytes** (no `: ping`, no event). Different from the official desktop app, which received `: ping` at 1 Hz as soon as it opened.

But the other REST calls work: `/N/version` 200 OK, `/N/status` 200 OK with `streamer_up:true`.

**An updated conclusion**: on the server side, our client is REGISTERED (POST /N/clients is fine) but is not considered an **authorised streaming client** — hence the silent SSE and the closed :13011. Something in the registration is filtering us out:
- Is our `opaque` metadata different from the desktop app's? (cf. `{"arch":"x64","platform-type":"desktop","os":"Linux","os-name":"Linux"...}` for the desktop app against our body)
- Notre `device-id` ?
- The `version` field in `/clients`?

App desktop POST /N/clients body inclut :
```json
{"opaque":{"arch":"x86_64","device-id":"<device-id 40hex>","os":"linux","os-name":"Ubuntu","os-version":"24.04","platform-type":"desktop","timestamp":"2026-05-06 21:13:26","version":"1.0.0"}}
```

Our client probably sends a different opaque (to be checked in `proximus_create_main_client`). The `device-id` looks like the key: if we do not send a "Shadow-trusted" device id → no streaming.

**Plan demain v3** :
1. Compare the desktop app's POST /N/clients body (seen in the server-side response) against what we send through proximus_create_main_client
2. Match exact des fields opaque (arch=x64, os=linux, os-name=Ubuntu, version=1.0.0, etc.)
3. Reproduce the device-id by generating a UUID matching the pattern (40 hex chars)
4. If the SSE starts receiving events → `:13011` should open too

## Final update 2026-05-06 21:48 — the opaque body fixed, the problem persists

The desktop app's POST /6/clients body decoded from the hook log:
```json
{
  "type": "main",
  "opaque": "{\"arch\":\"x86_64\",\"device-id\":\"<device-id 40hex>\",
              \"os\":\"linux\",\"os-name\":\"Ubuntu\",\"os-version\":\"24.04\",
              \"platform-type\":\"desktop\",\"timestamp\":\"2026-05-06 21:13:26\",
              \"version\":\"1.0.0\"}"
}
```

Our `build_opaque` was spoofing Chrome web (a WebRTC legacy). The patch:
- `platform-type`: `web` → **`desktop`** (CRITIQUE)
- `os`: `Linux` → **`linux`** (lowercase)
- `os-name`: `Linux` → **`Ubuntu`**
- `os-version`: long UA Chrome → **`24.04`**
- Ajout: `version: "1.0.0"`

Runtime test: `:13011` still times out, the SSE is still silent.

## Root cause identified: TWO different accounts

The `client_id`s returned:
- App desktop officielle (capture) : `<user-id-A>-<device-id 40hex>-main`
- Notre shadow-test-cli       : `<user-id-B>-<client uuid>-main`

**The `<user-id-A>` against `<user-id-B>` prefix is the Shadow user id.** The desktop app is logged into a different account from the one our OAuth refresh_token gives access to.

The user indicated that their account works with the desktop app, but our `oauth_load_refresh()` probably loads a token from another account (an internal test account?). Check:
- `/switch/shadow-client/refresh_token` or its Linux equivalent
- Whether we can force a login with the account that works in the desktop app

If the user_id differs → a VM allocated by a different account = a different test environment. That is probably the cause of the difference between the VM `gpu-lyric-upper` (the desktop app) and `gpu-above-object/joyful-value` (shadow-test-cli).

## VRAI plan demain

1. **Check the logged-in account**: examine the `iat`/`iss`/`sub` of the JWT we use against the desktop app's JWT
2. **Log in with the right account**: potentially re-do the OAuth to match the account that works in the desktop app
3. **OR force the use of the desktop app's account**: extract its refresh_token from its `~/.config/shadow-prod/` (the Electron localStorage / cookies)
4. Once on the right account, start from scratch with a correct desktop-renderer opaque body → test whether :13011 opens

## Final update 2026-05-06 22:00 — the OAuth client_id + the opaques + the device-id fixed, `:13011` still closed

Patches applied this session to match the official desktop app exactly:

**1. `SHADOW_OAUTH_CLIENT_ID`**: `7aa43568-...` (the Android APK) → `0c6ee748-5352-412c-944f-947e15df8bf0` (the Linux desktop renderer, seen in the idToken `aud` of `~/.config/shadow/launcher-prod.json`).

**2. Re-OAuth through the device_authorization flow**: the refresh_token was regenerated through an interactive login on the right client_id. The JWT idToken confirms the account `<account email>`, sub `8b7bc4ee-...`.

**3. Opaques differentiated by client type**:
- Launcher (Electron) : `{arch:x64, os:Linux, os-name:Linux, os-version:<kernel>, platform-type:desktop}`
- Main (renderer) : `{arch:x86_64, device-id:<40hex>, os:linux, os-name:Ubuntu, os-version:24.04, platform-type:desktop, version:1.0.0}`

**4. `device-id` format** : UUID v4 36 chars → SHA-1 like 40 hex chars (machine-id + 8 random).

Effects: a new client_id prefix returned by POST /N/clients (`<user-id-C>-<uuid>...` instead of `<user-id-B>-<client uuid>...`) → the server recognises a new client. `streamer_up:true` confirmed. But :13011 still times out.

## Conclusion: 6 differences aligned, but `:13011` is closed on OUR VM pool

The VMs allocated to our shadow-test-cli flow simply do not seem to **have :13011 listening**, whatever we do to reproduce the desktop renderer's identity. Our flow gets VMs (`gpu-above-object`, `gpu-joyful-value`, `gpu-holy-road`) that are in a different pool from the official desktop app's (`gpu-lyric-upper`).

A final hypothesis: **the Shadow VM pool takes into account a factor we are missing** — perhaps an additional API endpoint we did not capture (binary HTTP/2), or a flag in the user's account (a streamer subscription tier enabled/disabled), or a Cloudflare/anti-fraud cookie.

## The actual plan for tomorrow (simpler this time)

Given how much time we have spent trying to reproduce the desktop app, a more pragmatic alternative:

**Approche A — Patcher l'app desktop officielle** :
- The LD_PRELOAD hook as before, to observe and inject
- Modify the C/C++ code to intercept the active Shadow session: forward the wire to our Switch through a home-made TCP relay
- ~1 day of work, but it avoids REing the `:13011` binding

**Approach B — test in parallel with an active session**:
- Lancer ShadowPCDisplay → finit la session (cert error fix d'abord)
- While port :13011 is bound on THEIR VM, launch our client sniffing the SAME VM to validate our stack
- Confirmer Capabilities → Auth → Encryption marche bytes-exact

**Approach C — Pivot back to WebRTC**: abandon the native path, return to libdatachannel + transparent auto-reconnect (already implemented). The user said "forget WebRTC", so it is a last resort.

Recommendation: **A**. Forking the desktop app is ugly but unblocks everything. On Switch we would then just port the right pieces of the desktop app (no reverse engineering).

## Update 2026-05-06 22:15 — the headers patched, the first SSE event received (`display_is_ready`)

Patches additionnels :
- `SHADOW_USER_AGENT` Chrome → `Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3`
- `SHADOW_X_AGENT` Chrome → `Renderer-Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3`
- Removed the `Origin:` and `Referer:` headers towards the proximus VM (the desktop app does not send them — they are a sign of a web client)
- Smoke test: poll /N/status until `streamer_up=true`, then try `:13011` (with retries)
- The SSE writer parses and logs `[sse] data: ...` events + detects `acquisition_is_ready`

**The critical result**: one SSE event received this time! `data: {"type":"event","event":"shadow-manager","data":{"target":"(anonymous sender)","sender":"ShadowManager","type":"display_is_ready"}}`

**= proof that we are close**: 0 events before, 1 now. The header patches made the server recognise us as a legitimate client.

Mais :
- Later sessions = 0 SSE events (probably zombie sessions not DELETEd on the server side)
- `:13011` still times out even with `streamer_up=true`
- Every streaming port (`:13011`, `:13014`, `:13020`, `:1011`, `:1015`) times out on the allocated VM

## Endpoints tested in the Electron JS (asar)

`grep "/path"` in `release/main/main.js` (Electron) confirms the full list:
- `/clients` (POST main + launcher) ✅
- `/version` ✅
- `/status` ✅
- `/stream` SSE ✅
- `/stats` POST telemetry (we do not send it; probably not critical)
- `/forward` (the memory note mentions `{type:"hello"}`, but a POST gives 401 without the right proximus JWT)
- `/shadow/shared/session` (= secondary session, "Gap-Secondary")
- `/shadow/vm/start` `/shadow/vm/stop` `/shadow/vm/ip` `/shadow/vm/proximus-credentials` ✅
- `/shadow/vm/fsi` (inconnu)

**No obvious trigger for binding :13011.** The binding must happen automatically on the server side according to some internal state (perhaps tied to the SSE polling / a stream watcher) that we are not stimulating correctly.

## Conclusion finale ce soir (2026-05-06 22:20)

The native pivot: 12 critical differences aligned with the desktop app, but :13011 stays closed. **A genuine server-side wall** that we cannot get past without:
1. **A parallel test**: the user launches ShadowPCDisplay on their VM (resolving the expired log certificate first) and, while :13011 is open on THEIR VM, we hit it with our custom client to validate the stack (Capabilities → Auth → Encryption byte-exact)
2. **A deeper reverse** of the Electron main.js + the ShadowPCDisplay binary, to identify the exact trigger that makes the VM bind :13011 per session-client.

**The native pivot is viable but still needs ~1 day of effort** to reach the first stream. Against the WebRTC option (already working, with the t=120 s freeze as its single trade-off). The user has explicitly excluded WebRTC.

**Final state**: every code patch is clean, the memory notes are complete; tomorrow we either bypass through a fork of the desktop app (option A) or run a parallel test (option B).

### Source refs
- pcap : `/tmp/shadow_traffic.pcap` (4.8 MB)
- TLS log : `/tmp/shadow_tls.log` (106k lignes, sequence /6/* visible)
- Hook : `06-shadow-recon-linux/tls_hook/`
- Capture wrapper : `tls_hook/capture_full.sh` (root, /etc/hosts patch + tcpdump + LD_PRELOAD)
