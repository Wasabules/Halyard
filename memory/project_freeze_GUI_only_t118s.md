---
name: GUI-only freeze — a deterministic server close at t≈118s (the native path)
description: 2026-08-21 RESOLVED — /N/status is not an SSE stream but a POLLING endpoint (an immediate 200); sse_keepalive_thread did only one curl_easy_perform, so nobody re-polled and the server closed ~118 s later. Fix D9 = a reconnection loop.
type: project
---
## RESOLVED (D9)

**Root cause**: the 2nd SSE targeted `/N/status`. But **`/status` is not an SSE
stream, it is a JSON endpoint** (`proximus.h:44`; the SSE is `/stream`,
`proximus.h:70`): it answers `HTTP 200` and terminates immediately. And
`sse_keepalive_thread` did only **one `curl_easy_perform`**: at the first return
the thread exited and nobody re-polled. The server, whose binding rests on the
DUAL SSE, closed the session ~118 s later.

**The clean fix (D10)**: the GUI opens `/stream` for both JWTs → **t=225 s, 0
closures, 0 reconnections**. The comment justifying `/status` as "confirmed
through an LD_PRELOAD hook on 2026-05-09" was FALSE and steered the code for ~3
months; the capture shows `/1/status` only once, as a plain state check.
**The safety net (D9)**: a reconnection loop, correct for a real SSE that drops.
With `/status` it kept the session alive at 306 polls / 225 s, all at
`rc=0 http=200` — which is what proved the endpoint's nature. With D10 it never
fires.

**Why the headless build did not see it**: `main_test.c` opens `/stream` for both
JWTs — a real long-lived stream. Its survival came from a divergence from the
official client, not from a better implementation. That is what masked the bug.

**Methodological lesson**: surviving headless proves nothing about the GUI as long
as the two do not open the same endpoints — `main_test.c` was right by accident.

## The fact (the investigation)

On the **native path**, the Shadow server deliberately closes the `:base+11`
control channel (`wolfSSL_read rc=0 err=-397` = `SOCKET_PEER_CLOSED_E`, a clean
TLS FIN) then refuses the `:base+10` UDP (`errno=111` ECONNREFUSED). The last
exchange before the cut is a normal `send 90B / recv 139B`: no warning sign.

**In the GUI only.** Measurements:

| mode | survived | VM |
|---|---|---|
| GUI | 118 s | gpu-mature-image (base 9000) |
| GUI | 108 s | gpu-warmth-ham (base 11000) |
| GUI | 118 s | gpu-sturdy-yellow (base 11000) |
| headless ×4 | 195 / 195 / 295 / 395 s | gpu-mature-image (base 9000) |

The 4 headless runs that survive run on **the VM that kills the GUI** → the VM is
ruled out. `cuda hwdevice created` in all 6 runs → **NVDEC is ruled out**.

## What is refuted (do not replay)

1. **A session lease** (D3 `SHADOW_REG_PERIOD_MS`) — the desktop sends
   RegisterSession 8×, we send it once. A/B: the arm WITHOUT renewal survives
   195 s. Refuted.
2. **Malformed input events** (D5 `SHADOW_INPUT_SYNTH_MS`) — 581 events injected
   headless (against 26 in the dead GUI): no closure. Refuted.
3. **Heartbeat starvation** — the GUI only delivers 1.15 Hz instead of N48's 2 Hz.
   `SHADOW_HB_PERIOD_MS=200` → 4.10 Hz delivered, and it still died at 108 s.
   Refuted.

## What is measured (GUI vs headless, identical VM)

- reassembly holes **0.424/s vs 0.127/s** (×3.3)
- IDR requests **0.068/s vs 0.020/s** (×3.4 — the IFR being event-driven since G6,
  it *measures* the loss)
- heartbeat 1.15 Hz vs 1.75 Hz; identical inbound rate (409 vs 425 pkts/s)

Reading: decoding + the GL upload run on the thread that drains UDP → rendering
starves reception. **That explains the loss, not the closure** (cf. refutation 3).

## The remaining lead

The IDR rate is the only thing the GUI emits markedly more of and that has not
been neutralised. `SHADOW_IFR_PERIOD` does not cover it (G6 is event-driven): it
needs a hard cap to test. A structural fix independent of the freeze: move
decoding/rendering off the session thread.

## Corrects an earlier finding

[[project-freeze-t115s-SOLVED-switch-specific]] concluded "a Switch-specific bug,
the same code runs 245 s+ on Linux with no freeze". False on the native path: the
Linux GUI dies at 118 s. That note remains valid for May's WebRTC path, not
beyond.

## Refs

- `KB.md §3.17` (the source of truth), §3.16 (the "occasional UDP loss" there is a
  distinct phenomenon)
- `ctrl_tcp.c::is_peer_closed / mark_peer_closed`, `ctrl_session.c` (D1/D2/D3/D5)
- Collateral: `comchan` fails 5/5 on `:base+14` (a 2nd TLS connection on a port
  already taken), `audio-dtls` fails 5/5 on `:base+12` = no sound, a distinct
  cause, not elucidated.
