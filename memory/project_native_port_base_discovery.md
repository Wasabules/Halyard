---
name: 🎯 The native Shadow port_base is dynamic per VM (an extended LD_PRELOAD)
description: 2026-05-09 — The extended hook (connect/send/recv/write/read) confirms that TCP :9011 = SslCtrlChanV2 on the user's VM, and that the base port varies per VM (9000/10000/13000 seen). There is NO simple slot×1000+offset formula.
type: project
---
## TL;DR

DECISIVE 2026-05-09: extending `shadow_tls_hook.so` with `connect/send/recv/write/read` hooks exposed that:

1. **The official desktop app DOES open TCP `:9011`** on the current user's account (= confirmed by `CONNECT fd=249 peer=<VM public IPv6> type=TCP rc=-1`).
2. The port_base **varies per VM**; observed: 9000, 10000, 13000.
3. Our hardcoded `:13011` is wrong — we must discover it dynamically.
4. The byte-exact wire format is confirmed: `[u16 type][u16 len][protobuf]` directly over TCP+TLS on port `port_base+11`.

**Why**: for 6 days we were blocked on a `:13011 timeout` because that particular VM uses base=9000 (hence :9011), not base=13000. The shadow-test-cli + shadow-client GUI code hits :13011 → a systematic TCP timeout → the bootstrap fails.

**How to apply**: before any SslCtrlChanV2 TCP connect, **probe** the candidate `port_base + 11` ports for the known bases: `9000, 10000, 11000, 12000, 13000`. The first that accepts wins. Every other streaming port derives from it: `port_base + 10/12/13/14/15/20/30/32`.

## A complete inventory of the ports observed (the user's capture, 2026-05-09, instance=2)

| Port | Type | Usage |
|---|---|---|
| `:9010` | UDP | video chacha20 |
| **`:9011`** | **TCP** | **SslCtrlChanV2 control** |
| `:9012` | UDP | audio DTLS+Opus |
| `:9013` | UDP | input chacha20 |
| `:9014` | TCP | forward channel ? |
| `:9015` | TCP | ? |
| `:9020` | TCP | ? |
| `:9030` | UDP | cursor chacha20 |
| `:9032` | UDP | ? |

## HTTP REST endpoints (the desktop app's sequence)

```
GET    /<inst>/clients          → liste les clients existants (zombies inclus)
DELETE /<inst>/clients/<id>     → cleanup zombies (sien d'avant)
POST   /<inst>/clients          → creates a NEW main client (fetches the streamingtoken)
GET    /<inst>/version          → a version ping
GET    /<inst>/stream           → an SSE long-poll (a dedicated fd)
GET    /<inst>/status           → an SSE long-poll (ANOTHER fd, in parallel)
POST   /<inst>/stats            → telemetry JSON (renderer.event entries)
                                 PUIS connect TCP :<port_base+11> direct + TLS + SslCtrlChanV2
```

`<inst>` = JWT.instance, varie par session (vu /2/, /3/, /6/).

## Key points found through the extended hook

- **fd=249** on `:9011` does: `TCP_WRITE 347B` (the Capabilities request) → `TCP_READ 1224B` (the reply, all in clear under TLS) → then Auth/Encryption/Register.
- **`pid=34406`** = ShadowPCDisplay (the renderer process) opens the streaming sockets, not the Electron launcher at pid=33864.
- The desktop app makes NO `connect()` to :13011 — so our approach was pure speculation based on an earlier note that merely observed it working byte-exact in another situation (a different VM).

## Refs

- Hook source : `$REPO/06-shadow-recon-linux/tls_hook/shadow_tls_hook.c`
- Log capture : `/tmp/shadow_tls.log` (~6.7 MiB session live 2026-05-09)
- Backup ancienne capture : `/tmp/shadow_tls_v1.log`
- Cross-ref: `project_native_bootstrap_VALIDATED.md` (= obsolete on the `:13011` port), `project_shadow_real_protocol_confirmed.md`

## Resulting action items

- N7 (this) — DONE : sauvegarde finding
- N8 — decode where port_base comes from (probably the JWT.ports field, or the /N/clients response); failing that, implement a probe
- N9 — the missing REST sequence (DELETE zombies + version + the status SSE + stats)
- Change `ctrl_session.c` to port-probe instead of hardcoding `:13011`
