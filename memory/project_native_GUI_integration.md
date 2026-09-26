---
name: The native Shadow stack integrated into the Borealis Linux GUI
description: 2026-05-09 — ctrl_session_glue wired into connecting_activity, the build is OK, toggled with SHADOW_NATIVE=1
type: project
---
DECISIVE 2026-05-09: the native Shadow stack (a TCP :13011 bootstrap + UDP :13010 chacha20 video) is fully integrated into the Linux Borealis GUI. The full build is fine, and there is no longer any link to libdatachannel on that path.

**Why**: the user's goal was "validate the whole stack in the Linux GUI before porting to Switch — no hardcoded key, a clean dynamic negotiation". The native pipeline bypasses WebRTC (which breaks at t=120 s) and gives a robust alternative route.

**How to apply**: to enable the native path in the Linux GUI, run it with `SHADOW_NATIVE=1 ./shadow-client`. Otherwise the code stays on dc_session_run() (libdatachannel). CLI test: `./shadow-test-cli --mode=native-stream --duration=30`.

## Components delivered

- `streaming/ctrl_session.{h,c}` — the complete orchestrator: TCP+TLS `:13011`, Capabilities → Auth (extracts the 20 B hash) → Encryption (extracts the dynamic 32 B key) → RegisterSession → 8 channel announcements → the UDP registers on `:13010`/`:13030`/`:13013`, a recv loop with chacha20 + a Shadow VideoFrame parse + per-timestamp NAL reassembly. The callbacks on_video/on_cursor/on_progress/abort_flag. Polled at a 5 ms granularity.
- `streaming/ctrl_session_glue.{h,c}` — the wrapper plugging the ctrl_session callbacks → `h264_decoder_feed_annexb` → `stream_view_push_yuv` (Borealis) or a no-op (CLI).
- `webrtc/h264_decoder.h` + `.c` — a new function `h264_decoder_feed_annexb(d, buf, len, pts)` that splits on 3 B/4 B start codes and calls feed_nal_to_decoder per NAL then flush_access_unit. Distinct from feed_rtp (which parses RFC 6184).
- `activity/connecting_activity.cpp` — the `SHADOW_NATIVE=1` env-var toggle; starts the DUAL SSE (launcher_jwt + main_jwt) before ctrl_session_glue_run (critical for the server-side `:13011` binding), bypassing dc_session entirely.
- `main_test.c --mode=native-stream` — a CLI test without the GUI: duration_sec auto-aborts through `alarm()`, logs frames_decoded/displayed/decrypt_ok.

## Garde-fous robustesse

- NO hardcoded key: the 32 B chacha20 key and the 20 B hash identifier are parsed from the server's Reply protobufs (see ctrl_parse_authentication_reply_v2 and ctrl_parse_encryption_reply).
- abort_flag polled at a 5 ms granularity (the recv loop nanosleeps 5 ms when empty), honouring feedback_switch_thread_lifecycle.
- `shadow_cipher_decrypt_unsafe` for UDP video (out-of-order allowed), replay left to the caller through the SUFP seq on the audio side.
- On a frame-buffer overflow (>8MiB), drop with a log instead of crashing.

## Left to do

- A live Linux test: `SHADOW_NATIVE=1 ./build_linux/shadow-client` with an active Shadow account → check that the YUV frames reach StreamView.
- P15 audio (DTLS 1.2 on `:13012` + Opus): not wired, the on_audio callback = NULL.
- P12 cursor decoding: the raw bytes are passed through as they are (the Borealis cursor renderer still has to be wired).
- The :13013 input uplink: the RE is finished in the memory note project_shadow_real_protocol_confirmed but the send is not wired into ctrl_session (M5/M6's dc_session had DcInputBridge; to be ported onto ctrl_session).
- The Switch port: ctrl_session.c must also compile for aarch64 Switch (the same wolfSSL APIs + BSD-like sockets). To be tested after the Linux validation.

## Build status

- shadow-client (the Borealis Linux GUI): 28 MiB, it links fine with libdatachannel and ctrl_session side by side.
- shadow-test-cli (the CLI, no GUI): 1.7 MiB, supports --mode=webrtc / native-smoke / native-stream.
- dc-shadow-cli: unchanged.
