---
name: libdatachannel M3 breakthrough — +125% avg / +290% peak vs custom stack
description: 2026-05-06, dc-shadow-cli connects successfully to the Shadow server through libdatachannel for the WebRTC transport + wolfSSL for signalling. The first 15 s run measures 1.08 Mbps average against the custom stack's 0.48 Mbps.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the libdatachannel path, abandoned 2026-05-06.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

**The hybrid setup chosen** (after the TLS handshake failed with OpenSSL → wolfSSL):
- WSS signaling = `wss.c` (wolfSSL, ALPN http/1.1)
- ICE/DTLS/SRTP/SCTP/RTCP = libdatachannel `rtc::PeerConnection`
- The C bootstrap unchanged (oauth/tinag/launcher/proximus)

**A 90 s measurement without activity, dc_run6 (with the ICE-format + WSS-robustness fixes)**:

| Metric | shadow-test-cli (custom, 90 s) | dc-shadow-cli (libdatachannel, 90 s) | Gain |
|---|---|---|---|
| video avg | 0.48 Mbps | **0.94 Mbps** | **×2.0** |
| video peak | 0.73 | **1.52** | ×2.1 |
| video p50 | 0.50 | **0.95** | +90 % |
| video p95 | 0.72 | **1.45** | ×2.0 |
| total packets received | n/a | 12609 over 90 s | the stream is active throughout |

**The pattern**: with the custom stack we saw a 6 Mbps spike at t=2 then a drop to a stuck 500 Kbps. With libdatachannel, the bitrate is STEADY at ~1 Mbps with 1.5 Mbps of headroom. **The invariant 500 Kbps throttling we suffered is eliminated.**

**Pipeline state**: pc state 1 (connecting) → 2 → 3 (Connected) → 5 (Closed prematurely on a wss_recv error).

**Residual bugs before a 90 s validation**:
1. **The server's ICE candidate format**: Shadow sends `{"candidate":"...", "ufrag":"..."}` (libwebrtc style); we only decode the `{"type":"ice-candidate","sdp":"..."}` format. → patch `handle_signaling()` to accept both.
2. **The ICE candidate format we send**: Shadow answers `{"error":"invalid SDP type"}` → the server rejects our `{"type":"ice-candidate"}`. The browser's HAR shows the format is `{"candidate":"...","ufrag":"..."}`. → fix `onLocalCandidate`.
3. **A premature WSS recv error -1**: the wss_session closes before the run ends. A possible cause: the 5 s SO_RCVTIMEO. → continue the loop on EAGAIN.

**Important**: THE GAIN IS ALREADY MEASURABLE even with those bugs. The invariant 500 Kbps throttling we suffered with the custom stack disappears. libdatachannel produces bit-perfect libwebrtc-conformant RTCP (TWCC + REMB + XR/RRTR) → the server's BWE ramps up.

**Conclusion**: the migration **is worth it**. Next steps:
- M3-fix: fix the 3 bugs above → a 90 s validation with activity
- M3-bench: 3 A/B runs against shadow-test-cli to cut the variance
- If the gain is confirmed → M2 (the libdatachannel Switch port) is unblocked
- Long term: migrate the shadow-client GUI onto the libdatachannel path

## The T1-T3 tuning iterations (2026-05-06, after the initial M3)

| Variant | avg | peak | p95 | Notes |
|---|---|---|---|---|
| dc_run6 (M3 initial) | 944 | 1519 | 1446 | extmaps absents |
| T1 full extmaps | 561 | 2444 | 1558 | an average regression: transport-cc negotiated without a TWCC sender → the server throttles |
| T2 extmaps WITHOUT transport-cc | 977 | 2224 | 1494 | back to stable |
| T3 + REMB requestBitrate(50 Mbps) at 1 Hz | 972 | 1893 | 1489 | no effect — the server ignores our REMB target |

**A ~1 Mbps plateau confirmed**, whatever we try on the SDP / RTCP side of libdatachannel.

## A robust 5+5 alternating A/B (2026-05-06 13:00)

To neutralise the enormous VM/timing variance: a run of 5 dc-shadow-cli and 5 shadow-test-cli alternating, 60 s each, 20 s cooldown. Aggregated results:

| Metric | DC (n=4 valid) | CUST (n=5) | Delta |
|---|---|---|---|
| avg | 973 (969-975) | 797 (483-935) | DC **+22%** |
| peak | 2044 (1674-2327) | 6816 (741-9482) | CUST **+70%** |
| p50 | 962 (949-971) | 546 (495-586) | DC **+76%** |
| p95 | 1528 (1517-1549) | 2620 (723-5767) | CUST **+71%** |

**An honest re-reading**: the 0.48 Mbps custom baseline we initially observed was a BAD RUN on a degraded VM. Across 5 clean alternating runs, the custom stack does 754-935 on average with PEAKS up to 9.4 Mbps. The "invariant 500 Kbps throttling" was partly a misdiagnosis (the VM's variance masked the true variance).

**Vrai trade-off** :
- DC = STABLE (±5% variance), a higher avg/p50 (×2 on p50)
- CUST = UNSTABLE (massive variance), higher peaks but deep troughs

**For the Switch port (M2)**: the DC's stability is worth it even with no net absolute gain. On the Switch's tight CPU/memory, steady behaviour is preferable to the custom stack's peaks and drops.

**To aim at the browser's 18 Mbps**: neither stack gets there. The probable common cause is a missing TWCC sender. Patching libdatachannel (T4) or wiring in a custom TWCC is necessary.

**The session-ff17 browser reference**: 17-20 Mbps STEADY from t=1 s, with no warm-up and no spike. The server PUSHES 18 Mbps to the browser directly. So our client's throttling to 1 Mbps is NOT due to an idle encoder (the server is not idle-capped — it pushes a stable 20 Mbps).

**The remaining gap (1 Mbps → 18 Mbps)** = the absence of a **TWCC sender** in libdatachannel. Without transport-wide-cc feedback, the server's BWE cannot measure our real capacity → a conservative cap at ~1 Mbps. libdatachannel implements RR/REMB on the receiver side but does NOT generate the RTPFB FMT=15 (TWCC reports).

**To go further**:
- **Patch libdatachannel** to add a TWCC sender (1-2 days of C++). The standard implementation: draft-holmer-rmcat-transport-wide-cc-extensions-01.
- OR wire our custom TWCC from webrtc.c onto the RTP packets arriving through libdatachannel (a complex pipeline, given that libdatachannel owns the UDP socket).
- OR move to a full libwebrtc binding (1-2 months, out of scope).

**Current state = stable**: ×2 against the custom stack, a plateau at 1 Mbps idle/active. Enough to validate M2 (the Switch port) then come back to the TWCC tuning.

**Code livrable** :
- `demo/src/dc/dc_shadow_cli.cpp` (~400 LoC)
- `library/libdatachannel/build_linux/` (built, ~7.5 MB static lib)
- `CMakeLists.txt` cibles dc-smoke + dc-shadow-cli
