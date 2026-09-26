---
name: Cross-platform RE overnight findings 2026-05-03
description: Findings from the night of 2026-05-03 — a comparative RE of the Linux/Mac/Windows/Android Shadow clients + the JS launcher. An incorrect JWT.ports, a missing X-Shadow-Agent, the /3 → /5 path, and protobuf field bugs identified.
type: project
---
## Versions of the Shadow binaries analysed (downloaded 2026-05-03)

- Linux 9.9.10410 beta + 9.9.10388 prod (2 binaires distincts MD5)
- macOS x64 + arm64 (prod 9.9.10388 + beta 9.9.10410)
- Windows x64 (prod 9.9.10388 + beta 9.9.10410)
- Android ARMv7 (existant, ~v3.33)

## The cross-platform TLS stack (a major revelation)

| Plateforme | SSL_* imports | EVP_* | curl_easy_send/recv | Stack |
|---|---|---|---|---|
| **Linux** | **17** | many | absent | **dual: libcurl + raw OpenSSL** |
| Mac | 0 | many | absent | **libcurl only** |
| Win | 0 | 0 | **absent** (que curl_easy_perform) | libcurl only, single-shot |
| Android | (msquic statique) | — | — | msquic (dormant) + libcurl |

**Conclusion**: Mac/Win use libcurl exclusively → SSLKEYLOGFILE works for EVERY connection on those platforms. Linux has a raw OpenSSL path bypassing libcurl, hence the difficulty of decrypting its binary streams.

## Bugs identified in the Switch code

1. **`launcher.c:433`** body `proximus-credentials` :
   - Avait `client_type` (snake_case)
   - **Must be `clientType`** (camelCase) — seen in the Linux launcher's main.js
   - **Must include `ports:["controlchan","video","cursor","input"]`** for the main type
   - **Must include the `usb` clientType** for shadowusb

2. **`streaming/ctrl_rest.c:78,143,177`** :
   - Avait `/3/clients`, `/3/forward`, `/3/clients/<id>`
   - **Must be `/5/...`** (instance=5 in the JWT, confirmed by decoding the pcap)

3. **`shadow/proximus.c`** :
   - `X-Shadow-Agent` is missing from the 3 HTTP functions (createLauncherClient, createMainClient, dump_stream)
   - **Patched** with `headers = curl_slist_append(headers, "X-Shadow-Agent: " SHADOW_X_AGENT);`

4. **`streaming/ctrl_msgs.c::ctrl_build_authentication`** — protobuf wrong fields :
   - Field 3 used for `streamingtoken` → it SHOULD be a uint32 protocol_version
   - Field 5 used for `sessionUniqueId` → it SHOULD be `connectionUniqueId`
   - Field 6 used for `connectionUniqueId` → it SHOULD be `sessionUniqueId`
   - **The streamingtoken is NOT in the protobuf** — it goes into the `Authorization: Bearer <token>` HTTP header
   - **TO FIX** (not applied yet)

## The JWT decoded from the pcap — it invalidates the earlier hypothesis

The current user's main JWT (from the full2-20260503-032223 pcap):
```json
{
  "type": "main", "instance": 5,
  "permissions": ["stream","create","delete","forward","list","refresh"],
  "ports": null   ← BUT the binary stream WORKS anyway!
}
```

**So**: `JWT.ports=null` does not prevent binary mode. The note `project_shadow_jwt_ports_pivot.md` was partial/incorrect on that point.

## Custom HTTP headers to send

```
User-Agent: Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3
X-Shadow-Agent: Renderer-Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3
X-Shadow-Uuid: <SHA1 hex 40-char>
X-Vm-Id: <vm-uuid>
Authorization: Bearer <main_jwt>
```

## The ShadowPCDisplay CLI (50 flags identified)

Critiques :
- `--token <main_jwt>` (= tokens.renderer du proximus-credentials)
- `--agent <UA>` (User-Agent string)
- `--ip <vm-ip>` `--port`
- `--proximus-url <url>`
- `--vmp-http` (= force HTTP mode pour legacy accounts)
- `--session-id` `--vm-uuid` `--vm-session-id` `--user-uuid`

## The bootstrap state machine (the mandatory steps)

```
1. /shadow/auth_login
2. /shadow/vm/start (X-Vm-Id header)
3. /shadow/vm/ip → host, port, proximus_url
4. /shadow/vm/proximus-credentials [clientType×3] → tokens
5. POST <proximus_url>/clients (launcher, main, usb) → ids + spice/streamingtoken
6. GET <proximus_url>/stream (an SSE long-poll keepalive)  ⚠ the Switch skips this step!
7. POST <proximus_url>/forward {data:{type:"hello"}} ← perhaps implicit
8. GET /status → check streamer_up
9. ⏯ Spawn ShadowPCDisplay → opens binary CtrlChanV2 stream
```

## Fichiers analyse

- `$REPO/03-shadow-recon/overnight/REPORT.md` (683 lignes, 33 KB)
- `$REPO/03-shadow-recon/overnight/TODO.md`
- `/tmp/shadow_versions/extracted/{linux,mac,win}-{prod,beta}/` — every extracted version
- `/tmp/lin-beta-asar/release/main/main.js` — launcher Electron decompiled
- `/tmp/sslkeylog2.so` — an LD_PRELOAD hook that works on curl but not on Shadow (libcurl is internal there)

## Build status nuit 2026-05-03

- Last md5 : `5f82d7919e30164112ea8185253b7d0c`
- The Switch was disconnected overnight; the push is to be done after a reboot
- Code changed: launcher.c (the body), proximus.c (X-Shadow-Agent), ctrl_rest.c (/5/), ctrl_tcp.c (AES_128 cipher first)
