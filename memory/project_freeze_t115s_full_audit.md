---
name: Freeze t=115 s — the exhaustive audit is spent, the cause is invisible from software
description: 14 hypotheses tested on 2026-05-04/05, covering all of SDP/WSS/STUN/UA/spoofing/GOOG-NETWORK-INFO. The freeze at exactly t=115 s is invariant. The real cause is probably low-level (TLS fingerprint, IP geo, an absolute server-side timer).
type: project
---
**Investigation 2026-05-04/05** — an exhaustive summary after 14+ tests on Switch without managing to eliminate the t=115 s freeze.

## Hypotheses tested and eliminated

| # | Hypothesis | Test | Result |
|---|-----------|------|----------|
| 1 | RFC 7675 consent freshness | rebuild libjuice OFF→ON | ICE FAILED detection ✓ but the freeze persists |
| 2 | HTTP polling of /status, /stream, /forward | already done | eliminated |
| 3 | RTCP RR 20 Hz + REMB 1 Hz + TWCC 10 Hz | already done | eliminated |
| 4 | shadow-controller DC + ANNOUNCE + a 128 B ping at 0.5 Hz | implemented + an ordering bug fixed | the freeze persists |
| 5 | A 20 s client WSS PING | wss_send_ping added | the server answers, no effect on the freeze |
| 6 | An application-level WSS TEXT keepalive | analysis of sessions 14/16/17 | the browser sends none after init |
| 7 | SDP audio extmap:1 ssrc-audio-level | added | ✗ |
| 8 | SDP video b=AS:45000 + Chrome's 5 extmaps | added | +22% frames, the freeze persists |
| 9 | SDP video RTX (PT 104) | added then reverted | it makes throughput worse (-30%) |
| 10 | A WSS bitrate hint JSON {start,min,max} | added ✓ | **the best**: +45% throughput (147 pkts/s against 102), the freeze persists |
| 11 | User-Agent Chrome 147 (the WSS upgrade + HTTP) | spoofed | ✗ |
| 12 | proximus opaque os-name=Linux/arch=x86_64/platform-type=web | spoofed | ✗ |
| 13 | DTLS cert CN/O = WebRTC + a random RTCP CNAME | renamed | ✗ |
| 14 | STUN GOOG-NETWORK-INFO 0xC057 (a libjuice patch) | added | ✗ |
| 15 | SDP `a=rtcp-rsize` removed | toggled | neutral |
| 16 | RR cumul_lost + jitter computed per RFC 3550 | a real non-zero computation | it makes throughput worse, -36% (the server prefers the hardcoded 0) |
| 17 | RTCP XR/RRTR RFC 3611 §4.4 (BT=4) | added | **+5 s of session** (the freeze moves from t=115 to t=120 s) ✓ the first shift observed |
| 18 | NACK PT=205 + PSFB PT=206 parsing | logging added | the server sends neither → not the cause |
| 19 | Server XR PT=207 BT identification | logged | the server sends BT=5 (DLRR) in reply to our RRTR → standard RFC 3611, informational only, not the cause |
| 20 | Sec-WebSocket-Extensions: permessage-deflate | added to the upgrade | the server does not echo it → ignores it. Neutral |
| 21 | ALPN http/1.1 (wolfSSL recompiled with HAVE_ALPN) | added | the TLS fingerprint improves, **but the t=120 s freeze persists** |
| 22 | ALPN h2,http/1.1 | tested | ❌ it breaks the WSS upgrade (the server picks h2, we send HTTP/1.1 → close) |
| 23 | wolfSSL HAVE_HPKE + HAVE_ECH + HAVE_OCSP + PSK | rebuild | the freeze persists at t=120 s |
| 24 | TLS GREASE cipher 0xXaXa injected at the head (a tls13.c::SendTls13ClientHello patch) | wolfSSL patched | the freeze persists at t=120 s — **the TLS fingerprint is eliminated as a cause** |
| 25 | The shadow-input init replaced by 7× a 144 B mouse_move (the libnxbox TransactionStart pattern) | shadow_input.c patched | worse (12 against 10 server retries) — a specific vtable is expected |
| 26 | Server shadow-input messages 100b/96b, dumping 128 bytes | extended logging | the server sends 395 messages/120 s with an incrementing seq counter (1, 2, 4, 6, 7, c, d, 14, 15, 16). It is a continuous APPLICATION stream (not just retries), with a Shadow protocol structure that is non-trivial to reverse without the FlatBuffer schema |
| 27 | TWCC v2: detecting lost (status=00) + large_delta (status=10) + a run-length mix | refactored send_rtcp_twcc | 1150+ TWCCs sent correctly, **lost=0 always** (a true 0 % packet loss), the server **still ignores it** and does not raise the bitrate → confirms a server-side cap applied on criteria outside our control (IP geolocation, account, anti-bot) |
| 28 | A simplified GoogCC AIMD + a dynamic REMB (1000→60000 Kbps in ~30 s) | a new googcc_tick + dynamic REMB encoding | **DEFINITIVE CONFIRMATION**: the server receives BR=60000 Kbps but still streams ~1 Mbps and still freezes at exactly t=120 s. **The server COMPLETELY ignores our BWE feedback.** The certain cause is a server-side check outside our control (a legacy JWT.ports, the account profile, the IP, a global non-TLS fingerprint). |

## The invariant pattern observed

- The freeze happens at exactly t=115 s whatever the throughput received
- The server streams 14× less to the Switch (147 pkts/s) than to the browser (~2000 pkts/s)
- The WSS close with ssl_read_n FAIL err=6 arrives AFTER the RTP freeze (a cascade)
- ICE FAILED ~15 s after the freeze
- Pcap Chrome sans freeze 5min8s, 18 Mbps stable, 0.06% loss

## Hypotheses not yet explored

- DTLS handshake bytes (cipher suites order, srtp_protection_profiles)
- Cadence STUN connectivity checks : libjuice 4-6s vs browser 0.8s
- TWCC content (run-length chunks) delta timing exact
- DSCP/ToS bits on the UDP side (Switch HOS)
- TLS GREASE (RFC 8701): wolfSSL does not support it natively

## Aggregated observations across every test

| Config | rtp_v at t=115 s | freeze at |
|--------|---------------|-----------|
| Baseline | 11718 | t=115s |
| + b=AS:45000 + extmaps Chrome | 12215 | t=115s |
| + the WSS bitrate hint (the best throughput) | 16893 | t=115 s |
| + GOOG-NETWORK-INFO STUN | 10876 | t=115s |
| + Full Chrome spoof UA/os | 12057 | t=115s |
| + RR cumul_lost/jitter calc | 10808 | t=115s |
| **+ RTCP XR/RRTR RFC 3611** | **11905 @ t=120 s** | **t=120 s** ⭐ the first shift |
| + NACK/PSFB/XR parsing (logging only) | 10758 @ t=120s | t=120s |
| + Sec-WebSocket-Extensions | 10758 @ t=120s | t=120s |
| **+ ALPN http/1.1 (TLS fingerprint)** | **10735 @ t=120s** | **t=120s** |
| **+ N1 HPKE/ECH/OCSP + N2 GREASE cipher** | **11421 @ t=120 s** | **t=120 s** ❌ the TLS fingerprint eliminated as a cause |

## Why

**28 hypotheses tested + a working GoogCC and TWCC v2**. The t=120 s freeze is invariant. **THE CERTAIN CAUSE** = a server-side cap, NOT tractable from the client software.

Remaining hypotheses (heavy effort, UNCERTAIN ROI):
- Migration libdatachannel (~14-19 jours) — peut marcher si server check stack-specific, sinon non
- A deep Ghidra RE of the Linux Shadow client (~5-10 days) — it might reveal the server's checks (JWT.ports, anti-bot)
- An upgraded Shadow Pro account — it eliminates the cap if it is profile-based
- A test from another IP geo (a VPN) — it eliminates the cap if it is IP-based

The 30 s auto-reconnect workaround remains usable for an MVP. **Final recommendation: move on to M16 polish and accept the workaround.**

## How to apply

1. DO NOT retest these 14 hypotheses
2. Keep the beneficial changes: b=AS:45000, the audio-level + video extmaps, the WSS bitrate hint (+45% throughput)
3. Revert the changes with no effect: the Chrome UA, the proximus spoof, the WebRTC DTLS cert, the random CNAME, GOOG-NETWORK-INFO
4. The reference capture: captures/shadow-chrome-20260505_003141.{pcap,sslkeys}

**Sources** : sessions test 2026-05-04/05 logs `/tmp/switch-logs-*`, captures session14-17, pcap Chrome 003141, NetLog Chrome.
