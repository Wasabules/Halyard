---
name: WebRTC freeze t=120s unsolved investigation
description: Server stops sending ALL UDP at exactly t=120s session uptime, eliminating 11+ hypotheses. Workaround = auto-reconnect.
type: project
---
The pattern: at exactly **t=115-120 s** after `webrtc_session_open`, the Shadow VM's server **stops sending ALL UDP** to the Switch (RTP video, RTCP SR, SCTP DATA — everything). HTTPS (SSE, WSS) stays alive. Auto-reconnect restores it after ~30 s of detected inactivity. Chrome never freezes (>9 min idle confirmed in session13).

**Why:** it blocks any long session. Temporarily acceptable with auto-reconnect but to be resolved for the M16 polish.

**How to apply:** do not re-test these hypotheses, they are eliminated. For future debugging, capture a pcap on the Shadow side or do a byte-perfect SDP diff between Chrome and us.

## Hypotheses eliminated (all tested, the freeze persists at t=120 s)

1. **The RTCP RR cadence**: raised from 1 Hz → 20 Hz (the browser does 21 Hz). The code is in `send_rtcp_rr` in `webrtc.c`.
2. **LSR/DLSR populated** : parse incoming SR (PT=200) + remplir RR avec NTP middle 32 bits + delta. `parse_rtcp_compound` + `last_sr_lsr_video/audio`.
3. **Compound RTCP**: RR + SDES (CNAME "shadow2switch") instead of RR alone. RFC 3550 §6.1. An `append_sdes()` helper.
4. **REMB feedback**: RTCP PSFB FMT=15 AFB BR=50 Mbps at 1 Hz. `send_rtcp_remb()` compound RR+SDES+REMB.
5. **SDP extmap**: added abs-send-time, transport-cc, sdes:mid and toffset to the offer. The server confirms them in the answer.
6. **A real TWCC**: RTPFB FMT=15 PT=205 with proper run-length chunks (T=0 S=01 small-delta). `send_rtcp_twcc()` at 10 Hz, parsing the transport-wide-seq through `parse_twcc_seq()`.
7. **shadow-controller traffic**: temporarily disabled, the freeze persists = not the cause.
8. **HTTPS SSE keepalive**: `proximus_sse_start()`, a parallel thread keeping `/<slot>/stream` open. The SSE receives 1092 pings over 150 s, alive to the end. Not the cause.
9. **An aggressive STUN keepalive**: libjuice's `STUN_KEEPALIVE_PERIOD` from 15 s → 5 s in `library/libjuice/src/agent.h`. Not the cause.
10. **WSS PING/PONG**: a drainer thread `wss_drainer_thread` calling `wss_recv` continuously (auto-PONG through wss.c line 487). It runs for the whole session and exits cleanly at err=6 (peer close). Not the cause.
11. **libjuice DEBUG logs**: no critical event between t=110-130 s. libjuice detects nothing. But CAREFUL: DEBUG floods 5 lines per STUN binding and blocks the log thread → the stream does not start and the app crashes. STAY at INFO.

## Diagnostic counters concluants

The stats logging in `webrtc.c`'s main loop now includes:
`ice rx=N srtp=N srtcp=N dtls=N oth=N`. At the freeze, **they ALL stop at once**:
- t=115s: ice rx=12089 srtp=11720 srtcp=191 dtls=178
- t=120s: ice rx=12165 srtp=11793 srtcp=193 dtls=179
- t=125-150s: identiques → **server stoppe COMPLÈTEMENT d'envoyer UDP**

Our outbound continues (RR rc=0 every 50 ms). So it is a unidirectional cut.

## Unexplored leads that could still help

- **A pcap capture on the Switch side**: impossible directly, but possible through mirroring on a managed switch, or tcpdump on a nearby PC with ARP spoofing to see the Switch→Shadow traffic.
- **A test on a different network** (4G through tethering): it would rule out the Free.fr box.
- **A byte-perfect SDP diff**: generate the SDP exactly as Chrome does (msid stream/track ids, ssrc-group:FID rtx, multiple codec PTs, etc.).
- **A TURN relay**: force a TURN relay instead of srflx to rule out direct NAT.
- **Use the Linux Shadow client's logs**: `06-shadow-recon-linux/captures/launcher-20260503-023714/launcher.log` to see what the official client does.

## État actuel acceptable

Auto-reconnect in `webrtc.c` detects a freeze at 10/20/30 s then sets abort_flag → ConnectingActivity retries up to 3×. Degraded UX (~30 s of black screen every 2 min) but usable. All the other M16 leads (touch input, Joy-Con mapping, audio, polish) stay open.
