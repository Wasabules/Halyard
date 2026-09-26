---
name: 🏆 The native Shadow Linux GUI — live streaming confirmed
description: 2026-05-09 — the Borealis shadow-client GUI with SHADOW_NATIVE=1 streams H.264 1920x1080 @ 26-28 fps, 100% chacha20 decrypt OK, and the port probe finds port_base dynamically. The WebRTC→native PIVOT is validated end to end.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records a May 2026 snapshot of 'it streams now'.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

## TL;DR

DECISIVE 2026-05-09 ~01:00: the shadow-client Borealis GUI with the `SHADOW_NATIVE=1` env var streams **H.264 1920x1080 @ 26-28 fps** from the Shadow VM, **100% chacha20 decrypt OK** (818/818, 954/954 frames). No t=120 s freeze. The WebRTC→native pivot is validated end to end.

**Why**: 6 days blocked on a :13011 timeout. The extended LD_PRELOAD hook (connect/send/recv/write/read) exposed the fact that the port_base varies per VM. Probing `9000/10000/11000/12000/13000` dynamically finds the right port (e.g. `:9011` on the VM `self-belt-02deb0e7`).

**How to apply**: to validate/iterate on the native path under Linux, run `cd build_linux && SHADOW_NATIVE=1 ./shadow-client`. To port to Switch: copy ctrl_session.{h,c} + ctrl_session_glue + ctrl_msgs + ctrl_tcp + encryption + the port probe; it is all portable C + wolfSSL/jansson, already available on the Switch side.

## Stats observed (a t=35 s session, the VM `self-belt-02deb0e7`, instance=2)

| t | video pkts | bytes | frames decoded | decrypt OK |
|---|---|---|---|---|
| 5s | 1449 | 1.6 MB | 141 | 157/157 |
| 10s | 2374 | 2.7 MB | 261 | 277/277 |
| 15s | 3532 | 4.1 MB | 394 | 410/410 |
| 20s | 4628 | 5.4 MB | 530 | 546/546 |
| 25s | 5787 | 6.7 MB | 664 | 680/680 |
| 30s | 6983 | 8.1 MB | 802 | 818/818 |
| 35s | 8153 | 9.4 MB | 938 | 954/954 |

= ~27 fps soutenu, ~2.2 Mbps moyen, 100% chacha20 decrypt success.

## Components delivered

- `streaming/ctrl_session.c` — a port probe on `port_base + 11` for `port_bases ∈ {9000,10000,11000,12000,13000}`. A 3 s timeout per probe. The first accept wins.
- `port_base_used` propagated for the UDP registers `:port_base+10` (video), `:port_base+30` (cursor) and `:port_base+13` (input).
- `streaming/ctrl_session_glue.c` — pipeline ctrl_session → h264_decoder_feed_annexb → stream_view_push_yuv.
- `webrtc/h264_decoder.c::h264_decoder_feed_annexb` — split start codes + flush AU.
- `activity/connecting_activity.cpp` — the `SHADOW_NATIVE=1` toggle, a JWT.instance rewrite through `launcher_jwt_instance(creds.main_jwt)` + `launcher_rewrite_url_instance` (`proximus_url` and `messaging_url` from `/<slot>` to `/<jwt.instance>`).
- DUAL SSE keepalive (launcher_jwt + main_jwt) avant ctrl_session_glue_run.

## Caveats

- **ffmpeg cabac warnings**: an occasional `cabac decode of qscale diff failed at X Y` = lost NAL chunks / out-of-order chunks not reassembled. The decode carries on, a minor visual glitch. The SUFP reassembly in ctrl_session still needs investigating.
- **Audio NOT wired**: `ctrl_session_params.on_audio = NULL` in the glue. P15 to be picked up again (DTLS 1.2 on `:port_base+12` + Opus).
- **Cursor decoding NOT wired**: on_cursor passes the raw bytes, there is no bitmap render in StreamView.
- **Inputs NOT wired**: no send on `:port_base+13`. See M5/M6's dc_session DcInputBridge, to be ported.
- **No DELETE of zombie clients / GET version / GET status SSE / POST stats** in the REST sequence. The official desktop app does all of that but our code gets through without it (= the server tolerates it).

## Refs

- Code : `05-shadow-client-borealis/demo/src/streaming/ctrl_session.c`
- Build : `build_linux/shadow-client` 28 MiB
- Memory cross-ref : `project_native_port_base_discovery.md`, `project_native_bootstrap_VALIDATED.md`, `project_native_GUI_integration.md`, `project_pivot_native_protocol.md`
- Hook source: `06-shadow-recon-linux/tls_hook/shadow_tls_hook.c` (extended with connect/send/write/read)

## Resulting action items

- N9 — add the REST sequence (DELETE zombies + version + status + stats) if robustness is lacking
- M3 — investigate the cabac warnings (= incomplete NAL reassembly on multi-slice frames)
- P15 — wire the DTLS+Opus audio
- P12 — wire the cursor rendering
- P16 — wire the uplink inputs
- **Phase 3** — port to the Switch (= everything is portable C, wolfSSL+jansson already cross-compiled)
