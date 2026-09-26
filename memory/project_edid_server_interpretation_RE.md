---
name: project-edid-server-interpretation-RE
description: TIER6-M3 — the desktop's EDID confirmed byte-exact at C95. Our `SHADOW_EDID_128B` matches capture lines L19454-19467 byte for byte (= a K055G laptop panel, 1920×1080 @ ~144 Hz, 8 bpp DisplayPort, ext=0, csum=0xc1). The desktop binary contains `insert_vendor_hdmi_minimal` but it is not triggered in this capture. Not a taskbar trigger.
metadata:
  type: project
---

# Project — EDID server interpretation RE (TIER6-M3)

## Verdict

**C95** : notre `SHADOW_EDID_128B` (`ctrl_msgs.c:512-521`) match byte-exact
the EDID sent by the desktop (capture L19454-19467).

## Decode EDID critique

| Offset | Value | Meaning |
|--------|-------|---------|
| 18-19 | `01 04` | EDID v1.4 |
| 20 | `a5` | Digital, 8bpp, DisplayPort |
| 21-22 | `22 13` | 34×19 cm = ~16:9 laptop |
| 24 | `02` | Continuous freq=0 (= preferred only) |
| 54-71 (DTD 1) | `ce 8f 80 b6 70 38 88 40 30 20 a5 00 58 c2 10 00 00 1a` | 1920×1080 @ ~144Hz (pixel clock 367.5 MHz) |
| 108-125 (type 0xfe) | `K055G B156HAN` | Panel model |
| **126** | **0x00** | **Extension blocks = 0** (= no CEA-861, no DisplayID) |
| **127** | **0xc1** | **Checksum** (sum 0..127 mod 256 = 0 ✓) |

## Desktop EDID source

Strings dans binary :
- `Found EDID of physical screen {} ({} bytes)` — read from X11/Wayland
- `Using cached EDID with ID {}` + `CacheEDID` — cache mechanism
- `Using empty EDID on wayland` — fallback if Wayland sans EDID
- `EDID declared inconsistent number of extension blocks {}` — validation
- `Cea861 extension blocks:` + `DisplayID v1.3 extension blocks:` — logged if present
- `insert_vendor_hdmi_minimal` — **inject** un CEA-861 HDMI minimal si absent

## `insert_vendor_hdmi_minimal` — not triggered

That function exists but **was not called** in this specific capture
(= the raw EDID is sent directly with no block added). It is possible that
in another interactive run the desktop injects a block — to
no difference in our capture.

## Aucun trigger taskbar

The EDID is identical to the desktop's on that same configuration. No mismatch
de refresh / color depth / HDR / color space.

Refs : `tools/ida/out/TIER6_RE_2026-05-16.md §M3`.
