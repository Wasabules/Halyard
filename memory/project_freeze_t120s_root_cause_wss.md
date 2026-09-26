---
name: Freeze t=115 s — WSS eliminated, the real cause is a video throughput 20× too low
description: The server streams 1 Mbps to the Switch against a stable 18 Mbps to the browser. The major SDP difference: no b=AS:, no Chrome extmaps, no RTX → the server adapts extremely low and cuts at 115 s.
type: project
---
**Investigation 2026-05-04 (session17 Firefox 5min8s + NetLog Chrome)** :

**Hypotheses ELIMINATED for good**:
- WSS PING/PONG: the browser and the Switch are byte-identical (the server PINGs every 5.02 s, both sides auto-PONG). Chrome's NetLog confirms it. WS is not at fault — the WSS closing at t=125 s is a *consequence* of the freeze, not its cause.
- Audio: the browser receives no audio either (`inbound-rtp-audio` absent from Firefox's rtc-stats over 5 min). Audio = silent by design on an idle Shadow VM.
- Consent freshness, /status polling, RTCP RR/REMB/TWCC, the shadow-controller ping: all eliminated.

**Vraie diff observable** :
- The FF browser: **a stable 18 Mbps** for 5 min 8 s, 594k video packets, 0.06% loss, 19 ms RTT
- Switch: **1 Mbps**, 102 pkts/s, 18× less. No drops on the client side (highest_seq consistent with rtp_count). The server REALLY IS streaming 1 Mbps to us.

**Diffs SDP m=video** :
- `b=AS:45000` ← Browser annonce upper bound. Nous : ABSENT.
- `extmap:5` playout-delay, `extmap:6` video-content-type, `extmap:7` video-timing, `extmap:8` color-space, `extmap:13` video-orientation ← Browser. Nous : ABSENTS.
- 41 codecs offered (various H264 profiles + RTX). Us: just PT=102.
- RTX (PT 104 retransmission) ← Browser. Nous : ABSENT.

**A strong hypothesis**: with no `b=AS:`, no RTX and no signaling-cc extmaps, the server does not know our maximum bandwidth or our loss tolerance → it adapts extremely low → and cuts at t=115 s to free the VM's resources.

**Why**: it explains the uniform pattern observed. Our rtcp/twcc/keepalives did not help because the problem was at the level of the **server's encoding rate**, not the transport.

**How to apply** : 
1. Fix applied 2026-05-04: `b=AS:45000` + `extmap:1` audio-level + `extmap:5/6/7/8/13` video-* in `demo/src/webrtc/sdp.c`. The build is OK, the push is pending (ftpd offline).
2. Test: if the Switch receives 18 Mbps like the browser → the problem is solved. If not → add RTX (more complex).

**Sources** : `captures/shadow-session-ff17.json` rtc-stats `inbound-rtp-video` 593988 packets / 308s = 1929 pkts/s vs Switch 11718/115s = 102 pkts/s. NetLog Chrome `chrome-net-export-log.json` PING/PONG WS 5.02s identique.
