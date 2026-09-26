---
name: project-ssufp-prototype-RE
description: RegisterSession_StreamingProtocol.protoType — confirmed NOT SENT by the desktop; the multi-NAL hypothesis is ELIMINATED at C95
metadata:
  type: project
---

# SSUFP protoType — TIER4 axe A — verdict NOT-A-FIX

**Date** : 2026-05-23. **Confidence** : C95.

## TL;DR

The `RegisterSession_StreamingProtocol` proto and its `ProtoType` enum do exist
in the desktop binary indeed (the serialiser `sub_10E7490`, the rodata strings @
0x12A00A0..0x12A01E0). BUT **the desktop NEVER sends that message in its
the bootstrap**. The MASTER capture (`tls_plain.log`) shows a strict sequence
with no Streaming sub-msg at all:

```
seq=1 Capabilities → seq=1 Auth → seq=2 Encryption → seq=3 DisplayConfig+EDID
→ seq=4 Heartbeat → seq=5..12 8×ChannelAnnouncement
```

The 247 B `seq=3` is byte-decoded ([[project-channel-announcements-8msgs-RE]])
and contains only an EDID + Resolution + Scale. No Streaming sub-message.

## Pourquoi l'enum existe

Le `ProtoType` enum (FlatBuffers=0, SCP=1, SFTP=2, SSP=3, SSUFP=4, SUFP=5)
is a **server-side type descriptor**. The server tags each open port with
a protoType (= UDP video → SUFP, TCP video :base+20 → SSUFP, file transfer
:base+15 SSH → SFTP). The client does not need to re-declare it: the server
decides it by allocating the ports.

It is confirmed by ENCRYPTION_AND_FRAMING.md §3.5 + the Ghidra ExtractMQStack.java
which shows the callers would be `DebugString` reflection (= not a wire send).

## Implications

- The §4.2 hypothesis in `CHANNEL_ANNOUNCEMENTS_RE.md` (= "send protoType=SSUFP
  to trigger multi-NAL") is **refuted**. If it were the trigger, the desktop
  would have sent it.
- The multi-NAL gating is NOT in this layer.
- **No code to write** on the client side. The
  `ctrl_build_streaming_protocol_announcement()` proposed in the
  `CHANNEL_ANNOUNCEMENTS_RE` document is pointless.

## Refs

- The serialiser: `tools/ida/out/channels/FINAL_Streaming_serB_sub_10E7490.c` (sub_10E7490 @ 0x10E7490)
- Enum strings : `06-shadow-recon-linux/dumps/strings_display.txt:33102-33107`
- Capture : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log` L18534..L22953
- KB cross-ref : `06-shadow-recon-linux/ENCRYPTION_AND_FRAMING.md §3.5`
- Full documentation: `tools/ida/out/TIER4_RE_2026-05-16.md §A`

## Action items

**None.** This lead is definitively closed. The multi-NAL trigger is
elsewhere (= probably gated on real user input = it needs an interactive
desktop capture with a real mouse/keyboard).
