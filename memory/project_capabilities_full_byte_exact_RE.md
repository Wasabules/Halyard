---
name: project-capabilities-full-byte-exact-RE
description: TIER5-B1 — the 87 B ctrl Capabilities confirmed as a byte-exact match with the desktop; the 7 capability strings (display_management/gamepads/…) live in the JSON telemetry, not on the ctrl wire. No missing field gating multi-NAL.
metadata:
  type: project
---

# TIER5-B1 — Capabilities advertisement byte-exact RE complet

**Date** : 2026-05-23.  
**Source** : `tools/ida/tier5_re.py` + IDA 9.3 sur `06-shadow-recon-linux/ShadowPCDisplay.i64`.  
**Doc complet** : `tools/ida/out/TIER5_RE_2026-05-16.md` §B1.

## TL;DR

Our `ctrl_build_capabilities` (`ctrl_msgs.c:51-124`) is a **byte-exact 87 B
match with the desktop**. The ctrl Capabilities does NOT encode feature flags. The 7
strings "capability names" (`display_management`, `gamepads`,
`network_notifications`, `dynamic_bitrate`, `multiscreen`,
`streaming_profile`, `audio_out_codec`) construites par `sub_697050` vivent
in a **JSON telemetry blob** (= `sub_69B070`, 3692 lines), not in the
the ctrl wire. The server never receives that list through :base+11.

## Findings

### 1. Match byte-exact desktop (C95 verified)

```
{ f2={f3={f1=""}},
  f4={f1=2, f2="12.3.3", f3="Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3"},
  f5={f1=1, f3="OCapture"} }
```

87B exact, verified vs `tls_plain.log` MASTER capture L18534. Aucun field
missing on the Switch side.

### 2. A new proto sub-message confirmed in the rtti (C90)

| EA | RTTI string |
|----|-------------|
| 0x131A8E0 | `N12ControlProto22Capabilities_StreamingE` |
| 0x131A880 | `N12ControlProto31Capabilities_ChannelNetworkTypeE` |

`Capabilities_Streaming` (= probably f5.f2) already tested at runtime in RE12
2026-05-18 → effet 0 sur bottom-NAL (cf
`memory/project_RE11_RE12_final_attempts_2026-05-18.md`).
`Capabilities_ChannelNetworkType` untested — a C60 candidate, but probably
type-d-info (wifi/ethernet) sans effet stream quality.

### 3. The 7 capability strings = JSON telemetry, not the ctrl wire

`sub_697050` (@ 0x697050) construit vector<const char*> selon bools input.
Callers: `sub_6928C0` + `sub_69B070`. **`sub_69B070` is a JSON stats
serialiser** (= a ≥1024 B destination struct, not a byte-exact 87 B proto). See
`tools/ida/out/tier5/c2/c2_hw_sub_69B070.c` 3692 lignes.

### 4. Refs

- Builder vector : `tools/ida/out/h2h3/h3_sub_697050.c`
- JSON stats serializer : `tools/ida/out/tier5/c2/c2_hw_sub_69B070.c`
- B1 inventory : `tools/ida/out/tier5/B1_CAPABILITIES.md`

## Confidence

- C95 : match byte-exact desktop ctrl-Capabilities 87B.
- C90 : `Capabilities_Streaming` + `Capabilities_ChannelNetworkType` rtti
  exist as proto sub-messages.
- C85: the 7 capability strings live in HTTP telemetry, not on the ctrl wire.

## Implication bug taskbar

**None.** A byte-exact match + the sub-message already tested = no field
missing and exploitable on the Capabilities side.

## Cross-refs

- `[[project-ctrl-oneof-enum-VERIFIED]]`
- `[[project-RE11-RE12-final-attempts-2026-05-18]]`
- `[[project-setstreamingprofile-RE]]`
- `[[project-channel-announcements-8msgs-RE]]`
- `[[project-capabilities-reply-fields-RE]]`

## Status

C95 — closed. No RE follow-up on this axis.
