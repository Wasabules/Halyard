---
name: M4 GUI migration — shadow-client utilise dc_session/libdatachannel
description: 2026-05-06, the shadow-client GUI migrated onto the libdatachannel path + the TWCC sender + RTX + 1080p browser parity. A clean Linux build. One caveat remains: the data channels (input/cursor/controller) are no-op stubs.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the libdatachannel path, abandoned 2026-05-06.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

## Architecture delivered

```
shadow-client (GUI Borealis)
   │
   ├─ ConnectingActivity → dc_session_run(params)
   │                       (instead of webrtc_session_open — excluded from the build)
   │
   ▼
dc_session.cpp
   ├─ wss.c (wolfSSL signaling)
   ├─ rtc::PeerConnection (libdatachannel ICE+DTLS+SRTP+SCTP)
   ├─ TwccSenderHandler (a custom TWCC unblocks the server's BWE)
   ├─ patch_sdp_for_shadow (42001f, sps-pps, b=AS:45000, transport-cc, ccm fir)
   ├─ Multi-PT 103/107/109/117/119 + RTX 104/108/114/118/120
   ├─ Standard Chrome extmaps (transport-cc enabled)
   ├─ vm-config 1920x1080 + hid-sync + ICE per sdpMid
   └─ video_track.onMessage → h264_decoder_feed_rtp → stream_view_push_yuv
```

## Files created / modified

| Fichier | Action |
|---|---|
| `demo/src/dc/dc_session.h` | NEW — API publique `dc_session_run(params, stats)` |
| `demo/src/dc/dc_session.cpp` | NEW — DcShadowSession extracted from dc_shadow_cli (~440 LoC) |
| `demo/src/dc/sctp_stubs.c` | NEW — no-op stubs for `sctp_send_msg*` (the data channels are disabled) |
| `demo/src/activity/connecting_activity.cpp` | EDIT — `webrtc_session_open` → `dc_session_run` |
| `demo/src/main.cpp` | EDIT — supprime `webrtc_global_cleanup()` (custom stack exclu) |
| `CMakeLists.txt` | EDIT — exclude webrtc.c/ice.c/sctp.c/srtp.c/dtls.c, link libdatachannel deps via `--start-group`/`--end-group`, link OpenSSL system |

## Build Linux verified

```
shadow-client     27.9 MB ✅
dc-shadow-cli      4.5 MB ✅
shadow-test-cli    1.7 MB ✅ (custom stack pour A/B reference)
dc-smoke           3.0 MB ✅
```

## Current caveats (M5 to come)

1. **The data channels (shadow-input/cursor/controller/clipboard) are disabled** — `sctp_send_msg*` is stubbed as a no-op. **The shadow-client GUI is viewer-only**: it receives the video but CANNOT send mouse/keyboard/gamepad. To enable the inputs, a bridge has to be implemented, `shadow_input.c` → `pc->dataChannel(label)->send()` on the libdatachannel side.

2. **The custom WebRTC stack is excluded from the build** — `webrtc.c`, `ice.c`, `sctp.c`, `srtp.c`, `dtls.c` are no longer compiled into shadow-client. The custom CLI binaries (shadow-test-cli) keep their separate compilation as an A/B reference.

3. **shadow_input.c / shadow_controller.c compile but are inactive** — their `sctp_send_msg*` calls resolve to the no-op stubs. No runtime impact except that the inputs do not work.

4. **wolfSSL + OpenSSL coexist** — wolfSSL for wss.c (signalling), OpenSSL for libdatachannel (DTLS + SRTP). Different symbols, so it is fine.

5. **The Switch port (M2) is not done yet** — libdatachannel's build_switch still has to be configured (an mbedTLS backend, the devkitPro toolchain).

## Next steps

| Step | Effort | Impact |
|---|---|---|
| **M5 : data channels bridge** (input/cursor/controller via libdatachannel DC) | 1-2j | Inputs fonctionnels GUI |
| **A real gameplay test** (run the shadow-client GUI on Linux) | 30 min | Validate zero artefacts + zero stutter |
| **M2 : Switch port libdatachannel** | 2-3j | Image clean sur Switch 720p |
