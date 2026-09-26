---
name: project_G52_G53_gamepad_downstream
description: G52/G53 — the gamepad announcement could not be sent on desktop (a double compile guard), and :base+13's downstream direction was not being read
metadata:
  type: project
---

A 50-agent investigation into the gamepad channel, 2026-08-28, with a refutation pass.
Deux constats decisifs. Details complets : `KB.md` §3.40 et §3.41.

## G52 — the announcement could NOT be sent on desktop

G50 replaced the direct `ctrl_gamepad_attach` send with an ARMING
(`g_plug_arme = 1`), drained by `ctrl_gamepad_replug_tick()`. The ONLY caller of
that tick, `padforward::poll()`, has its body entirely under `#ifdef __SWITCH__`
(`input/pad_forward.cpp:116-186`), and its only call
(`activity/stream_view.cpp:1649`) is itself inside a Switch block. **A dead path
deux fois.**

Effect: the VM creates NO gamepad, the evdev reader emits into the void, the
diagnostic affiche « annonce ARMEE » indefiniment. **Aucune erreur nulle part** —
this protocol's usual signature ([[feedback_absence_erreur_ne_prouve_rien]]).

**Worth remembering: every desktop gamepad A/B made since G50 is INVALID.**
That is the confounder KB §3.35's series was missing.

The flush now happens in `ctrl_session.c`'s receive loop (both platforms), guarded
with `#ifndef __SWITCH__`: on console `pad_forward` already flushes, and two
flushes would fight over G51's pulse queue, which has no lock.

## G53 — the server was answering, we were not listening

`:base+13` is bidirectional; we were only doing `send()` on it. Three
messages descendants, octet 2 = type :

    04 00 08 03 … [11]=01   kReply a kPlug    -> l'annonce a ABOUTI
    04 00 08 04 … [11]=00   kReply a kUnplug
    04 00 07 …              kVibration

**The device identifier is 255 = "unassigned" until the reading of
ce kReply.** Telemetrie officielle : `gamepad 1 - 255 - UNDEFINED -> PLUGGING`
then `gamepad 1 - 0 - PLUGGING -> PLUGGED`. Without reading it, our gamepad stays in
the equivalent of PLUGGING: no announcement confirmation, no rumble routing
(le binaire officiel porte `Received vibration message for unknown remote device ID`).

Three differences from the video and the audio, all of them traps:
- **enveloppe NUE** `[ct 14][nonce 12][tag 16]`, 42 octets, **sans en-tete SUFP** ;
- chaque reponse envoyee **trois fois** a ~10,5 ms : lecture **idempotente** obligatoire ;
- decryption **without replay protection** — the cipher is shared with the video,
  which has its own nonce space; replay protection here would get pictures rejected.

Voir aussi [[project_G49_manettes_fantomes]] (annonce unique), [[project_channel_map_proven]].
