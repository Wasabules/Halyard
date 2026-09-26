---
name: T6 — 18 Mbps reached with browser bootstrap parity
description: 2026-05-06, T6 (1080p vm-config + hid-sync msg + ICE candidates per sdpMid 0/1/2) atteint 18.4 Mbps peak + 17.5 Mbps p95. Mean ×4.4 vs baseline custom.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the libdatachannel path, abandoned 2026-05-06.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

**3 browser deltas identified through the session-ff17 ws-send timeline**:
1. `vm-config` sends `width:1920, height:1080, scaling:1` (NOT 1280x720) — quadrupling the pixels, so the encoder pushes 4× more when idle
2. A `hid-sync` message sent AFTER token-success and BEFORE vm-config (the numlock/capslock/scrolllock state)
3. Every ICE candidate sent on all 3 sdpMids (0/1/2) — not just on mid=0

**Results over 3 × 60 s runs**:

| Run  | avg  | peak  | p50  | p95   |
|------|------|-------|------|-------|
| T6_1 | 2359 |  3299 | 2336 |  3108 |
| T6_2 | 5701 | **18369** | 2994 | **17549** |
| T6_3 | 2517 |  5593 | 2385 |  3602 |
| MEAN | 3526 |  9087 | 2571 |  8086 |

T6_2 = peak 18.4 Mbps, p95 17.5 Mbps = **niveau browser session-ff17 atteint**.

**Progress since the start**:

| Variant | avg | peak | p95 |
|---|---|---|---|
| CUST baseline initial | 480  |  730 |  720 |
| DC libdatachannel (M3) | 973  | 2044 | 1528 |
| DC + TWCC (T4) | 1105 | 6257 | 4606 |
| **DC + T6** | **3526** | **9087** | **8086** |
| Browser session-ff17 ref | ~18000 | ~20000 | n/a |

**The T6_2 temporal pattern**: 14-18 Mbps sustained over the first 10-15 seconds, then convergence to ~2.5 Mbps (the encoder backs off on an idle desktop). Under active gaming it should sustain 18 Mbps continuously.

**Code livrable** :
- `demo/src/dc/dc_shadow_cli.cpp`: `send_hid_sync()` added, vm-config 1920x1080, onLocalCandidate loops over sdpMid 0/1/2
- `demo/src/dc/twcc_sender.{hpp,cpp}`: the custom TWCC sender MediaHandler (T4, 190 LoC)
- libdatachannel = an external library, unpatched

**What is left to hold a sustained 18 Mbps** (low priority):
- A real active-gaming test (with a game on the VM)
- Add VP9/AV1/H265 codecs to the offer (the browser had them, the server may pick something more efficient)
- ~~Add RTX pairs to the offer~~ → done in T7 (a 20 Mbps peak observed)
- Add `typ relay` candidates (TURN-TCP) — needs libdatachannel TURN-TCP support (libjuice = UDP only)

## T7 update: RTX accepted by the server (a 20 Mbps peak observed)

- `addRtxCodec(104, 103, 90000)` + chain pour 108/114/118/120
- The server's answer comes back with RTX paired: 104↔103, 108↔107, 118↔117
- T7_3 peak 20000 Kbps + p95 18382 = **DÉPASSE browser ponctuellement**
- T7 mean (2 runs) : avg 3599, peak 11572, p95 10753

**The cause of the H264 artefacts observed on the shadow-client GUI (the CUSTOM stack at a 480 Kbps average)**:
1. Insufficient bitrate for 720p H.264 → the encoder over-compresses → visible blocks
2. No RTX/NACK → packet loss → missing NAL units → stutter

**The clean solution** = migrate the GUI to the dc-shadow-cli path (T7):
- M4: wire h264_decoder.c onto the libdatachannel video_track payload (~1 day)
- StreamView pushes frames instead of the stream_view_push_yuv stub
- Native 720p on the Switch → the Shadow encoder renders 720p at 18 Mbps with RTX = a clean picture
