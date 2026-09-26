---
name: The Shadow input protocol — the decoded formats (revised 2026-05-05)
description: The FlatBuffer wire-format mapping of the Shadow input messages. LEFT click corrected: it needs PointerDown 144 B + MouseDown 152 B + MouseUp 144 B with vtable=20001f00. The old session2 extraction was a false positive.
type: project
---
The common wire format: `<size:4 LE><FlatBuffer payload>`. All on shadow-input, PPID=53.
Common patches: a uint64 LE ts at offset 40, a uint64 LE seq at offset 48.

## MouseMove 144b — vtable `20 00 1f 00`, marker `b130-131 = 01 01`
- Wire size = 0x8c = 140 + 4
- ts @40, seq @48, seq lo @100
- **X uint16 LE @134**, **Y uint16 LE @132**
- The browser duplicates every move (2 back-to-back messages with seq+1)

## PointerDown 144b — vtable `1c 00 1b 00`, marker `b130-131 = 02 01` (b124=0x14)
- ⭐ **It ALWAYS PRECEDES the 152 B MouseDown** in the browser's click sequence
- Without that message, the Shadow VM does not track the "pressed" state → the following MouseUp is ignored → a "stuck" button
- ts @40, seq @48, seq lo @48 (no mirror @100 — a smaller vtable)
- **Y @124, X @126** (different offsets from MouseMove!)

## MouseDown LEFT 152b — vtable `20 00 1f 00`, marker `b130-131 = 02 01` (b124=0x14)
- Identical to MouseUp/MouseMove on the vtable side, told apart by `b124=0x14` (against 0x10 for mouseup)
- ts @40, seq @48, seq lo @100
- **X @134, Y @132** (idem MouseMove)
- Source verified: session1.json idx 194, session12.json idx 67

## MouseUp LEFT 144b — vtable `20 00 1f 00`, marker `b130-131 = 02 01` (b124=0x10)
- Wire size 0x8c
- The same vtable as MouseDown, told apart by `b124=0x10` (against 0x14 for mousedown)
- ts @40, seq @48, seq lo @100
- **X @134, Y @132**
- Source : session1.json idx 195, session12.json idx 68

## MouseDown RIGHT 152b — vtable `20 00 1f 00`, b124=0x14, b130-131 = `02 01`
- Source : session3.json idx 0 (premier right-click)
- seq lo @100, X @134, Y @132 (idem LEFT mousedown)
- **NO PointerDown** (checked 2026-05-05: 0 occurrences of vt=`1c 00 1b 00` across 22 mousedowns in session3)
- The browser duplicates the mousedown (2× the same ts+seq) — redundancy, no effect on Shadow
- LEFT against RIGHT discriminators within the same vtable: bytes 140, 142, 150 (LEFT=`00`/`07`/`00`, RIGHT=`07`/`06`/`01`)

## RIGHT MouseUp, 144 B — IDENTICAL to LEFT MouseUp
- The browser sends the **same** 144 B mouseup_left_template (vt=`20 00 1f 00`, b124=0x10, b130-1=`02 01`) to release both LEFT and RIGHT
- Which button is released is tracked on the Shadow VM side through the preceding mousedown
- ⭐ **For RIGHT, send `click_right_template` on press + `mouseup_left_template` on release** (NOT the same template at both moments — a bug from 2026-05-05)

## Keyboard 144b — vtable `20 00 1f 00`, marker `b130-131 = 06 00`
- Wire size 0x8c, a different marker from the mouse's
- ts @40, seq @48, seq lo @100
- **uint16 LE keycode @142** = scancode **Linux evdev** (`include/uapi/linux/input-event-codes.h`)
- Press and release are told apart by 3 bytes:
  - byte 114 : 0x09 (press) → 0x00 (release)
  - byte 116 : 0x08 (press) → 0x09 (release)
  - byte 128 : 0x01 (press) → 0x00 (release)
- Validated: 'azertyuiop' = codes 0x10..0x19 (KEY_Q..KEY_P), 'qsdfghjklm' = 0x1E..0x27 (KEY_A..KEY_M)

## Keyboard 144b avec marker `b130-131 = 08 00`
- 8 occurrences dans session4 — probablement modifiers (Shift/Caps/AltGr)

## PointerEnter, 136 B — already documented elsewhere
- Voir project_shadow_pointer_enter_unlock.md

## ⚠️ Bug historique 2026-05-05 — faux positif session2.json

The old `click_left_template` (vt=`24 00 23 00`, b124=0x10, b130-131=`00 00`) extracted from session2.json **IS NOT** a mousedown. Probably a drag/scroll/text event. The bug's symptoms:
- A drag-select starts on the Shadow VM (a visible effect)
- MouseUp is never recognised as a release → the button stays "stuck" until we reconnect
- The selection follows the cursor permanently after the first click

The real mousedown has the vtable `20 00 1f 00` (same as mouseup). We must ALWAYS send `PointerDown 144 B` BEFORE the `MouseDown 152 B`.

**Why:** without this document, hours are lost looking for why clicks "do not work" when the template is simply wrong.

**How to apply:**
- For a click: send **PointerDown 144 B** + **MouseDown 152 B** (press) then **MouseUp 144 B** (release). 3 distinct messages, no byte-identical pair.
- Patcher X/Y aux bons offsets selon event type (PointerDown @124/126, MouseDown/Up @132/134)
- For the keyboard: 1 press message + 1 release, key @142, press/release through bytes 114/116/128
- Check the `b16-19` vtable when extracting a new template — `20 00 1f 00` = a classic mouse event, `1c 00 1b 00` = a pointer event, `24 00 23 00` = something else (NOT a click)
