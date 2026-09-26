---
name: project-cursor-channel-payload-RE
description: TIER7 T3 — the cursor on :base+30 UDP uses the same SUFP v3 + chacha20 wire as the video (type=3). 2273 × 47 B position updates + 31 × 11 B server pings + 2 × 310 B bitmaps. No hidden ctrl payload. Not a taskbar trigger.
metadata:
  type: project
---

# TIER7 T3 — Cursor channel `:base+30` payload deep dive

> **Mission**: our client opens `:base+30` UDP and registers but never looks at
> the bytes received. Decode the wire format, work out whether the server pushes
> control data mixed in with the cursor positions, and if so use it as a trigger.
>
> **Verdict C90** : cursor channel = pur cursor data en SUFP v3 + chacha20
> (= the same wire as the video's :base+10 but with type=3). No hidden ctrl payload.
> Our CUR1 implementation is byte-exact correct.
>
> **Not a multi-NAL / taskbar trigger**.

## RX inventory (capture MASTER, sockfd=302)

| len | count | cadence p50 | meaning |
|-----|-------|-------------|---------|
| **47** | 2273 | 67.98ms (~15Hz) | cursor position update (SUFP+chacha20) |
| **11** | 31 | 7477ms (~7.5s) | server ping/heartbeat |
| **310** | 2 | 24ms | cursor bitmap shape upload (initial + 1 change) |

## TX inventory (= client uplink)

| len | count | cadence | meaning |
|-----|-------|---------|---------|
| **1** | 31 | 7.5s | client heartbeat reply (0x70) |
| 25 | 1 | one-shot | initial cursor register (= hash20) |

**A 1:72 asymmetry = the cursor channel is dominated by RX**.

## 47B wire format (= SUFP v3 + chacha20)

```
0000  23 01 00 00 00 00 74 5c  2f 07 01 <captured payload elided>
0010  b8 9b d6 fc 73 d1 7d 4b  e5 c3 ab 7f a1 ab 8b 65
0020  07 85 3c 4d a8 e2 07 d9  a6 16 b5 cf 8b 4f 75

= [SUFP hdr 11B]
    byte0=0x23   → version=2, type=3 (= cursor stream type)
    byte1=0x01   → sub
    byte2-3=0x00 → chunk_idx
    byte4-5=0x00 → chunk_max (= self-contained)
    byte6-9 LE   → packet_id ~120M, increments per packet
    byte10=0x01  → data flag (= encrypted)
  [chacha20 ct 16B + nonce 11B + tag 9B] = total 36B encrypted area
```

## 11B server heartbeat

```
03 00 00 00 00 00 00 00 00 00 00
```

byte0=0x03 = SUFP v=0 type=3 (= cursor ping). Zero payload. Send every ~7.5s.

## Hidden ctrl payload check ?

Across 2273 consecutive 47 B packets, **bytes 11-46 are near-identical**
(L28605 against L28785 = 100 % identical bytes). So the encrypted payload is
**cursor position only** (= very little variance, since the cursor barely moves
in this capture).

**Conclusion C90**: no ctrl message mixed in. The cursor channel = position-only
data stream.

## Client cursor pos uplink ?

**None**: 0 TX cursor packets are cursor positions uplinked to the server.
Cursor info **descend uniquement** depuis serveur. Mouse position via input
channel `:base+13` / `:base+14`, **never** on the cursor `:base+30`.

## Refs

- Script analyzer : `tools/ida/tier7_cursor.py`
- CUR1 wire decode : `memory/project_CUR1_cursor_format_2026-05-18.md`
- Capture hex : L28228 (310B bitmap), L28605/28785 (47B pos), L149752 (11B ping)
- Doc : `tools/ida/out/TIER7_RE_2026-05-16.md` §T3

## Cross-refs

- [[project-CUR1-cursor-format-2026-05-18]] (= existing impl)
- [[project-shadow-encryption-framing]] (= SUFP + chacha20 baseline)
- [[project-cursor-user-activity-RE]] (= TIER6 M4 ComChan, distinct du cursor channel)
