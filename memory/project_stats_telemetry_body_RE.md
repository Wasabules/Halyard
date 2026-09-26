---
name: project_stats_telemetry_body_RE
description: TIER10-Y1 — The POST /1/stats body schema decoded byte-exact. 13 batches over 235 s, 28 unique event types, 100% privacy:PUBLIC ELK telemetry. The server's reply = an empty {}. Not a taskbar trigger.
metadata:
  type: project
  confidence: C95
  date: 2026-05-23
  tier: 10
---

# TIER 10 Y1 — `/1/stats` telemetry body deep decode

## TL;DR
**13 POST `/1/stats`** over the 235 s MASTER session (= ~3.6 msgs/min). The body
contient batches d'events `renderer.*` JSON avec `privacy:"PUBLIC"`. **Server
the reply is systematically `{}`, 2 bytes**. **The "the server routes its quality decision
sur stats body" RÉFUTÉE C95.**

## Capture summary

| Metric | Value |
|----------|--------|
| Endpoint | `POST /1/stats` HTTP/1.1 (TLS sur :443) |
| Host | `ipv6-gpu-acumen-dish-…shadow.tech` |
| Content-Type | `application/json` |
| Auth | `Bearer eyJ0eX…` (main_jwt) |
| Body framing | `{"data":[{event1},{event2},…]}` array |
| Server reply | `{}` (Content-Length: 2), systematically |
| Frequency | 13 batches en 235s, espacement 8K-300K lines (variable) |

## The complete event taxonomy (28 unique names, 20 unique event values)

**Top-level `name` field** (= ELK stream routing):
- `renderer.event` — one-off events (state, transitions, network)
- `renderer.network_traceroute` — periodic traceroute (= TIER 9 W1 cross-ref)
- `renderer.streamer_configuration` — bitrate/codec/resolution snapshot
- `renderer.dynamic_quality` — `stabilisation_factor_mean` aggregate
- `renderer.video_overall` / `_session` — percentile latency histogram
- `renderer.video_decode` / `_session` — decoder timing
- `renderer.video_decrypt` / `_session` — decrypt timing
- `renderer.video_reception` / `_session` — UDP reception timing
- `renderer.video_network_session` — network stats aggregate
- `renderer.video_pipeline_session` — pipeline timing
- `renderer.audio_out_*` (8 variants) — audio output timing
- `renderer_overlay.notification_displayed` — UI overlay events
- `renderer_overlay.settings` — UI settings snapshot

**`event` field values (`renderer.event`)**:
- Bootstrap states : `ctrlchan_connecting`, `ctrlchan_connected`,
  `ctrlchan_getting_version`, `ctrlchan_got_version`, `ctrlchan_authenticating`,
  `ctrlchan_authenticated`, `ctrlchan_encrypting`, `ctrlchan_encrypted`,
  `ctrlchan_apply_edid_req`, `ctrlchan_apply_edid_completed`,
  `ctrlchan_start_session`, `ctrlchan_session_registered`
- Runtime : `state_changed`, `video_started`, `network_state` (laggy/unstable/great)
- Config : `streaming_profile_startup` (= TIER 9 W4), `stream_change`,
  `control_rate`, `video`, `fit_to_screen_updated`, `vmproxy_connecting`

## Key fields observed (deep schema)

### `renderer.streamer_configuration` event
```json
{
  "metadata": {
    "audio-in-protocol":"udp",  "audio-out-protocol":"udp",
    "client-role":"mainscreen", "connection_id":"1778765996913",
    "cursor-protocol":"tcp",     "input-protocol":"udp",
    "output_id":"0",             "video-protocol":"udp",
    "vm-uuid":"v-frsbg01-…",     "session_id":"…"
  },
  "values": {
    "bitrate": 20000000,
    "codec": "H264",
    "control-rate": 0.0,
    "event": "stream_change",
    "resolution": "1920x1080 @ 144.028Hz x1 (pos: 0,0)"
  }
}
```

### `renderer.network_traceroute` (= TIER 9 W1)
`{"first_hop_average_latency_ms":0.3297,"last_hop_average_latency_ms":9.454,"result":[{hop,ip,latencies:[…]}]}`

### `renderer.dynamic_quality`
`{"begin_collect":<ts>,"end_collect":<ts>,"stabilisation_factor_mean":0.0}` — a 3-minute `DynamicQualityLegacy` aggregate, see TIER 9 W4.

### `renderer.video_overall` (latency histogram)
Percentiles P0.10 → P99.90 + average/count/min/max over a sliding window.

## Critical findings

### No `multi_nal_supported`/`slice_mode`/feature flag in the body
**C95** : grep exhaustif sur `multi_nal`, `slice_mode`, `nal_split`, `slice_split`,
`enable_split`, `slice_preferred` → **0 matches** across the 13 stats POSTs.

The only close word is `slice-split`, which exists as a BINARY string but is
a **local VAAPI decoder option** (= cf §Y4) — not on the wire and not in the stats.

### Server reply byte-exact identique
All 13 HTTP 200 OK responses return `{}`, 2 bytes. **No X-* header,
no routing signal**. The `/1/stats` endpoint is **fire-and-forget telemetry**
type ELK ingestion.

### Frequency = adaptive, not periodic
The spacing between POSTs varies from 8K to 300K lines (= ~5 s to several minutes), not
periodic. Very probably triggered by a **batch buffer flush** (= 100
events ou 30s, qu'importe).

## Conclusion / verdict Y1
**REFUTED at C95**: the `/1/stats` body is PURE ELK observability. No field
influences the streaming server. The REST API endpoint answers with a
the load balancer / gateway, never consumed by the streaming VM.

Our Switch client can **skip that telemetry entirely** (= already
is currently the case). No missing signal on the wire side.

## Refs Y1
| Artefact | Path / Line |
|----------|-------------|
| Capture POST/1/stats #1 (bootstrap, 9786B body) | `tls_plain.log:L18674` |
| Capture POST/1/stats #2 (5628B) | `tls_plain.log:L30623` |
| Capture POST/1/stats #5 (2861B, traceroute) | `tls_plain.log:L316783` |
| Capture POST/1/stats #11-13 (small batches) | `tls_plain.log:L950983, L997449, L1039866` |
| Server reply `{}` shape | `tls_plain.log:L317091 (Content-Length: 2)` |
| Cross-ref TIER 8 U2 | `memory/project_rest_during_streaming_RE.md` |
| Cross-ref TIER 9 W1/W4 | `tools/ida/out/TIER9_RE_2026-05-16.md` |
