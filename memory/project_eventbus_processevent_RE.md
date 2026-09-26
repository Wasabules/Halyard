---
name: project-eventbus-processevent-RE
description: TIER5-B4 — the desktop's EventBus is strictly intra-process (Notify events, server→GUI), and no UI event triggers a ctrl SSL_write. The "a UI event triggers multi-NAL" hypothesis is REFUTED.
metadata:
  type: project
---

# TIER5-B4 — EventBus / ProcessEvent client-side triggers RE

**Date** : 2026-05-23.  
**Source** : `tools/ida/tier5_re.py` + IDA 9.3 sur `06-shadow-recon-linux/ShadowPCDisplay.i64`.  
**Doc complet** : `tools/ida/out/TIER5_RE_2026-05-16.md` §B4.

## TL;DR

The desktop has an internal EventBus through `PostEvent` / `ProcessEvents`. **9
NotifyEvent types identified**, all in the server→GUI direction (= the dispatch
of a ctrl-chan reply towards the GUI Observers). **No UI event
triggers a ctrl SSL_write**. The "a user-activity event triggers
multi-NAL via wire" RÉFUTÉE C80.

## Findings

### 1. NotifyEvent types inventoried

| Event | Source rtti EA |
|-------|----------------|
| `ShadowVmProxyNotifyEvent` | 0x123F5A0 |
| `ShadowCtrlChanV2NotifyEvent` | 0x123F620 ⭐ |
| `ShadowClipboardNotifyEvent` | 0x123F720 |
| `ShadowCursorNotifyEvent` + `ShadowCursorReceivedEvent` | 0x12491C0 |
| `ShadowInputNotifyEvent` + `ShadowInputPositionEvent` | 0x1249260 |
| `ShadowGamepadNotifyEvent` | 0x1239200 |
| `ShadowStatusSubscribeInstallStatusEvent` | 0x1208920 |
| `ShadowStatusRequestSubscribeInstallStatusesEvent` | 0x12088E0 |

They all follow the `App::HandleEvent(const Event&)` pattern — receivers, not
emitters towards the wire.

### 2. Direction du flux

```
SSL_read on :base+11 ctrl chan
  → parse Reply.f7 (NotifyResolution) / Reply.f15 (NotifyVideoCommand)
  → instantiate ShadowCtrlChanV2NotifyEvent
  → ProcessEvent(event)
  → Observers (= GUI stream_view) update local state
```

The reverse is NOT observed: no UI event ends up as a ctrl SSL_write.

### 3. PostEvent + ProcessEvents = generic dispatcher

- `sub_88BF80` (PostEvent) — 2932 lines, a generic event poster. No
  SSL_write in the path.
- `sub_1077EA0` (ProcessEvents) — the main event loop. No SSL_write
  in the path.

### 4. SSL_write inventory

Seules sources de SSL_write ctrl :
- Bootstrap (= Capabilities, Auth, Encryption, DisplayConfig, 8× ChannelAnnouncement)
- Periodic Heartbeat 2Hz (= `0x70` 1B, `sub_D67430` pour cursor channel)
- IFR / Flush (= UDP, pas ctrl)

None of those sites is triggered by a UI event.

### 5. Refs

- B4 inventory : `tools/ida/out/tier5/B4_EVENTBUS.md`
- Dispatchers dumps : `tools/ida/out/tier5/b4/b4_disp_sub_*.c` (9 fns)

## Confidence

- C90: 9 NotifyEvent types identified byte-exact through the rtti strings.
- C80: a single direction, server→GUI, never the reverse.
- C80: no UI event → SSL_write correlation observed.

## Implication bug taskbar

**None.** The EventBus is intra-process decoupling. Multi-NAL cannot
be triggered by a desktop UI event. The "real user input
desktop → the server's bitrate ramp" has to go through an interactive runtime
capture (= blocked without WSL2), not through a static IDA RE.

## Cross-refs

- `[[project-ctrl-oneof-unused-RE]]`
- `[[project-channel-announcements-8msgs-RE]]`
- `[[project-session-lifecycle-msgs-RE]]`

## Status

C80 — closed. No RE follow-up is possible on this axis.
