---
name: project-remote-bitrate-estimator-RE
description: "RBE = a mixin base of VideoUdpChannel, not a separate channel. It emits `gE` (15 B, 50 ms) + `rG` (NACK) on `:base+10` (the same UDP socket as the video RX). The byte-exact `gE` format = gE 01 | u32_LE bytes*4 | s32_LE avg_delay_us | u32_LE sufp_id."
metadata:
  type: project
---

## Question

What is `RemoteBitrateEstimatorIOChannel` (RTTI @ 0x12A9D20) — a port
missing? The format of the packets it emits? The hypothesis "the server gates multi-NAL emission
on receiving RBE feedback" → opening that channel could fix the taskbar bug
(bottom NAL ratio 0.81% vs 11% desktop) ?

## The answer

### 1. RBE is NOT a network channel — C95

`RemoteBitrateEstimatorIOChannel` is a **base class** inherited by
`VideoUdpChannel` (multiple inheritance). The RBE sub-object is at
`vuc + 1312` (= 0x520). **No dedicated port is opened** — every emission
goes through VideoUdpChannel's UDP socket (= `:base+10`, the same as the video
RX).

La capture `tls_plain.log` MASTER confirme : `UDP_SENDMSG sockfd=260
peer=...:8010 len=15` (the same fd as the video RX); no other socket that was not already
identified.

→ **Conclusion** : nos ports manquants `:base+14`, `:base+15`, `:base+32`
(observed in captures but not implemented) are **NOT** tied to RBE. That
are other channels (Gamepad/Clipboard/ComChan/FileTransfer/Sftp — the RTTI
exists but the usage is unconfirmed).

### 2. Two packets emitted: `gE` (feedback) + `rG` (NACK) — C95

Buffer init en ctor `sub_C476C0` @ 0xC476C0 (= called from VUC ctor) :
```c
vuc[1312+376] = new[](1472);   // rG buf, pre-stamped "rG\x01..."
vuc[1312+392] = new[](1472);   // gE buf, pre-stamped "gE\x01..."
```

### 3. `gE` wire format byte-exact — C95

**15 octets** :
```
[0..1]  magic    "gE" = 0x67 0x45
[2]     u8       version = 0x01
[3..6]  u32_LE   bandwidth_signal = bytes_rx_window * 4
[7..10] s32_LE   avg_packet_delay_us (signed)
[11..14] u32_LE  last_received_sufp_frame_id
```

Échantillon capture :
```
67 45 01 | dc 86 01 00 | f4 fe ff ff | 57 88 66 07
gE  v=1 | bw=99548     | delay=-268µs | sufp_id=0x07668857
```

**Period**: exactly **50 ms** (~20 Hz) — confirmed by the LD_PRELOAD
timestamps (48-53 ms between two `UDP_SENDMSG` len=15 on `:8010`).

**Trigger** : inline dans `OnSufpChunkReceived` (sub_C49FE0 @ 0xC49FE0) lignes
729-741. On every video chunk received, it checks `(now_us - last_send_us) > 50000` →
emit. The fields are built from the decoder's state (= `bytes_received_window`,
`delay_sum_us / packet_count`, `last_sufp_id`).

**Send** : virtual call `vt[4]` = `sub_DA4A20` = `UdpIOChannel::TrySend`
(`udp_io_channel.cpp:386`) → libuv's `uv_udp_try_send` on the video UDP socket.

### 4. The `rG` wire format, byte-exact — C95 (already partly RE'd in TIER2 §E)

`6 + 2*count` octets :
```
[0..1]  magic    "rG" = 0x72 0x47
[2]     u8       version = 0x01
[3]     u8       subchan_id
[4..5]  u16_LE   count (= # chunk_idx)
[6..]   u16_LE[] chunk_idx values
```

Trigger : deux paths dans `OnSufpChunkReceived` :
- `sub_C464A0` @ 0xC464A0: on reception, scan the bitmap, emit a NACK for the subchan
  actuel.
- `sub_C46A50` @ 0xC46A50 : sur subchan advance, emit NACK per missing chunk +
  empty `rG` (cnt=0) en marker.

Send: also through `vt[4]` = `UdpIOChannel::TrySend` on `:base+10`.

### 5. Architecture — C95

```
VideoUdpChannel object (≥ 1920 B) :
  +0          primary vptr (VUC, vt @ 0x12A9110)
  +0x520     ← RBE subobject base
    +0x180   ← rG buf write head (pointer)
    +0x190   ← gE buf write head (pointer)
    +0x178   ← rG buf alloc (1472 B)
    +0x188   ← gE buf alloc (1472 B)
    +0x1D8   ← pthread_mutex_t
```

Class hierarchy:
- `RemoteBitrateEstimatorIOChannel` (mixin) inherits from `SufpUdpIOChannel`,
  `UdpIOChannel`, `MessageIOChannel` (= base IO channels).
- `VideoUdpChannel` inherits from `RemoteBitrateEstimatorIOChannel` + other
  bases, RBE subobject at `+1312`.

Vtable @ 0x12A9110 (VUC primary) :
- vt[4]  = `UdpIOChannel::TrySend` (sub_DA4A20) — used by gE/rG sender
- vt[5]  = `SufpUdpIOChannel::OnDataReceived` (sub_D7F1E0) — RX dispatcher
- vt[13] = `OnSufpChunkReceived` (sub_C49FE0) — RBE main entry, 881 lignes

### 6. Why our Switch suffers from it — a C50 hypothesis

Our Switch receives the SUFP video but never emits `gE`. Hypothesis:
- The server uses `gE.bandwidth_signal` + `gE.avg_delay_us` to drive the
  GoogCC / GCC congestion control on the encoder side.
- Without `gE`, the server stays in "client bandwidth unknown → conservative
  mono-NAL encoding" mode.
- → a bottom NAL ratio of **0.81%** (= the server emits no multi-NAL chunks) against
  **11%** on the desktop (= the desktop sends `gE`, so the server unshackles the encoder).

To be validated by implementing §IMPL of `tools/ida/out/REMOTE_BITRATE_ESTIMATOR_RE.md`.

### 7. Implication TIER2 §E (rG NACK)

Our N29 failure "the rG NACK kills the session" (392→66 pps) is better explained now:
- **rG alone (without the accompanying `gE`) = a client orphaned** from the feedback stream.
- The server detects "the client sends NACKs but no BW telemetry" =
  broken/buggy client behaviour → it throttles.
- **If we send `gE` + `rG` together**, it becomes a "normal" client again.

So TIER2's conclusion "rG is useless" is **half true**. rG is fine but it
needs `gE` generated alongside it so as not to be interpreted as spurious.

### 8. Confidence ladder

| Finding | C |
|---------|---|
| RBE = mixin base of VideoUdpChannel (multi-inheritance) | C95 |
| No dedicated socket (= it emits on `:base+10`) | C95 |
| `gE` is 15 bytes, 6 fields, a 50 ms period | C95 |
| `rG` is 6+2*cnt bytes (= a TIER2 §E confirmation) | C95 |
| `bytes * 4` is a bandwidth signal | C80 |
| `delay_us` signed = jitter / network slack indicator | C80 |
| `sufp_id` u32_LE = dedup / RTT marker | C80 |
| Server gate multi-NAL on RBE → fixe taskbar bug | **C50 — TODO test** |

## Action items

- Implement `gE` in `streaming/ctrl_session.c` + `sufp.c`.
- Hook a 50 ms timer, or inline it in the SUFP chunk reception.
- Live Switch test: `tcpdump` must see `67 45 01 ...` at 20 Hz on `:base+10`.
- Measure the bottom NAL ratio after the fix → if it is > 5%, the hypothesis is confirmed.
- If that does not fix it, add `rG` (= per-subchan tracking, more complex).

## Cross-ref

- [[project-rg-nack-format-v2-RE]] — the TIER2 version of the same format (to be updated)
- [[project-V12-taskbar-status-2026-05-15]] — the current state of the bug
- [[project-image-50pct-state-2026-05-15]] — the KPIs (0.81% bottom NAL)
- [[project-image-100pct-PROOF]] — the initial reference state
- The TIER3 doc: `tools/ida/out/TIER3_RE_2026-05-16.md` §M (= the one that mentioned
  RBE as a hypothesis, now confirmed and superseded)
- Full documentation: `tools/ida/out/REMOTE_BITRATE_ESTIMATOR_RE.md`
