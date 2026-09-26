---
name: project-multinal-chunks-RE
description: "TIER1 2026-05-16 — An RE of the server-side multi-NAL packing trigger. The RegisterSession_Video proto has 5 booleans + 2 u32s. One of the booleans (probably field 4/5/6/8/10 = bytes 44/45/46/47/52) triggers multi-NAL. C70, narrowed to 5 candidates. A plaintext capture is needed for byte-exactness."
metadata:
  type: project
---

# Multi-NAL server-side packing trigger RE — TIER1 2026-05-16

## Question

Capture desktop MASTER 2026-05-14 (= `project_image_100pct_PROOF.md`) prouve :

| Metric | The official desktop | The Switch client |
|---|---|---|
| Top NAL (first_mb=0) | 698 (89.3%) | 1042 (100%) |
| Bottom NAL (first_mb=4080) | 84 (10.7%) | 0 |
| **Multi-NAL packets (2-3 NAL/ct)** | **132** | **0** |
| Packet sizes | 40-1280 B variables | 1241 B uniform |

The desktop **receives** multi-NAL packets; we **do not**. Which client-side toggle triggers multi-NAL on the server?

## RegisterSession_Video proto (C95 byte-exact)

The serialiser = `sub_10E60A0` (= the proto encoder). The mapping field# ↔ wire-tag ↔ source offset:

| Wire-tag (varint) | Field# | Type | Source offset (a1+) | Confidence |
|---|---|---|---|---|
| 0x08 (= 1<<3 \| 2) | 1 | submessage | +24 | C95 |
| 0x10 (= 2<<3 \| 2) | 2 | submessage | +32 | C95 |
| 0x18 (= 3<<3 \| 0) | 3 | u32 varint | +40 | C95 — probablement `bitrate` |
| 0x20 (= 4<<3 \| 0) | 4 | bool | +44 | C95 |
| 0x28 (= 5<<3 \| 0) | 5 | bool | +45 | C95 |
| 0x30 (= 6<<3 \| 0) | 6 | bool | +46 | C95 |
| 0x38 (= 7<<3 \| 0) | 7 | u32 varint | +48 | C95 |
| 0x40 (= 8<<3 \| 0) | 8 | bool | +47 | C95 |
| 0x50 (= 10<<3 \| 0) | 10 | bool | +52 | C95 |
| 0x5A (= 11<<3 \| 2) | 11 | bytes | +16 | C95 — vrSystem string |

Field 9 = **absent** du serializer (= probable removed/deprecated field).

## Multi-NAL trigger candidates (C70, narrowed to 5)

The 5 booleans (fields 4, 5, 6, 8, 10) are the only plausible "feature toggles". Field 7 (a u32) is also a possible candidate (= a `packetization_mode` enum?). Field 3 (a u32) is very probably `bitrate`, given its ordering and size.

### Plan A/B test (= 5 runs × 30 min)

1. Build the Switch client with **field 4 toggled to 1** in our RegisterSession_Video → measure the `MULTI_NAL_SEEN` stats counter (to be added in `ctrl_session.c`).
2. Si 0 → toggle field 5 = 1, retry.
3. Etc. pour fields 6, 8, 10.

A field that moves the counter from 0 → > 0 = **the multi-NAL flag**.

## Path desktop → server : RegisterStreaming

```
CtrlChanV2Manager::RegisterStreaming  sub_853770
  → envoie ctrl msg type 8 = kRegisterSession (oneof case 8)
  → contient sous-message RegisterSession_Video (= sub_10E60A0 serializer)
  → contient `bitrate, codecId, ...flags..., vrSystem`
```

8 channel announcements are sent at bootstrap (cf. `ctrl_msgs.c::CI_BODY_5..12`). The RegisterSession message contains a **nested** RegisterSession_Video for the Video stream. That is where the 5 booleans are set.

## RegisterSession_StreamingProtocol enum (C95)

Strings rodata 0x12A00A0..0x12A01E0 :
- `RegisterSession_StreamingProtocol_ProtoType_SFTP`
- `RegisterSession_StreamingProtocol_ProtoType_FlatBuffers`
- `RegisterSession_StreamingProtocol_ProtoType_SCP`
- `RegisterSession_StreamingProtocol_ProtoType_SSP`
- `RegisterSession_StreamingProtocol_ProtoType_SSUFP`
- `RegisterSession_StreamingProtocol_ProtoType_SUFP`

Our client negotiates `SUFP` (= byte10 SUFP v3 chunks). The desktop might negotiate `SSUFP` (= "Secure SUFP", potentially with multi-NAL or pre-chacha20). To be tested as a variant.

## Autres pistes (C60)

- `SetVideoPacketsRedundancy(float)` = `sub_859530`. Changing that float changes the amount of redundancy/aggregation on the server side.
- The `Authenticate(bool, ...)` (= sub_7CF2A0 line 757) packs **8 boolean feature flags** into a u16 sent through `Output->vtable[+192]`. Decoding:
  ```
  flags = (v105 << 8)
        | (4 * bit0) | (8 * bit1) | (bit2 << 7) | (bit3 << 6)
        | (32 * bit4) | (16 * bit5) | (2 * bit6)
  ```
  Without a bit→name map, pinpointing it is impossible. A plaintext capture is needed.

## Why C is not enough (= a client-side NAL splitter)

Our Switch client already uses `h264_decoder_feed_annexb(buf, len)`, which calls `av_parser_parse2` (libavcodec) — that automatically scans for the `00 00 00 01` start codes and splits the NALs internally. **If the server were sending multi-NAL chunks, our code would decode them correctly.** So the bug really is the **server-side packing strategy**, not client-side splitting.

## Next concrete step

An LD_PRELOAD `SSL_write` hook on the Linux desktop side → dump the **complete RegisterSession ctrl message** (= the protobuf bytes of the encrypted payload for kRegisterSession=case 8). Decode it field by field. Identify bytes 44/45/46/47/52 (= the 5 booleans). Mirror it on the Switch side.

Estimated: 1 h.

## Refs

- [[project-image-100pct-proof]] — preuve empirique gap multi-NAL
- [[project-ctrl-oneof-enum-VERIFIED]] — ctrl message oneof mapping
- [[project-sufp-v3-wire-RE]] — SUFP v3 wire format
- `tools/ida/out/TIER1_RE_2026-05-16.md` §B
- `tools/ida/out/tier1/reg_sub_10E60A0.c` — RegisterSession_Video serializer Hex-Rays
- `tools/ida/out/tier1/reg_sub_853770.c` — RegisterStreaming caller
- `tools/ida/out/tier1/REG_HUNT.md` — strings + xrefs RegisterSession family
