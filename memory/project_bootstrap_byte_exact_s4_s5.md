---
name: project-bootstrap-byte-exact-s4-s5
description: "2026-08-21 — The bootstrap made byte-exact: Authentication 245 B (permissions 0x1fe, a single channels_per_stream entry) and kHid 99 B (f3 was missing). Conformance gains, with no effect on the input."
metadata:
  type: project
---

A diff of our real bytes (`SHADOW_DUMP_AUTH`) against the desktop capture:

**Authentication (f4)**: 309 B on our side against 245 B. Two causes:
- `permissions` = 250 instead of **510 (0x1fe)** — `AUDIO_IN` was missing
  (0x004) and `FILE_TRANSFER` (0x100). The desktop sets all 8 bits.
  → `SHADOW_PERM_ALL`, override `SHADOW_AUTH_PERMS`.
- `channels_per_stream` (f7): **F3 declared 9 stream types**, the official one
  declares **only one** (`Video → UDP`). The exact gap is +64 B. F3 declared, notably,
  `Input → UDP` when the input goes over TCP. It was a hypothesis ("don't
  look entry-tier") that was never confronted with the capture.
  → `SHADOW_AUTH_FULL_MAP` defaults to 0.

Result: **245 B against 245 B**, structure and lengths identical field by field.

**kHid (f6)**: the desktop emits `f6 { f1{f1=1}, f2{}, f3{} }` (99 B); we were
omitting `f3{}` (97 B). → `SHADOW_HID_F3` defaults to 1.

**No effect on the input**: an A/B on the I9 harness (the Windows key → a spike in
video bitrate) gives 1.18× against 1.15×, i.e. the VBR ramp in both branches.
The input's cause is elsewhere — see [[project-input-port-not-base14]].

`KB.md` §3.22.
