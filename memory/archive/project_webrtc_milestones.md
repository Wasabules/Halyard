---
name: WebRTC milestones progress shadow2switch
description: The state of milestones M7-M13 of the native WebRTC implementation on the Switch — the end-to-end pipeline works, image quality is in progress
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the WebRTC milestones, abandoned with that path.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

The user pivoted from the native msquic/protobuf port to a **native WebRTC implementation** because their Shadow account returns `JWT.ports=[]` (legacy), which forces WebRTC. Cf. project_shadow_jwt_ports_pivot.md.

**State at 2026-05-02 (end of the overnight run)**:

The complete pipeline works: WSS signalling → SDP → ICE (libjuice) → DTLS-SRTP (wolfSSL as server) → SRTP unprotect (libsrtp) → RTP H.264 reassembly (RFC 6184: single, STAP-A, FU-A) → access-unit aggregation (1 packet per PTS) → libavcodec decode → YUV→RGBA (manual BT.601) → nanovg display inside the Borealis StreamView. **Validated run = 1896 frames at 1280x720, err=0, a sharp picture of the Windows desktop on screen.** What remains is a slight up/down scrolling that does not appear in Chrome (probably an intra-refresh artefact visible without GPU post-processing).

**Milestones atteints :**
- M7-M13: ✅ the complete pipeline, a clean decoded picture
- M13 polish is left: PTS pacing for smooth display, vsync-bursty decode (60 fps on average, but 18-100 fps instantaneous)
- M14: ✅ Opus + audout 48 kHz stereo. Sound heard on the Switch (user validation 2026-05-02). Hooked PT=111, 4 page-aligned PCM ring buffers with buf_free tracking (fixing the overwrite bug that caused crackling).
- M15 stage A: ✅ minimal SCTP scaffolding (INIT/INIT-ACK/COOKIE-ECHO/COOKIE-ACK ESTABLISHED), hardware NEON CRC32c, dtls_ctx_recv_app/send_app for post-handshake app data. SRTCP outbound protect (server_write keys) for PLI/RR/FIR — otherwise Shadow drops our RTCP.
- M15 stage B (post-2026-05-02): ✅ SACK + DCEP RFC 8832 scaffolding (open/ack), per-stream SSN, the public API `sctp_open_dc(stream_id, type, prio, reliab, label, proto)`. At ESTABLISHED we open the 4 Shadow channels (1=shadow-input ord+maxRetx=1, 3=shadow-cursor reliable, 5=shadow-controller unord+maxRetx=1, 7=shadow-clipboard reliable) — odd IDs because DTLS is the server. on_sctp_msg logs incoming msgs with a hex preview.
- M15 stage C **BLOCKED 2026-05-02**: a byte-perfect replay of the messages captured from Chrome (Connect 96 B then Hello 128 B on shadow-input + a 96 B cursor init on shadow-cursor) → the SCTP SACK's `cum_ack` advances (Shadow delivers Connect+Hello+mouse#1 to its app layer), BUT Shadow never echoes (RECV[0] seq=1 expected). Also tried: timestamp patching (the Switch's RTC aligned to within <1 h of Chrome's), the Connect→Hello order (the browser sends the 96 B before the 128 B), Forward TSN not implemented on the outbound side. Hypotheses for resuming: (1) session-specific data inside the FB that we have not identified (a UUID, an encoded client fingerprint); (2) a vm-config WSS mismatch; (3) a deeper RE of the Connect serialiser inside libAwesomeClientProject.so. The capture hook `dumpchrome/inputs/raw_dc/capture_hook.js` allows re-capturing at will. **Status of the code**: `shadow_input.c` + `shadow_cursor.c` send the blobs verbatim in the right order, and sctp.c decodes SACK with gap/dup tracking for debugging.

**Early-M16 video optimisations validated 2026-05-02**:
- ✅ **Hardware NVDEC decode on the Tegra X1** through `AV_HWDEVICE_TYPE_NVTEGRA` (Averne's switch-ffmpeg): 10 ms→1 ms per H.264 frame. Output in NV12 (Y + interleaved UV), not YUV420P → a dedicated handler.
- ✅ **NEON SIMD YUV→RGBA** (yuv420p_to_rgba + nv12_to_rgba) with int32 multiplication (vmull_s16) — pure C 30-50 ms → NEON 5-8 ms at 720p. DO NOT use `vmulq_s16` directly (int16 overflow on 298×Y).
- ✅ **A more-or-less lock-free triple YUV buffer**: writer_slot (the webrtc thread, no lock) → a microsecond swap under mutex → consumer_slot (the borealis thread, no lock). Doing the conversion on the Borealis thread frees the WebRTC thread for audio + SCTP.
- ✅ **A 60 fps frame-rate cap** on pushYuvFrame using std::chrono.
- ✅ **Adaptive SCTP polling**: 100 ms during the handshake, 1 s after ESTABLISHED.
- ✅ **Phase 2A — an NV12 GLSL shader on the GPU**: a custom NV12→RGB fragment shader, uploading Y (R8) + UV (RG8) as 2 GL textures, rendered through raw inline GL (nvgEndFrame + draw + nvgBeginFrame). Saves 5-8 ms of NEON and 60% of the upload bandwidth.
- ❌ **Phase 2B — true zero-copy NVDEC→GL**: not trivially feasible. The NVDEC buffer is **block-linear tiled** (`is_linear=0`), not directly accessible. `av_hwframe_transfer_data` takes ~11 ms (the VIC engine does the detiling in hardware, and that cost is incompressible without a Block Linear detiling GLSL shader).
- ✅ **An async transfer worker** (h264_decoder.c): a dedicated pthread worker + a queue of 4 cloned frames (av_frame_clone = a refcount bump). The 11 ms transfer_data runs on that thread, while libjuice receives-decodes-pushes and carries on. Frames are dropped if the queue is full (low latency > completeness). The `async_processed`/`async_dropped` stats are exposed.
- ✅ **Log buffering** (log.c): setvbuf 64 KB + a flusher thread doing fflush every 500 ms. No more fflush per line (~1-5 ms of SD sync × ~100 logs/sec = 100-500 ms wasted). The mutex hold goes from milliseconds to microseconds.

**Milestones restants :**
- M14: decode Opus + AudioOut (Shadow does not send audio yet in our tests)
- M15 : SCTP data channel + inputs Switch (joycon → Shadow)
- M16 : Polish — reconnect, latency tuning, transport-cc feedback

**The library stack cross-compiled for the Switch:**
- wolfSSL : DTLS 1.2 + ECDSA + SRTP profile export. Flags : `WOLFSSL_DTLS WOLFSSL_CERTGEN WOLFSSL_KEYGEN WOLFSSL_SRTP WOLFSSL_SMALLSTACK KEEP_PEER_CERT`
- libjuice : ICE/STUN. Flags : `DISABLE_CONSENT_FRESHNESS=ON`. Shims : `pipe_switch.c` (UDP socketpair), `ifaddrs.h` (-1 stub), `__LITTLE_ENDIAN__`
- libsrtp v2 : pour AES_CM_128_SHA1_80
- libopus, libavcodec (switch-ffmpeg via dkp-pacman)

**Why:** it captures the real state so the whole recon does not have to be redone in later conversations — project_shadow2switch.md still talked about the msquic/protobuf port, which is obsolete.

**How to apply:** when the user resumes a shadow2switch session and talks about M14+, know that they are on the native WebRTC branch, not msquic. The code is in `05-shadow-client-borealis/demo/src/webrtc/` (the modules webrtc.c, dtls.c, ice.c, h264_decoder.c, sdp.c, wss.c, rtp.c, srtp_session.c). The NRO is pushed through `gio copy ... mtp://Nintendo_Nintendo_Switch_XTJ10221245951/SD\ Card/switch/shadow-client.nro`.
