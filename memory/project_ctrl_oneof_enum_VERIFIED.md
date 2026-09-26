---
name: project-ctrl-oneof-enum-verified
description: "DECISIVE 2026-05-14 — The complete enum of the Request oneof's 26 cases RE'd through Ghidra (ghidra_video_command_v2.txt). The field# ↔ kFoo mapping confirmed for every SslCtrlChanV2 message."
metadata: 
  node_type: memory
  type: project
---

## Request oneof — 26 cases (de l'enum descriptor @ 0x129fa20)

| field | name                       | usage |
|-------|----------------------------|-------|
| 0     | TYPE_NOT_SET               | sentinel |
| 1     | kNotify                    | ? |
| 2     | kPing                      | 3× desktop |
| 3     | kState                     | **heartbeat** (2Hz desktop) |
| 4     | kAuthentication            | bootstrap (1×) |
| 5     | kRestart                   | ? |
| 6     | kHid                       | DisplayReady (~4/min desktop) |
| 7     | kFlush                     | Request_Flush (~3.5/min desktop) |
| 8     | kRegisterSession           | **channel registration** (× 8 at bootstrap) |
| 9     | kUnregisterSession         | cleanup (× 8 at session end only) |
| 10    | kDisplayConfig             | **EDID + resolution + scale** (= what our code calls "RegisterSession") |
| 11    | kUpdate                    | ? |
| 12    | kEncryption                | bootstrap (1×) |
| 13    | kUpdateSession             | **NEVER sent by the desktop** (= a false lead) |
| 14    | kNotifyResolution          | resolution change |
| 15    | kNotifyVideoCommand        | video cmd (kFlush etc.) |
| 16    | kStopUsingGpu              | ? |
| 17    | kGetDisplayConfig          | ? |
| 18    | kRemoveAllDisplays         | ? |
| 19    | kDisplayToSafeState        | ? |
| 20    | kActivateBanner            | ? |
| 21    | kDeactivateBanner          | ? |
| 22    | kLastInput                 | ? |
| 23    | kNotifyVR                  | ? |
| 24    | kNotifyFileTransfer        | ? |
| 25    | kInitVR                    | ? |
| 26    | kUpdateLoggerOptions       | ? |

## Notre code (ctrl_msgs.c) — mapping correct

- `pb_write_submsg(..., 0, 4, ...)` → Authentication ✓ (= f4 = kAuthentication)
- `pb_write_submsg(..., 0, 8, ...)` → Channel announcement ✓ (= f8 = kRegisterSession)
- `pb_write_submsg(..., 0, 10, ...)` → Display setup (EDID) ✓ (= f10 = kDisplayConfig — commentaire trompeur dit "RegisterSession")
- `pb_write_submsg(..., 0, 12, ...)` → Encryption ✓ (= f12 = kEncryption)
- `pb_write_submsg(..., reqo, 14, ...)` → NotifyResolution ✓
- `pb_write_submsg(..., reqo, 15, ...)` → NotifyVideoCommand ✓

**Misleading comments in the code**: what we call "RegisterSession" is in fact DisplayConfig. What we call a "channel announcement" is in fact RegisterSession (per channel).

## UpdateSession_Video (kUpdateSession's f2) — RE'd but useless

Format wire (FUN_010e6cb0 serializer):
- f1 bool (tag 0x08) — offset +0x10
- f2 uint32 (tag 0x10) — offset +0x14
- f3 fixed32 float (tag 0x1d) — offset +0x18
- f4 uint32 (tag 0x20) — offset +0x1c
- f5 fixed32 float (tag 0x2d) — offset +0x20

**The desktop does not use it** (0 occurrences in 4 min). Conclusion: the video configuration happens through RegisterSession_Video at bootstrap, not through a runtime update.

## Conclusion bottom slice problem

Every ctrl-message difference between the desktop and the Switch is a **frequency** (kState 2 Hz against 1 Hz, kHid 4/min against 1×, kFlush 3.5/min against 1 Hz). No message type is unique to the desktop.

The server **really does** send ~10 % bottom slice (= the desktop receives the same, but its renderer handles it better). For a 100 % picture: work on the **decoder** side (cache the IDR's bottom MBs as a reference for subsequent P-frames), not on the ctrl side.
