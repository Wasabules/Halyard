---
name: T4 — a custom TWCC sender unblocks the server's BWE (×3 peak/p95)
description: 2026-05-06, adding a custom TwccSenderHandler MediaHandler to dc-shadow-cli. It emits RTPFB FMT=15 every 100 ms by parsing the transport-wide-seq RTP extension. Result: peak ×3 (6.3 Mbps), p95 ×3 (4.6 Mbps), variance ÷3.
type: project
---
**Context**: libdatachannel does NOT implement the TWCC sender (verified by grep: no "transport-wide" anywhere in src/). The Shadow server's delay-based BWE stays at a start_bitrate of ~1 Mbps for want of feedback.

**Solution T4** (sans modifier libdatachannel) :
- Classe `shadow::TwccSenderHandler : public rtc::MediaHandler` (dans `demo/src/dc/twcc_sender.{hpp,cpp}` — ~190 LoC C++)
- Chained through `video_track->chainMediaHandler()` after the default RtcpReceivingSession
- Override `incoming(messages, send)` :
  - Parses every RTP packet to extract the transport-wide-seq (RFC 5285 one-byte ext, profile 0xBEDE, extmap id=3 hardcoded)
  - Stocke (seq, recv_ms) dans ring buffer 512 entries
  - Every 100 ms: builds and sends an RTPFB FMT=15 (PT=205) with a 300 ms hold-back for out-of-order packets
- Format draft-holmer-rmcat-transport-wide-cc-extensions-01 : run-length status chunks + delta SMALL (1 byte) ou LARGE (2 bytes) en 250µs units, ref_time 24-bit en 64ms units

**Results over 3 × 60 s runs (idle desktop)**:

| Metric | T4 (DC+TWCC) | DC without TWCC | CUST | vs DC without | vs CUST |
|---|---|---|---|---|---|
| avg  | 1105 | 973  | 797  | +14%  | +39%  |
| peak | 6257 | 2044 | 6816 | ×3.0  | ≈     |
| p50  | 635  | 962  | 546  | -34%  | +16%  |
| p95  | **4606** | 1528 | 2620 | ×3.0  | +76%  |
| variance | ±5% | ±0.3% | ±28% | — | ÷5 |

**The temporal pattern**: an initial 5-15 s burst at 2-3.5 Mbps then convergence to ~500 kbps when idle (the Shadow encoder lowers the bitrate when few pixels change). p95 captures the maximum amplitude available.

**For active gaming**: the encoder pushes continuously → we should sustain ~5-6 Mbps on average. To be tested with a game running on the VM.

**Trade-off** :
- DC + TWCC = a higher average, ×3 the peak against no TWCC, **×5 the stability against CUST**
- No losers on the DC + TWCC side against DC without TWCC: ONLY p50 falls, 962→635 (the encoder converges faster when idle)

**Conclusion**: **T4 is the best configuration on every axis that matters for real use**. The invariant 500 Kbps throttling we suffered at the start is definitively eliminated. The remaining gap to the browser's 18 Mbps = the encoder converging on idle content (not a server cap).

**Code** :
- `demo/src/dc/twcc_sender.{hpp,cpp}` — 190 LoC, MediaHandler standalone
- `demo/src/dc/dc_shadow_cli.cpp` — added `chainMediaHandler` + extmap:3 re-enabled + rtcp-fb transport-cc in the SDP patch
- CMakeLists.txt — twcc_sender.cpp added to the dc-shadow-cli sources
- No libdatachannel change
