---
name: project_differential_captures_plan
description: Campaign plan — identify each Shadow channel by guided capture, one setting and one function at a time
metadata:
  type: project
---

A plan settled with the maintainer on 2026-08-26 for what comes next: stop making
hypotheses about the unexplored channels, and IDENTIFY them by contrast instead.
It is the method that resolved the audio (19.8 packets/s in silence against 170.3
with sound) and the only one that has ever produced answers on this protocol.

**Tooling ready** (written on 2026-08-26):
- `tools/capture_scenario.sh <name>` — launches the OFFICIAL client hooked, with
  `SHADOW_TLS_MAX_DUMP=65536` (the 4096 default hid the 11 KB bodies of
  `POST /stats` and truncated large frames), and lets you mark every action from
  the keyboard into `actions.tsv`.
- `tools/analyze_scenario.py <directory>` — traffic per port and per window, plus
  the list of ports SPECIFIC to a window: that last table is what identifies a
  channel.

**Scenarios to run**, one single change at a time:
1. transport **UDP** then **TCP** (a Shadow setting, before launching the stream)
2. audio quality **HIGH** then **REGULAR**
3. clipboard: copy/paste in both directions
4. file transfer
5. the mic (`:base+32`, which the official client opens without ever using)
6. the gamepad: it goes through **Spice over WebSocket** via `POST /N/devices`
   (the `usb` token), NOT through `:base+13` where we emit today — see
   [[project_shadow_usbredir_protocol]]

**What we are looking for**: the real channel map. Today we ANNOUNCE eight
channels and serve half of them — Mic (`+32`) and Transfer (`+15`) never have a
socket open, and yet the server gives us an ed25519 key for the second one in its
522-byte announcement reply (never to be logged).

**Confounders to remove BEFORE any measurement** (KB §3.35): the physical gamepad
unplugged, a single VM named and logged, one process per session.
