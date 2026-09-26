---
name: The PIVOT to the native Shadow protocol — the 2026-05-06 decision
description: WebRTC/libdatachannel ABANDONED after ~1 week of optimisation; the t=120 s freeze ceiling + fps variance. A pivot to the native desktop app (TCP/443 h2 + UDP chacha20).
type: project
---
## The decision (2026-05-06 ~20:00)

After ~1 week of libdatachannel migration + 30 optimisations (T*, L*, M*, N1):
- The server-side t=120 s freeze bug was never resolved (28 hypotheses tested, none successful)
- Large fps variance (oscillating 15-78 fps) despite the pacing buffer
- An unpredictable server BWE (a variable cap, 500 kbps – 8 Mbps at 720p)
- Acceptable latency, but a GPU pipeline tax on the Intel UHD

The user confirms: **the Linux Shadow desktop app works perfectly with their legacy account** (`JWT.ports=null`). So the native Shadow protocol accepts legacy accounts too → the JWT.ports blocker is ruled out.

## Why
WebRTC is a standard, which is tempting, but Shadow has probably modified/constrained it server-side in a way incompatible with a custom non-Chromium client. The native app = what Shadow optimises for.

## How to apply
- **The WebRTC stack** (custom + libdatachannel) to be kept in the repo for the archive but **disabled** in the build
- A pivot to `demo/src/streaming/` (already ~70% written, see below)
- The shadow-client GUI must use a new `ctrl_session_run()` instead of `dc_session_run()`

## State of the native port at the time of the pivot

`demo/src/streaming/` (2953 LoC already committed):

| Module | LoC | État | Fonction |
|---|---|---|---|
| `ctrl_rest.c/h` | 210/66 | ✅ | POST /N/clients REGISTER, DELETE unregister |
| `ctrl_tcp.c/h` | 459/77 | ✅ | TCP+TLS:443 open, send/recv cleartext + encrypted, path framing |
| `ctrl_msgs.c/h` | 269/98 | ✅ | Protobufs Authentication, Encryption request/reply |
| `encryption.c/h` | 143/66 | ✅ | chacha20-poly1305 wire `[ct\|nonce12\|tag16]` |
| `sufp.c/h` | 230/85 | ✅ | SUFP chunks 11B header framing |
| `proto.c/h` | 146/82 | ✅ | Protobuf varint encode/decode |
| `shadowusb.c/h` | 301/73 | ✅ | Tunnel gamepad WSS+Spice+usbredir (separate daemon) |
| `usbredir.c/h` | 141/98 | ✅ | usbredir frame format |
| `smoke_test.c/h` | 358/51 | ✅ | M32 bootstrap test (TCP+Auth+Enc OK) |

## WebRTC pieces to KEEP (already well polished)
- `audio.c`: the Opus decoder + ALSA/audoutAppendAudioOutBuffer — reusable
- `h264_decoder.c`: libavcodec + low-delay flags + an NV12 output target — reusable
- `stream_view.cpp`: the pacing queue, the NV12 GPU shader path, a 240 Hz cursor poll — 100% reusable
- `wss.c`: signaling auth (may still serve if the native path negotiates through WSS too)
- `log.c`: logging persisted to `/tmp/shadow-client/webrtc.log` (to be renamed)

## WebRTC pieces to THROW AWAY
- `webrtc.c, ice.c, sctp.c, srtp.c, dtls.c` (the custom stack — already excluded from the build)
- `dc/dc_session.cpp, dc/twcc_sender.cpp, dc/nack_sender.cpp, dc/dc_input_bridge.cpp` (the libdatachannel layer)
- `library/libdatachannel/` (vendored, ~XXX MB)

## Plan macro post-pivot

| Milestone | Description | Estimate |
|---|---|---|
| **N0** | A full audit of smoke_test.c — which candidate_path gets through → identify the real binary endpoint | 1-2 h |
| **N1** | Carry the bootstrap on past Encryption: the Capabilities request/reply, channel setup | 2-3 h |
| **N2** | Receiving SUFP-encrypted H.264 video frames → reuse h264_decoder.c | 3-4 h |
| **N3** | Receiving encrypted Opus audio → reuse audio.c | 1-2 h |
| **N4** | Inputs (mouse/keyboard) in the native wire format → an SUFP send | 2-3 h |
| **N5** | A public `ctrl_session_run()` API (analogous to dc_session_run) | 1-2 h |
| **N6** | Wire GUI : `connecting_activity.cpp` switch dc_session_run → ctrl_session_run | 1h |
| **N7** | Cleanup : remove libdatachannel + dc_session/twcc/nack/dc_input_bridge | 1h |
| **N8** | A 5+ minute test with no freeze (the t=120 s bug hopefully gone) | 30 min |

**Total estimate: ~14-20 h** (the old estimate in `project_streaming_port_progress` said ~18 h for the remaining wiring — consistent).

The RE can continue in parallel if we discover unexpected protocols.
