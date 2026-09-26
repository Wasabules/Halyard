---
name: project-rest-during-streaming-RE
description: TIER 8 U2 — the REST endpoints during a stream = telemetry only, not a taskbar trigger.
metadata:
  type: project
---

# TIER 8 U2 — REST endpoints during streaming

**Date** : 2026-05-23
**Capture** : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log`
**Outil** : `tools/ida/tier8_http_endpoints.py`

## TL;DR

**C95: no new protocol-critical REST endpoint during the stream**.
Desktop fait 59 HTTP requests total (235s) :
- 40× `/logs-renderer-*/_bulk` (= ElasticSearch dev logging)
- 12× `/1/stats` (= renderer.event telemetry batched)
- 7× endpoints bootstrap (= `/1/clients`, `/1/stream`, `/1/version`, `/1/status`, sentry)

All of them uplink-only. The server never reads those JSONs to modulate the stream.
Our Switch does NOT make them, **and that is fine** — not a taskbar fix.

## Inventory complet

| Method | Endpoint | Count | Phase | Effet streaming |
|--------|----------|-------|-------|-----------------|
| POST | `/logs-renderer-2026.05.14/_bulk?pretty` | 40 | Mixed | Aucun (ELK upload) |
| POST | `/1/stats` | 12 | Mixed | Aucun (telemetry) |
| POST | `/api/{N}/envelope/` | 1 | bootstrap | Aucun (Sentry crash) |
| GET | `/1/clients` | 1 | bootstrap | Bootstrap step |
| DELETE | `/1/clients/{id}main` | 1 | bootstrap | Cleanup stale client |
| POST | `/1/clients` | 1 | bootstrap | Register client |
| GET | `/1/version` | 1 | bootstrap | VM version probe |
| GET | `/1/stream` | 1 | bootstrap | Stream info fetch |
| GET | `/1/status` | 1 | bootstrap | Status probe |

## `/1/stats` body decode

```json
{"data": [
  {"name": "renderer.event",
   "values": {"event": "state_changed",
              "new_state": "Starting", "old_state": "NoState",
              "step_time_ms": 0, "type": "renderer"},
   "metadata": {"renderer-mode": "RELEASE",
                "renderer-version": "12.3.3",
                "session_id": "<uuid>",
                "video-pipeline": "{...selected: {chroma, codec, decoder, mode, paint}}"}}
]}
```

= batched **renderer.event telemetry**. Pure uplink, no feedback.

## `/logs-renderer-*` body decode

```
POST /logs-renderer-2026.05.14/_bulk?pretty
Host: <a Shadow telemetry host>:9200   ← direct ElasticSearch (address withheld)
Content-Type: application/x-ndjson

{"index":{...}}
{"@timestamp":"...","level":"INFO","message":"...","session_id":"..."}
```

= **direct ES log dump**. Dev-only, jamais en prod.

## Strings binary URLs hardcoded

Every `/1/*` and `/api/*` confirmed as already catalogued in `KB.md §4.x` +
`demo/src/shadow/http.c`. **Aucun nouveau endpoint** (= no `/sessions/refresh`,
`/heartbeat`, `/health`, `/streaming/status`).

## Verdict

**C95: REST during a stream = uplink telemetry only**. No taskbar cause
here. No code to write.

A possible hygiene item = implement `/1/stats` for Shadow's operator dashboard,
but it has no effect on the stream.

## Refs

- Script : `tools/ida/tier8_http_endpoints.py`
- `/1/stats` body @L18674 capture MASTER
- `/logs-renderer-*` @L3977 capture MASTER
- Doc : `tools/ida/out/TIER8_RE_2026-05-16.md` §U2
- KB existing : `KB.md §4.x` REST endpoints
