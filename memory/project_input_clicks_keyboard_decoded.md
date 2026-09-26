---
name: Clicks and keyboard decoded byte-exact (KB §3.20)
description: 2026-08-21 — Complete tables of the official client's input messages, obtained through guided captures with isolated transitions. A 152 B click (@150 the button index, @140, the press/release structures), a 144 B keyboard message (@142 the scancode, @110 the state).
type: project
---
## Clics — 152 B

    APPUI        @106=16 @136=8 @138=8 @144=8
                 @140 = 0 (gauche) / 6 (droit, milieu)
                 @150 = 0 gauche · 1 droit · 2 milieu
    RELÂCHEMENT  @106=18 @136=0 @138=6 @144=6 @140=8 @150=0
    LEFT RELEASE: the official client sends NOTHING (6/6, including when
    releasing the left one first while the right is held — it is the button, not the order)

It is **not** a multi-button mask: the simultaneous LR state produces no
an unseen value, the protocol emits one message per transition.

## Clavier — 144 B

    @142 = scancode evdev (51/52 exacts)
    @110 = 6 on press, 0 on release
    No separate modifier state: SHIFT_L is an ordinary key (42).
    Symmetric: press AND release both emit (52/52).

## What our code had wrong

`CLICK_LEFT_TEMPLATE` was byte-exact (0 divergence over 134 constants) but
6 fields were never written. `KBD_TEMPLATE` diverged on **50 bytes
constants** — a message from another era — and we were writing `@114/@116/@128`
with invented values when they are constants, while ignoring `@110`.

## Not validated at runtime

This session's input tests went through a channel **corrupted by F1**
(big-endian frames inside a little-endian length-prefixed stream). F1 is
now OFF; everything is to be replayed. Detail: `KB.md §3.20`.

Tooling built: uinput injection, window location in pointer space,
reading real `/dev/input` events, a guided protocol with a window
d'instructions. Voir `tools/capture_states_run.sh`.
