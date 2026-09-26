---
name: libdatachannel migration M1 done — Linux build OK
description: 2026-05-06, M1 delivered. libdatachannel v0.24.2 builds on the Linux desktop (build_linux/libdatachannel-static.a). dc-smoke proves PeerConnection + ICE gather + SDP generation are OK. The next step M3 (a Shadow signaling wrapper) was swapped with M2 so the gain is validated before the Switch port.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the libdatachannel path, abandoned 2026-05-06.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

**M1 status** : ✅ DONE 2026-05-06.

**Build setup** :
```bash
cd library/libdatachannel/build_linux
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
         -DNO_EXAMPLES=ON -DNO_TESTS=ON \
         -DUSE_GNUTLS=OFF -DUSE_MBEDTLS=OFF
make -j$(nproc) datachannel-static
```

Output: `libdatachannel-static.a` (7.5 MB) + the static deps (libjuice-static.a, libsrtp2.a, libusrsctp.a). Linked against the system OpenSSL 3.0.13. A ~3 minute build.

**Smoke binary**: `build_linux/dc-smoke` (CMakeLists.txt:284+). Creates an rtc::PeerConnection with STUN, gathers candidates, generates a Shadow-style SDP (audio Opus PT 111 + video H.264 multi-PT 103/107/109/117/119 + m=application data channels). Everything works.

**SDP differences, libdatachannel against our custom one** (to patch in M3):
- `profile-level-id=42e01f` by default (constrained baseline) — it MUST be `42001f` for SPS/PPS-in-keyframe (see the memory note `Shadow H.264 quirks`)
- Manque `b=AS:45000`, `sps-pps-idr-in-keyframe=1`, extmaps custom (transport-cc, abs-send-time, video-orientation, playout-delay, video-content-type, video-timing, color-space, sdes:mid/rtp-stream-id/repaired-rtp-stream-id), `transport-cc` + `ccm fir` dans rtcp-fb
- Also included: `a=group:LS 0 1` (lip-sync) — to be removed if the server rejects it

**A tactical decision**: SWAP the M2/M3 order. Do M3 (the Linux wrapper + benchmark) BEFORE M2 (the Switch port). If the gain is not measurable → M2 is abandoned. The validation criterion: `bench_runner.py` avg >= 5 Mbps (10× the current 0.48 Mbps baseline).

**The detailed M3 plan**: see `LIBDATACHANNEL_MIGRATION_PLAN.md`. The hybrid architecture chosen:
- The C bootstrap (oauth, tinag, vm/ip, proximus) + the wolfSSL WSS = unchanged
- libdatachannel ICE+DTLS+SRTP+SCTP+RTCP through rtc::PeerConnection
- An SDP patcher to match Shadow's quirks after pc->localDescription()
- Track callbacks → h264_decoder + audio.c (transport-agnostic)
- DC callbacks → shadow_input.c + shadow_controller.c (FlatBuffers)

**How to apply**: if M3 is resumed, start by forking `main_test.c` into `dc_shadow_cli.cpp`, keep the same flow up to proximus_create_main_client then replace webrtc_session_open with a libdatachannel-based dc_session_open(). Keep the JSON output in the same format so `bench_runner.py` A/B stays compatible.
