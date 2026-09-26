---
name: project-byte0-upper-nibble
description: The SUFP wire's byte0 decomposed — bits 0..3 = the packet type (3 for a data chunk), bits 4..7 = a "flag" stored as a u16 (= IDR=1, P-frame=2). NOT a SoF indicator, as agent V2 believed.
metadata:
  node_type: memory
  type: project
---

# SUFP wire byte 0 — the breakdown

## Reference source
- IDA Hex-Rays `sub_D85FA0` (`UdpDataChunk::init`) at `tools/ida/out/sub_D85FA0.c` (475 L)
- Runtime confirmation: `[H1.V7 dbg]` logs with `byte0=0x13` and `byte0=0x23`

## Layout byte 0
```
bits 7 6 5 4 | 3 2 1 0
  upper nibble | lower nibble
  (= "flag")   | (= "type")
```

- **Lower nibble (type)**: 3 = a data chunk (= our case). Other types: 15 = a ping packet (sub_D85FA0:140).
- **Upper nibble (flag)**: stored at `chunk+0x28` as a u16 (= the upper part extends to 16 bits). Accessed through `chunk_get_flag()` (= `sub_D80840`). Values observed at runtime:
  - **`flag=1`** (byte0=0x13): **IDR keyframe** chunks (= subchan=0, max=25). 10 data + 15 parity.
  - **`flag=2`** (byte0=0x23): **P-frame** chunks (= subchans 1, 10, 11, … max=1, 4, 6, 7, 8, 10, 11, …). 21 data + 154 parity over ~200 chunks.

## Anti-pattern : agent V2 deep avait dit "flag=1 = SoF" — FAUX
The V2 agent (H1_SUFP_deep_v2.md §A.3) said `flag(chunk) == 1` = start-of-frame. Runtime shows that **flag=1 is constant** for ALL the chunks of an IDR keyframe (= idx 0..24), not just the first.

So `flag` is a **stream TYPE marker** (= IDR against P), not a boundary indicator.

## SoF (start of frame) detection — another mechanism
The SoF is in fact signalled by `chunk_idx == 0` ALONE (= a 0-indexed wire, count semantics).

## The complete UdpDataChunk struct layout (verified byte-exact)
| Offset | Type | Field | Wire bytes | Notre nom |
|---|---|---|---|---|
| +0 | ? | (= pointer/vtable) | - | - |
| +8 | qword | wire buffer ptr | - | - |
| +16 | qword | wire size | - | - |
| +0x20 | int | header_len (= 11 for type 3) | - | - |
| +0x28 | u16 | flag (= byte0's upper nibble) | byte0 >> 4 | not parsed |
| +0x2A | u8 | seq_id (= byte 1) | byte1 | parsed as `subchan` |
| +0x2C | u16 LE | chunk_idx (= bytes 2-3) | byte2-3 | parsed as `chunk_idx` |
| +0x2E | u16 LE | max_chunks (= bytes 4-5) | byte4-5 | parsed as `max_chunks` |
| +0x30 | u32 LE | timestamp/frame_id (= bytes 6-9) | byte6-9 | parsed as `g_last_frame_id` |
| +0x38 | bit | the data flag (= byte10 & 1) | byte10 & 1 | parsed as `flag10` |

Notre code parse correctement seq_id, chunk_idx, max_chunks, timestamp, flag10.
**`flag` (= byte0's upper nibble) is NOT used** on our side — runtime shows it is not critical.

## Implication
Our code loses no useful information by ignoring byte0's upper nibble. The `flag` is only a descriptive marker (= IDR/P). Our SoF detection through `chunk_idx == 0` is correct.

Cf. [[project-image-50pct-state-2026-05-15]]
