---
name: project-rg-nack-format-re
description: 2026-05-14 — An RE of the desktop's rG NACK packet format (= 0x72 0x47 0x01 + seq + count + u16 LE × N chunk_idx). The format is CORRECT but sending it breaks the session if the chunks requested were not actually lost.
metadata: 
  node_type: memory
  type: project
---

## Format wire rG packet (UDP :11010 envoi client → server)

Captured from the desktop through LD_PRELOAD on 2026-05-14:

```
byte 0:    0x72         // 'r'
byte 1:    0x47         // 'G'
byte 2:    0x01         // proto version
byte 3:    NN           // counter / random nonce (varies)
byte 4:    count        // # of u16 entries that follow
byte 5:    0x00         // padding
bytes 6+: count × u16 LE  // the chunk_idx list (= the chunks asked for in the resend)
```

Total len = 6 + count*2 bytes.

Patterns observed (count: len, sample entries):
- count=1 : len=8, entries=[1] ou [0]
- count=2 : len=10, entries=[1, 4] ou [0, 1]
- count=9 : len=24, entries=[0..8]

## A naive send test (= request a resend of chunks 0..N-1)

Implemented as N29 + tested on 2026-05-14:
- Our code was sending rG `72 47 01 NN count 00 [u16 0..N-1]` every 2 s
- **Result: the video UDP throughput collapses, 392 pps → 66 pps** → the server stops streaming

→ Probably the server detects that chunks 0..N already exist (= received, server-confirmed) and either ignores it or kills the session.

## Bonne utilisation de rG

The rG is a **NACK list for real UDP packet loss**. The client must:
1. Track the chunk_idx received per subchan
2. Detect which chunks are actually missing (= gaps in the received sequence)
3. Envoyer rG avec ces vrais missing indices

In our case (= a stable LAN/Wi-Fi), there is no UDP packet loss → no rG to send.

## Conclusion

rG is NOT a way to ask for extra chunks such as the bottom slice. It is only a retransmit mechanism for packet loss. The bottom slice is simply NOT among the chunks the server sends.

## Cross-ref

- [[project-video-540-clamp-workaround]] — baseline 1920×540 actuel
- [[project-video-decode-38pct-solved]] — the full subchan analysis
