---
name: M4 — H.264 decoder bridge libdatachannel ✅ done
description: 2026-05-06, h264_decoder.c wired onto libdatachannel's video_track. The pipeline RTP → NAL reassembly → libavcodec → YUV → stream_view_push_yuv. M4_1 = 919 frames decoded in 30 s at 1920x1080 = a confirmed 30 fps.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the libdatachannel path, abandoned 2026-05-06.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

## The complete pipeline validated

```
libdatachannel video_track
    │ onMessage (the RTP packet, decrypted post-SRTP)
    ▼
rtp_parse_payload(buf) → strip header RTP, extract payload + timestamp + PT
    │ (skip si PT != H.264 i.e. 103/107/109/117/119)
    ▼
h264_decoder_feed_rtp(payload, len, ts)
    │ (NAL reassembly : single / STAP-A / FU-A — RFC 6184)
    ▼
libavcodec H.264 decode (async transfer worker)
    │
    ▼
dc_h264_frame_cb(width, height, YUV planes, pts, …)
    │
    ▼
stream_view_push_yuv(...) — no-op headless via main_test_stubs.c
                          — vrai rendering Borealis dans shadow-client GUI
```

## Test M4_1 (30s, idle desktop) :
- avg 2334 kbps, peak 3206, p50 2362, p95 3194 (steady ~2.3 Mbps)
- **919 frames decoded sur 30s = 30 fps**
- **Resolution 1920×1080 confirmed** (the server honours the 1080p vm-config)
- 8567 RTP packets received, 8.7 MB

## To migrate the Borealis GUI (shadow-client):
The dc-shadow-cli C++ path compiles but coexists with the custom stack. To integrate it into the shadow-client GUI:

1. Factor dc_shadow_cli.cpp content en library callable :
   - Move `DcShadowSession` class + bootstrap helpers to `demo/src/dc/dc_session.cpp`
   - Exposes the API `dc_session_open(params, frame_cb)`, similar to `webrtc_session_open`
2. Modify `connecting_activity.cpp` to call `dc_session_open()` instead of `webrtc_session_open()`
3. The Borealis StreamView's `stream_view_push_yuv` (already in place for the custom stack) receives the YUV naturally
4. Linking : `target_link_libraries(shadow-client … libdatachannel-static.a libjuice-static.a libsrtp2.a libusrsctp.a)`

Estimated effort: 1 day for the refactoring + integration + a test on the Linux GUI side.

For Switch (M2): the same factoring, but building libdatachannel for the devkitPro toolchain (an mbedtls backend, Switch threads) — ~2 days.

**The final cause of the shadow-client GUI's artefacts/stutter** = the custom stack stuck at a 480 Kbps average with no RTX. With the migration to the dc-shadow-cli path (T7+M4):
- 18 Mbps disponible → zero artefact H264
- RTX paired (104/108/118) → recovery automatique sur loss → zero saccade
- A custom TWCC sender → a stable delay-based BWE → predictable latency
