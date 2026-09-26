---
name: project-bandwidth-probe-RE
description: TIER9-W1 — the Shadow desktop has a speedtest-url + a traceroute capability but the result is sent to /1/stats PUBLIC only, never to the ctrl wire. The "the server gates multi-NAL on bandwidth" hypothesis is REFUTED at C90.
metadata:
  type: project
---

# TIER 9 W1 — Bandwidth probe / Initial speed test RE

## Verdict C90 — the capability exists but is never activated and never on the wire

The Shadow desktop has **TWO distinct** network-probe mechanisms, **both
client-side telemetry uniquement** :

### 1. `speedtest-url` CLI capability — opt-in, NON-active dans capture

**Strings** :
- `strings_display.txt:27789` = "Speedtest URL" (= help text)
- `strings_display.txt:27790` = "speedtest-url" (= CLI flag name)
- `strings_display.txt:29051` = "speedtest.{}.shadow.{}" (= URL template,
  = speedtest.<region>.shadow.tech, classique speedtest.net-style)

**Capture analysis** : `grep -nE "speedtest|/speed|/bandwidth|/iperf" tls_plain.log`
→ **0 matches** in the whole 235 s MASTER capture. The user of that session
did not trigger the speedtest. Probably opt-in through a launcher button "Run
network test", or automatic on a connectivity change.

### 2. `TracerouteUpdater` — runtime, ACTIVE dans capture

**Strings** :
- `strings_display.txt:31174` = `../udu/src/TracerouteUpdater.cpp` (= source)
- `strings_display.txt:31186` = `periodicTracerouteUpdate(chrono::duration<l,ratio<1,1>>, string)`
- `strings_display.txt:31187` = `startTraceroute(string)`
- `strings_display.txt:31188` = `onTracerouteResult(json)`
- `strings_display.txt:31960` = `Try poor traceroute to {}:{}`
- `strings_display.txt:31966` = `udu/deps/framework/src/common/traceroute/src/network.cpp`
- `strings_display.txt:31969` = `Start poor traceroute async with id {}`

"Poor traceroute" = the fallback path when a raw socket is unavailable (= it uses
TTL probing via UDP + ICMP unreachable responses, classique RFC 792
implementation by hand).

**Capture analysis** :
- L46127 / L156306 / L694597 = log DEBUG `"...but traceroute not needed"`
  (= skipped because it is recent, within the cooldown)
- L206951-L207010 = `POST /logs-renderer-*` body avec `"log.origin.file.name":"TracerouteUpdater.cpp"`,
  `"function":"onTracerouteResult"`, `"message":"Traceroute result (...): [1] 0.3297ms -> [end] 9.454ms"`
- L316783-L316900 = `POST /1/stats` event `"name":"renderer.network_traceroute"`
  avec values `{"first_hop_average_latency_ms":0.3297,"last_hop_average_latency_ms":9.454,"result":[...hops...]}`

### Verdict W1 — telemetry-only, jamais wire ctrl

**C90**: the runtime traceroute exists and runs (= 2 events over 235 s, plus
several intermediate "not needed" log lines) but the result is sent
ONLY to `/1/stats` (= PUBLIC privacy, unread by the streaming server,
cf TIER 8 U2 verdict).

The speedtest-url is an **opt-in capability**, not active in this
session. If enabled, a GET probably towards `speedtest.<region>.shadow.tech`
avec known-size payload + timing.

**The "the server gates multi-NAL on the bandwidth-probe result" hypothesis is REFUTED at C90.**

Our Switch client can ignore W1 entirely — there is no signal to emit,
no protobuf field to advertise. The Shadow server **never receives**
of a "bandwidth class" / "speed test result" from the client.

## Refs

- Strings : `06-shadow-recon-linux/dumps/strings_display.txt:27789-27790, 28914, 29050-29051, 29274, 31174-31188, 31960-31973`
- Capture traceroute event : `tls_plain.log:L206867-L207010 (POST /logs-renderer)`
- Capture network_traceroute stat : `tls_plain.log:L316783-L316900 (POST /1/stats)`
- TIER 9 doc : `tools/ida/out/TIER9_RE_2026-05-16.md` §W1
- Cross-ref TIER 8 U2 REST endpoints : `memory/project_rest_during_streaming_RE.md`
