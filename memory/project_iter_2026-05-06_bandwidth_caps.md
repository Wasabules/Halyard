---
name: The 2026-05-06 bandwidth iteration — the server caps at 500 Kbps, invariant
description: 7 hypotheses tested autonomously through shadow-test-cli + bench_runner.py. NONE moved the average bitrate from 0.48 Mbps. The server-side throttling is insensitive to client-side changes.
type: project
---
**Setup**: `shadow-test-cli --duration=60 --activity` (mouse moves every 1 s) on the legacy account. Measuring the cumulative `webrtc_stats_t.rtp_video_bytes` → a bitrate from the delta. Account = legacy WebRTC, provider = "legacy" in /vm/ip.

**Baseline** (the code after the 2026-05-06 fixes: an extended SDP + the os-version UA + telemetry + capabilities):
- avg 0.48 Mbps, peak 0.73, p50 0.50, p95 0.72
- 2641 frames @ 1280x720 sur 90s = ~29 fps
- bootstrap 4.5-5.4s

**The session-ff17 browser reference** (Firefox on the same account): 18 Mbps average. The gap ratio = 1:38.

**Hypotheses tested (all single-run, the variance not quantified)**:

| H | Modif | avg / peak | Verdict |
|---|---|---|---|
| H1 | `googcc_target_kbps = 60000` (init 60 Mbps direct) | 0.48 / 0.75 | KO no effect |
| H2 | `--no-activity` (no mouse moves) | 0.48 / 0.73 | KO, no effect |
| H3 | TWCC disabled entirely | exit=3 (a WSS/ICE timeout) | TWCC is mandatory |
| H4 | PT 119 (H264 high@3.1) first in the m-line | 0 packets received | the server rejects it |
| H5 | remove `a=rtcp-rsize` | 0.84 / 6.32 (run 1) then 0.48 / 0.74 (run 2) | flake — not reproducible |
| H6 | `a=msid-semantic: WMS` (sans stream id) | 0.48 / 0.74 | KO no effect |

**Conclusions** :
1. The server CAPS at exactly ~500 Kbps on average + a 700 Kbps peak in steady state, **invariant** with respect to:
   - The REMB target (tested up to 60 Mbps)
   - Activity injection (mouse-moves)
   - msid-semantic
   - Cosmetic SDP variants
2. PT 103 (H.264 baseline 42001f) must stay first — this confirms `project_shadow_h264_quirks` (the server rejects it otherwise).
3. TWCC is **REQUIRED** — without it, the server does not complete the ICE/DTLS handshake.
4. **H5 (rtcp-rsize OFF) was a false positive**: run 1 = a 6.3 Mbps peak, run 2 (same code) = a 0.74 Mbps peak. Enormous variance between runs → every single-run result is to be taken with extreme caution.

**Hypotheses not yet tested**:
- Bit-perfect TWCC content (the ref_time format, the exact status_count, the fb_pkt_count increment)
- A non-zero fake jitter in the RR
- XR/RRTR removal
- Probe packet ack via TWCC
- Migration libdatachannel (cf. `LIBDATACHANNEL_MIGRATION_PLAN.md`)
- Tester sur compte non-legacy (provider != "legacy")
- Test from a non-Cloudflare-datacentre IP (our IP is visible on the server side)

**Recommendation**: to really move the needle, either (a) migrate to libdatachannel for bit-perfect libwebrtc-conformant RTCP, or (b) test on a standard non-legacy account, or (c) audit the network to see whether the NAT type / IP ASN influence the QoS scoring.

**Tooling**: `tools/bench_runner.py` + `shadow-test-cli` are operational. Run >=3× per variant to reduce the variance — a single run is not a signal.
