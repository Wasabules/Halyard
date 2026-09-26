---
name: Freeze t=115s — bit-perfect audit findings (5 agents 2026-05-05)
description: An exhaustive audit of the decrypted Chrome pcap against the Switch code. 6 strong hypotheses remain, ordered by how likely each is to be the root cause.
type: project
---
**Source**: 5 parallel agents, 2026-05-05, over the pcap `shadow-chrome-20260505_003141.{pcap,sslkeys}` + the Switch code `demo/src/webrtc/` + `library/libjuice/src/`.

## The top 6 untested hypotheses, ordered by probability

### 1. The TWCC `reference_time` is badly formatted (VERY HIGH probability)
- **The problem**: `reference_time` in an RTPFB FMT=15 must be `(arrival_ms_first / 64) & 0xFFFFFF` in **64 ms ticks** (not in raw ms). The deltas are in 250 µs ticks.
- **If malformed**: the server ignores the TWCC → the goog-cc bandwidth estimator decays → **it cuts at ~120 s** (kBweTimeoutMs).
- **Audit** : `webrtc.c:205-302` (`send_rtcp_twcc`).
- **Source** : `transport_feedback.cc` libwebrtc.

### 2. `cumul_lost_video=0` and `jitter=0` are hardcoded (VERY HIGH)
- **The problem**: `webrtc.c:477+483` always sends `cumul_lost=0x000000` and `jitter=0` in the RR. The `expected_video`/`cumul_lost_video` fields are initialised at `webrtc.c:129-130` but **never updated** in the dispatch.
- **If faked**: the server sees "a perfect link, zero loss, zero jitter" → suspicious → concludes the client is broken/a mock → cuts.
- **Fix** : calculer correctement RFC 3550 §A.3 (loss) + §A.8 (interarrival_jitter).

### 3. RTCP XR/RRTR (Receiver Reference Time Report) absent (HAUTE)
- **The problem**: Chrome's libwebrtc sends XR/RRTR (PT=207, BT=4, 12 B) compounded with every RR for non-sender RTT computation. Our code sends none.
- **If absent**: the server times out the RTT (kRrTimeoutIntervals × interval) → considers the peer "dead" → cuts.
- **Implementation**: ~40 lines after `append_sdes` in webrtc.c. Wire: an XR header (8 B) + an RRTR block (BT=4 BL=2 NTP-MSW NTP-LSW).
- **Source** : `extended_reports.cc` libwebrtc, RFC 3611 §4.4, [Pion #2767](https://github.com/pion/webrtc/issues/2767).

### 4. NACK feedback (PT=205 FMT=1) received but ignored (MEDIUM-HIGH)
- **The problem**: `parse_rtcp_compound` (webrtc.c:566-606) parses only PT=200 (SR) and ignores PT=205 (RTPFB).
- **If the server NACKs** and we do not retransmit: the server times out → concludes the client is broken → cuts.
- **Note**: we also have `o=12` other packets in the recent stats = possibly PT=104 RTX packets we were dropping.

### 5. STUN attribute order + SOFTWARE non-conforme (MOYENNE)
- **Chrome ordre** : USERNAME → GOOG-NETWORK-INFO → ICE-CONTROLLING → USE-CANDIDATE → PRIORITY → MESSAGE-INTEGRITY → FINGERPRINT
- **libjuice's ordering** (stun.c:147-295): PRIORITY (before USE-CANDIDATE) + a SOFTWARE="libjuice" attribute that Chrome does not send.
- **Fix** : reorder + supprimer SOFTWARE.

### 6. The SDP `a=rtcp-rsize` to be removed (LOW-MEDIUM, easy to test)
- **The problem**: we announce `a=rtcp-rsize\r\n` in both m=audio and m=video (sdp.c:55, 89). If the server does not support reduced-size but we send it some, or if the server accepts rsize but we send classic compound, a strict server-side parser can silently reject everything.
- **Test** : 1 ligne, retirer rtcp-rsize, tester.

## Less likely but notable hypotheses

- **TLS fingerprint** (JA3/JA4) wolfSSL vs BoringSSL Chrome : 6 ciphers vs 16, no GREASE, no ALPN, no PQ X25519MLKEM768. Anti-bot Cloudflare possible.
- **HTTP headers WSS upgrade** : ordre + missing Pragma/Cache-Control/Accept-* + Cookie cf_clearance.
- **Sec-WebSocket-Extensions: permessage-deflate** not sent.
- **DTLS cipher suites**: 2 (wolfSSL) against 11 (Chrome, including CHACHA20_POLY1305).
- **The first-video gate is too strict** on the RTCP RR (webrtc.c:1454-1456) — a race condition if the first packet arrives after 100 s.
- **SCTP heartbeat absent** — RFC 4960 §8.3.
- **IPv6 is preferred on the Switch side** in libjuice's udp.c:159, whereas the browser falls back to IPv4.

## Why

Those 6 leads are **non-redundant** with the 14 already tested. The combination of TWCC content + a fake RR + a missing RRTR = 3 signals the server could use together to time out at 115 s.

## How to apply

**Recommended test order** (effort × ROI):
1. **Test without rtcp-rsize** (5 min, 1 line) — eliminates option 6
2. **Fix `cumul_lost` + `jitter` in the RR** (30 min) — an RFC 3550-conformant fix
3. **Audit + fix the TWCC `reference_time` format** (1 h) — check the 64 ms ticks
4. **Implem RTCP XR/RRTR** (40 lignes, ~1h) — combler le gap libwebrtc
5. **NACK handling + STUN reorder** (2h)
6. **TLS fingerprint** (gros chantier, dernier recours)

**Files concerned**:
- `demo/src/webrtc/webrtc.c:129-130, 477, 483` (RR loss/jitter)
- `demo/src/webrtc/webrtc.c:205-302` (TWCC)
- `demo/src/webrtc/webrtc.c:498-499` (insert XR/RRTR after SDES)
- `demo/src/webrtc/webrtc.c:566-606` (extend the RTCP parse to PT=205/206)
- `demo/src/webrtc/sdp.c:55, 89` (toggle rtcp-rsize)
- `library/libjuice/src/stun.c:147-295` (attribute order)

**Agent sources, 2026-05-05**: 5 parallel reports through Explore + general-purpose agents.
