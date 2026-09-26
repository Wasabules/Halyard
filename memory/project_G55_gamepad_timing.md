---
name: project_G55_gamepad_timing
description: G55 — the gamepad did not work because we spoke on :base+13 too soon after the grant; the server tore the channel down before our announcement
metadata:
  type: project
---

RESOLVED on 2026-08-28, verified on console: **Steam inside the VM sees the
gamepad**. Detail: KB §3.40, §3.42; report
`captures/tests_autonomes_20260828/G55_manette_channel_down.md`.

## The mechanism

The server emits `CHANNEL_DOWN canal=CONTROLLER` when you put a byte on
`:base+13` within ~500 ms of the channel being granted. Our plug announcement then
went out **on an already-dead channel**.

We were speaking at grant+12 to +299 ms. **The official client does not even OPEN
the socket before grant+518 ms**, sends its kPlug at +529 ms and receives its
kReply 20.6 ms later.

**The determinant is the MOMENT, not the content** — an internal A/B: sessions
with no gamepad put the SAME encrypted packet, on the SAME channel, with the SAME
key, but at 8-19 s: zero CHANNEL_DOWN out of 23. At ~200 ms: 57 out of 61.

It is the same family as [[project_S57_audio_solved_we_spoke_too_early]]. The
mechanism had been documented for two days and had not been applied to the
gamepad. **A reflex worth keeping: on this protocol, when a channel does not
answer, ask FIRST whether we spoke to it too soon.**

## Two fixes

1. `udp_register`'s 25-byte CLEARTEXT datagram disappears from `:base+13`. The
   compiled default was already `false`, but the SD's `settings.txt` carried
   `udp_register_input=1`: **the key was RENAMED** to `udp_register_input_v2` so
   that the old value became inert without touching the file.
2. An order guard in `send_payload` (the channel's only point of passage) gated on
   "has the kPlug gone out", not on "is the announcement armed".

**Do NOT lower `SHADOW_GAMEPAD_DELAY_MS`**: G50's 500 ms are right, the error was
what PRECEDED them.

## What was refuted along the way

- the "gamepads" capability: the official client's capability field is **EMPTY**;
- the encryption key: `ctrl_tcp.c` encrypts the control channel with the same
  `key_tx`, and that channel works;
- phantom gamepads: the VM was restarted, the symptom unchanged.

See also [[project_G49_manettes_fantomes]], [[feedback_methode_capture_et_bureau]].
