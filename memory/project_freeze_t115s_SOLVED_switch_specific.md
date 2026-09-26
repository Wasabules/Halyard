---
name: 🎯 Freeze t=120 s — FINAL DIAGNOSIS = a Switch-specific bug (not server-side)
description: The decisive test of 2026-05-05: the same WebRTC code runs >245 s with NO freeze on Linux x86_64. The server-side cap was a false trail. The real bug is in the Switch HOS/libnx layer (the UDP buffer, threading, DTLS, or similar).
type: project
---
## TEST DÉCISIF 2026-05-05

Compiled our complete stack (libjuice + libsrtp + wolfSSL + the custom WebRTC + GoogCC + 28 hypotheses applied) for Linux x86_64 and ran it through `shadow-test-cli` (the headless CLI binary).

**Result**:

| Metric | Switch HOS | Linux x86_64 (the same code) |
|----------|------------|---------------------------|
| Stats t=120s | rtp_v=12000 + **FREEZE** ssl_read_n FAIL err=6 | rtp_v=10766 + **continue** |
| Stats t=180s | n/a freeze | rtp_v=16086 ✓ |
| Stats t=240s | n/a freeze | rtp_v=21429 ✓ |
| Session duration | ~120 s + ICE FAILED | **≥245 s with no freeze** |
| Average throughput | ~100 pkts/s | ~89 pkts/s |

**Conclusion** : 
- The **~100 pkts/s throughput cap** applies to both (probably an account-profile thing)
- The **t=120 s freeze is ONLY on Switch** ⇒ a bug in the HOS/libnx layer

## Remaining leads (Switch HOS-specific)

1. **The UDP socket buffer is too small** on the Switch's bsd:s — overflow → kernel drop → the server concludes the client is dead
2. **Threading**: the libjuice thread, the drainer thread and the dispatch may have a subtle block preventing the STUN consent from going out in time
3. **DTLS retransmissions**: an internal wolfSSL or bsd:s timeout at 2 minutes
4. **NAT/network stack** Switch HOS interne timeout
5. **Heap fragmentation** Switch (apparmor mode crashe, title mode marche)
6. **An audio buffer leak**: we initialise audio but never receive packets (pkts=0). Perhaps a blocked audio thread consuming all the worker threads

## Why

None of the previous 28 hypotheses (TLS fingerprint, SDP, RTCP, bitrate hint, GREASE, GoogCC, etc.) could resolve it, because the bug is not in WebRTC. It is in our code's **Switch HOS implementation**.

## How to apply

**Recommandation** : 
1. **DO NOT migrate to libdatachannel** — the bug would potentially reproduce identically
2. **A Switch-HOS-specific audit**: profile the ICE drainer, the libjuice thread, the DTLS retransmissions, libnx memory
3. Test by isolating the threads (one at a time, disable audio, disable the DC…)
4. Augmenter SO_RCVBUF/SO_SNDBUF UDP socket Switch
5. A pcap capture from a PC ARP-spoofing the Switch, to see whether the server sends anything after t=120 s or not

**Fichiers Switch suspects** :
- `library/libjuice/src/agent.c` (consent freshness threading)
- `demo/src/webrtc/webrtc.c` ice_recv_dispatcher (boucle main)
- `library/libnx` BSD socket impl

**Sources** : test 2026-05-05 22:09 sur Linux x86_64, build `build_linux/shadow-test-cli`, log `/tmp/shadow-client/webrtc.log` 245s session.
