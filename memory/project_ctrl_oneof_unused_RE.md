---
name: project-ctrl-oneof-unused-re
description: "TIER2 2026-05-16 — No kRequest*/kRefresh*/kRetransmit* in the Request oneof. IDR refresh is the IFR UDP packet, NOT the ctrl proto. The bottom-slice problem CANNOT be addressed through a control message."
metadata:
  type: project
---

## Question TIER2-D

Does the Request oneof enum have a "request bottom-slice retransmit" / "request
retransmit frame X" that we could try for fixing the taskbar?

## Answer — C95

**NON.** Aucun `kRequestPicture`, `kRequestKeyframe`, `kFrameRequest`,
Neither `kRetransmit`, nor `kBottomSlice`, nor `kSliceRefresh` exists in the binary.
The enum descriptor at `0x129F8F0..0x129FA20` contains **exactly 27 cases**,
all already known and listed in `project_ctrl_oneof_enum_VERIFIED.md`.

Confirmed through `dumps/strings_display.txt:33055..33086` (= the `kFoo`
strings du descriptor protobuf) :
```
TYPE_NOT_SET, kNotify, kPing, kState, kAuthentication, kRestart, kHid,
kFlush, kRegisterSession, kUnregisterSession, kDisplayConfig, kUpdate,
kEncryption, kUpdateSession, kNotifyResolution, kNotifyVideoCommand,
kStopUsingGpu, kGetDisplayConfig, kRemoveAllDisplays, kDisplayToSafeState,
kActivateBanner, kDeactivateBanner, kLastInput, kNotifyVR,
kNotifyFileTransfer, kInitVR, kUpdateLoggerOptions
```

**Not a single "Request"/"Refresh"/"Retransmit" name in the list.**

## Direction des oneof cases (C→S vs S→C)

Reverse-engineered depuis :
- nos send-side captures LD_PRELOAD desktop (`tls_plain.log`)
- the handlers identified in `OUTPUT_VTABLE_HUNT.md`

| oneof | Direction | Usage desktop |
|-------|-----------|---------------|
| kPing, kState, kAuthentication, kHid, kFlush | C→S | bootstrap / runtime |
| kRegisterSession, kUnregisterSession, kDisplayConfig, kEncryption | C→S | bootstrap |
| kLastInput, kInitVR | C→S | runtime opt |
| **kRestart** | **S→C** | server reboot notif |
| **kNotifyResolution, kNotifyVideoCommand** | **S→C** | server change notif |
| **kStopUsingGpu, kGetDisplayConfig** | **S→C** | server query |
| **kRemoveAllDisplays, kDisplayToSafeState** | **S→C** | display state |
| **kActivateBanner, kDeactivateBanner** | **S→C** | UI overlay |
| **kNotifyVR, kNotifyFileTransfer** | **S→C** | event notif |
| **kUpdateLoggerOptions** | **S→C** | log cfg |
| **kUpdateSession (f13)** | undetermined | **0 occurrences on the desktop** — a false lead |

## IDR refresh ≠ ctrl proto — C95

The IDR refresh mechanism is **out-of-band UDP**, not the ctrl proto:

```
({}) asking for IDR (req={}), clearing the queue
({}) we waited too long for an IDR, re asking. PLS GIVE ME A FRAME!!!
AskForIdrFrame / AskForIdrFrameAsync   (strings 33290..33293)
```

The IFR packet is 6 B: `[0x49 0x46 0x52 cnt_lo cnt_hi out_id]`. Our code
already sends it (= [[project-IFR-counter-FIX-2026-05-15]]). Bytes 4-5 = an incrementing
u16 LE counter; with bytes 4-5 stuck at 0 → the server de-duplicates every IFR.

## Conclusion for the taskbar bug

**The bottom-slice gap CANNOT be addressed through a ctrl proto oneof.**
The only "refresh" available is the full-frame IDR (= the IFR), which we already
send correctly. The bottom slice is absent **at the source** (= the server's
encoder does not pack it into the majority of frames). Root cause = the
the protocol / encoding-mode selection in `RegisterSession_Video`
(= cf. TIER1-B + `project_multinal_chunks_RE.md`).

## Action items

- Mark the "ctrl oneof" lead definitively **abandoned** for
  le bottom-slice.
- **NEVER** send, client→server: `kUpdateSession`, `kStopUsingGpu`,
  `kInitVR` (sauf VR setup), `kRestart`, `kActivate/DeactivateBanner` —
  those messages are S→C, and sending them risks confusing the server.
- Keep investigating the **RegisterSession_Video** field map (see TIER1-B)
  qui a 5 bools dont 1 toggle probably multi-NAL / chunk-aggregation.

## Cross-ref

- [[project-ctrl-oneof-enum-VERIFIED]] — l'enum complet
- [[project-IFR-counter-FIX-2026-05-15]] — the IFR mechanism
- [[project-multinal-chunks-RE]] — RegisterSession_Video field map
- TIER2 doc : `tools/ida/out/TIER2_RE_2026-05-16.md` §D
