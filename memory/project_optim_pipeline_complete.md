---
name: The libdatachannel optimisation pipeline — the full summary
description: 2026-05-06, an exhaustive list of latency/quality optimisations for the shadow-client GUI on libdatachannel. Every lead explored + the verdicts.
type: project
---
## Executive summary

The Linux shadow-client GUI has been migrated to libdatachannel + a custom layer (TWCC, NACK, RTX, the M4 h264 bridge, M5 inputs, M6 RX). For 720p on Switch, the stack has everything needed for a clean picture and low latency.

## Optimisations applied

| ID | Description | Effet | Status |
|---|---|---|---|
| **T2** | Multi-PT H.264 (5 PTs) with differentiated profiles | The server picks 4 PTs instead of 1 | ✅ |
| **T4** | The TwccSenderHandler MediaHandler (190 LoC) | The server's BWE unblocked (×3 peak) | ✅ |
| **T5** | profile-level-id per PT (baseline/constrained/main/high) | The server can pick the high profile | ✅ |
| **T6** | vm-config 1920×1080 + the hid-sync msg + ICE per sdpMid | The encoder allocates full throughput (an 18.4 Mbps peak) | ✅ |
| **T7** | RTX pairs PT 104/108/114/118/120 | The server is ready to retransmit | ✅ |
| **L1** | `AV_CODEC_FLAG_LOW_DELAY` + `FAST` + `has_b_frames=0` | -16-33ms decode latency | ✅ |
| **L2** | Input drain queue 60Hz → 240Hz | Mouse responsiveness ×4 | ✅ |
| **N1** | NackSenderHandler (RTPFB FMT=1 Generic NACK) | Active vraiment RTX recovery | ✅ |
| **M4** | h264_decoder bridge libdatachannel | Pipeline RTP → YUV → display | ✅ |
| **M5** | DcInputBridge (sctp_send_msg → DC.send) | Inputs TX fonctionnels | ✅ |
| **M6** | DC RX callbacks + counters | The cursor/clipboard infrastructure is ready | ✅ |

## Leads tested with no measurable gain

| Tweak | Pourquoi pas | Action |
|---|---|---|
| **B1 VP9/AV1/H.265 codec offer** | The server picks H.264 anyway (verified in the 2026-05-06 latency test) | The code stays in place, no harm |
| **REMB requestBitrate(50 Mbps)** | The server ignores our REMB target | Kept, since it is free |
| **Disabling the transport-cc extmap** | The server was expecting TWCC → it throttled worse | Re-enabled in T4 |

## Leads eliminated (not actionable or not relevant)

| Piste | Raison |
|---|---|
| **A TURN-TCP relay** | A residential user has direct UDP working; libjuice = UDP only |
| **playout-delay min/max** | The extension is sender→receiver, not configurable on the receiver side |
| **The libdatachannel jitter buffer** | libdatachannel has NO application-level jitter buffer (RTP is delivered directly) |
| **HW decode with NVDEC on Linux** | The GPU→CPU transfer path is not wired, and CPU decode is already <5 ms at 1080p |
| **The `sps-pps-idr-in-keyframe` quirk** | Already applied in patch_sdp_for_shadow |

## Current pipeline latency (estimated)

```
Mouse Borealis → drain queue          (~4ms — L2)
              → DC.send libdatachannel (~1ms)
              → DTLS encrypt + UDP    (~1ms)
              → Network 1way RTT/2    (~10-30ms selon distance VM)
              → Server decode + apply (~1ms)
              → the encoder renders a frame  (~16 ms at 60 fps)
              → RTP UDP send          (~1ms)
              → Network 1way          (~10-30ms)
              → libdatachannel → cb   (~1ms)
              → libavcodec decode     (~3-5ms CPU low-delay flags)
              → stream_view_push_yuv  (~1ms)
              → the Borealis display vsync (~16 ms at 60 Hz)
                                       ─────────
                                       Total ~60-100ms
```

That is within the standard WebRTC range. The browser under the same conditions = the same range.

## Plafonds physiques restants

- **18 Mbps maximum**: what the Shadow encoder allocates at 1080p high profile + a b=AS:45000 hint. It cannot push more on the server side.
- **~60 ms of base latency**: the Shadow encoder (16 ms) + 2× the network (~20-60 ms) + decode (~5 ms) + display (16 ms). The network and the display are OS-bound.
- **H.264 only**: the Shadow encoder only emits H.264. VP9/AV1, even when offered, are ignored.

## Deliverable code, recap

```
demo/src/dc/
├── dc_session.{h,cpp}     — API publique + impl session libdatachannel
├── twcc_sender.{hpp,cpp}  — T4 TWCC sender custom
├── nack_sender.{hpp,cpp}  — N1 NACK sender custom (RTPFB FMT=1)
├── dc_input_bridge.{hpp,cpp} — M5 sctp_send_msg → DC.send dispatch
├── dc_smoke.cpp           — sanity test
└── dc_shadow_cli.cpp      — CLI bench harness

shadow-client GUI utilise dc_session_run() depuis connecting_activity.cpp.
Custom WebRTC stack (webrtc.c, ice.c, sctp.c, srtp.c, dtls.c) exclu du build.
shadow_input.c, shadow_controller.c, h264_decoder.c, audio.c and stream_view.cpp = unchanged.
```

## Nothing left to optimise without a trade-off elsewhere

Every reasonable lead has been either applied or tested. Any further gain would need:
- **Migrating to full libwebrtc** (1-2 months, out of scope) — the same results as the custom stack (TWCC/NACK already implemented by hand)
- **Reduce the vsync display** (16 ms→8 ms) — needs a 120 Hz Switch dock
- **HW decode rendering direct GPU** (skip transfer) — refonte du chemin display

The current state = **ready for the Switch port (M2)**.
