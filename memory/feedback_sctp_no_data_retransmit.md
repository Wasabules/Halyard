---
name: SCTP DATA retransmission missing on the Switch side
description: Our sctp.c does not retransmit DATA chunks and never sends FORWARD-TSN — a single lost packet blocks all of shadow-input.
type: feedback
---
The final cause of Shadow's silence on the data channel (validated 2026-05-02, after the timestamp fix):
- DCEP_OPEN, DCEP_ACK et DATA chunks **partent** correctement
- The wire+40 timestamp is now patched with `time_now_ms_synced()` (an HTTP Date sync offset) → we are no longer stale
- BUT the returning SACKs show a frozen `cum_ack` with `ngaps=1→2`: one of our chunks (probably the 128 B Hello or a mouse-move) is lost on the network
- Our `sctp.c` has NO DATA retransmit, and never sends FORWARD-TSN even though we advertise the parameter in INIT
- shadow-input being `ordered=true`, the Shadow server keeps everything in its SCTP reassembly queue — the Shadow app receives NOTHING until the missing TSN arrives

**Why:** with neither retransmit nor FORWARD-TSN, any UDP-DTLS packet loss blocks the ordered stream permanently. The browser (libwebrtc) does it natively through usrsctp.

**How to apply:** for M15, the options in order of complexity:
1. **Buffer SACK + retransmit**: keep the last N chunks sent, parse the gap blocks in SACK, retransmit the missing TSNs. ~150 lines in sctp.c.
2. **FORWARD-TSN**: implement sending a FORWARD-TSN chunk (type 0xc0) after a timeout on abandoned chunks (shadow-input is partial-reliable, max_retx=1). RFC 3758 §3.
3. **The ultra-simple hack**: send each Shadow message 2-3 times back to back with different TSNs. If one copy gets through, the server will deliver it. Cost: ×3 traffic but zero logic.
4. **Switch to `unordered=true`** on shadow-input → but the browser uses ordered=true, so we would deviate from the protocol.

Recommendation: (1) is the clean solution, (3) is a quick test to confirm the packet-loss hypothesis before investing in (1).
