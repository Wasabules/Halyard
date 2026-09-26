---
name: project_state_polling_endpoints_RE
description: TIER10-Y3 — No state polling during streaming. /1/status is hit once at bootstrap (vm_status/reachable/streamer_up). 7 bootstrap GETs in total, then REST silence.
metadata:
  type: project
  confidence: C95
  date: 2026-05-23
  tier: 10
---

# TIER 10 Y3 — Periodic state polling endpoints

## TL;DR
**No periodic polling during streaming.** Over 235 s of the MASTER capture, we
sees exactly **4 `/1/*` GETs at bootstrap** then complete silence (= 0 REST
GET during the whole stream, apart from the /1/stats uplink telemetry POSTs).
**The "the server gates multi-NAL on health-check polling" hypothesis is REFUTED at C95.**

## REST GET inventory exhaustif (235s capture)

| EA capture | Endpoint | Time | Purpose |
|------------|----------|------|---------|
| L11940 | `GET /1/clients` | bootstrap t=0 | List active clients |
| L17898 | `GET /1/version` | bootstrap | Server version |
| L18047 | `GET /1/stream` | bootstrap | Active stream info |
| L18320 | `GET /1/status` | bootstrap | **VM health probe one-shot** |

And the POSTs (= already covered in TIER 9 / TIER 10 Y1):
- 1× `POST /1/clients` (= register client, bootstrap)
- 13× `POST /1/stats` (= telemetry batch uplink)

**ZERO runtime GET during streaming.** No `/1/status` polling, no
`/vm/state`, no `/session/heartbeat`, no `/health`.

## `/1/status` response decoded byte-exact

```
HTTP/1.1 200 OK
Server: nginx
Content-Type: application/json; charset=utf-8
Content-Length: 104

{"meta": {"version": "7.7.1"},
 "data": {"vm_status": "started",
          "reachable": true,
          "streamer_up": true}}
```

**Purpose** : sanity-check VM readiness avant ouvrir `:base+11` TCP+TLS ctrl.
**Polled** : 1 fois unique pendant 235s.

Cross-check binary : strings `vm_status`, `reachable`, `streamer_up` :
- `reachable` (the string at EA 30210) — used in pingable_channel.cpp (= TIER 10 Y2 §strings 34141)
- `streamer_up` — variant des response handlers

## Strings dump exhaustif state-endpoints

| String | EA | Notes |
|--------|----|----|
| `/1/status` | 41533 | endpoint path |
| `/1/version` | 41534 | endpoint path |
| `/1/stream` | 41530 | endpoint path |
| `/1/clients` | 41532 | endpoint path |
| `/forward` | 41528 | an endpoint (unused this session) |
| `/devices` | 41531 | USB devices (unused) |
| `/renderer/batch` | 41527 | the renderer batch endpoint (unused) |
| `/stream` | 41530 | (canonical) |

**ABSENT** :
- `/vm/status` — ZERO match
- `/session/state` — ZERO match
- `/clients/status` — ZERO match
- `/session/heartbeat` — ZERO match
- `/health` — ZERO match
- `/ping` — ZERO match
- `/keepalive` — ZERO match

## The SSE channel = the other state-notification channel

Cross-ref TIER 8 U1: the SSE stream (`/proximus-sse`) is the **only channel**
server→client for state pushes during streaming:
- `shadow-manager.encoding_is_ready` (= 1 event observed)
- `l2tp.*`, `vm-reachable.*`, `bsod.*`, `get-out.*` — non-vus cette session

**No REST polling. State notification = SSE-driven.**

## Cross-ref Switch implementation

`shadow/launcher.c` + `shadow/proximus.c` :
- ✅ GET `/1/clients` (= bootstrap discovery)
- ✅ POST `/1/clients` (= register client)
- ✅ GET `/1/version` (= sanity)
- ❌ **NO** GET `/1/status` at bootstrap (= missing!)
- ✅ SSE `/proximus-sse` (= dual launcher + main JWT)

**Candidate action C50**: add a GET `/1/status` at bootstrap before opening
`:base+11`. It could diagnose a case where we knock too early (= before the
`streamer_up=true`). But **not a taskbar trigger** — just a guard against
race-condition bootstrap.

## Verdict Y3
**REFUTED at C95**: no REST endpoint is polled during streaming. The only
the runtime server→client channel = the SSE (= already covered in TIER 8) + the ctrl-msgs
binaires `:base+11` (= kRequestFlush, kNotifyVideoCommand cf TIER 6 M1).

The "an 'I'm alive and watching' poll gates the server's multi-NAL" hypothesis is **dead**.

## Refs Y3
| Artefact | Path / Line |
|----------|-------------|
| Capture GET /1/clients | `tls_plain.log:L11940` |
| Capture GET /1/version | `tls_plain.log:L17898` |
| Capture GET /1/stream | `tls_plain.log:L18047` |
| Capture GET /1/status | `tls_plain.log:L18320` |
| /1/status response body | `tls_plain.log:L17898+SSL_READ 527B (200 OK, body 104B)` |
| Endpoint strings | `dumps/strings_display.txt:41527-41535` |
| pingable_channel src | `dumps/strings_display.txt:34141` |
| Cross-ref TIER 8 U1 (SSE) | `memory/project_sse_event_stream_RE.md` |
| Cross-ref TIER 8 U2 (REST) | `memory/project_rest_during_streaming_RE.md` |
