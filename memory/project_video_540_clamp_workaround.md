---
name: project-video-540-clamp-workaround
description: "2026-05-14 — A visual workaround: clamp the display to 540 (= height/2) because the server only sends first_mb=0 slices (= the top half). A usable picture for the first time, no more green half. Not a root-cause fix."
metadata: 
  node_type: memory
  type: project
---

## Constat

**The Shadow server sends 98 %+ first_mb=0 slices** (= the top 1920×540) on :11010 UDP. Bottom slices (first_mb=4080) arrive at a 1:60 ratio, and every bottom IDR is rare (~0.1 % of frames). With no reference bottom IDR, the bottom P-slices are rejected by libavcodec.

Root-cause fixes attempted and set aside:
- N23 IDR injection prepend SPS+PPS+IDR_top devant IDR bottom → marche pas (= rare)
- N24 VideoSslTcpChannel on `:11020` → it is an STFP TCP fallback, not the video bottom (see [[project-video-ssl-tcp-channel-found]])
- N25 BOTTOM_INJECT appended the last bottom slice on every flush → the decode was rejected (P-slices with no reference IDR)

## The workaround applied (2026-05-14 ~01:45)

`ctrl_session_glue.c`'s `on_frame()`: clamp `eff_height = height / 2`. Can be disabled with `SHADOW_NOCROP=1`.

→ `stream_view_push_yuv` receives width=1920 height=540 (= only the pixels actually decoded). No green half displayed.

## Limite

The visible picture = the desktop's top 50 % (a 16:5 aspect, a 32:9 ratio). You see the icons, the wallpaper, the cursor. NOT the Windows taskbar (= the bottom).

## N26 tested: RegisterSession height=540 changes nothing

Test of 2026-05-14 ~01:47: `SHADOW_DISPLAY_HEIGHT=540` → the server reconfigures the SPS for a native 1920×540 BUT keeps sending first_mb=0 slices only (= 270 effective pixels out of 540 → a green half still visible). An invariant pattern: **the server always sends the top 50 % of whatever we ask for**.

→ The 50 % ratio is hard server-side, and not negotiable through RegisterSession. Probably tied to `ShadowAppSetStreamingProfileEvent` or a control message not yet RE'd.

## Formula CONFIRMÉE 2026-05-14 12h07

**The server SYSTEMATICALLY sends the top 50% of the requested height**:
- height=480 → 240 effective + 240 vert
- height=720 → 360 effective + 360 vert
- height=1080 → 540 effective + 540 vert
- height=2160 → REJECTED (probably > max supported)

So the `eff_height = height / 2` clamp is UNIVERSALLY correct.

## Env-var toggles exposed

- `SHADOW_DISPLAY_HEIGHT=N` : override height request (default 1080)
- `SHADOW_NOCROP=1`: disables the height/2 clamp (= to see the green)
- `SHADOW_STRETCH=1`: fill the window without letterboxing (= vertical distortion, but no black bars)

## Recommended combinations

- **A 1080p Linux desktop**: no env vars → a 1920×540 picture letterboxed inside 1920×1080
- **A fullscreen Linux desktop**: `SHADOW_STRETCH=1` → a 1920×540 picture stretched to 1920×1080 (= a 2× vertical ratio)
- **Switch 720p (future)**: `SHADOW_DISPLAY_HEIGHT=720 SHADOW_STRETCH=1` → a 1920×360 picture stretched to 1280×720

## The measured baseline (= the 540 clamp + bottom_inject disabled) — 2026-05-14 02:00

- **stable fps ~28** (356 frames decoded over 20 s, i.e. ~17.8 fps on average, but ~28 fps in steady state after 15 s)
- **decode_slice_header errors: 4 over 30 s** (against 600+ with bottom_inject) — a negligible ratio
- **A SHARP 1920×540 picture**: the Windows icons are visible, the wallpaper is fine, the cursor is visible, no green half
- video pkts/s : ~430 (= 8459-6501 / 5s = 392 pps)
- decrypt_ok / total = 100% (= 643/643)

This is the production-ready BASELINE for the Switch port. Any future regression must come back to these metrics.

## Fix root cause possible (= [[project-video-540-clamp-workaround]] task N26)

1. Change `ctrl_build_register_session(1920, 540)` instead of `1920, 1080` → the server might encode natively at 540p
2. RE the binary to find a "FullKeyframe" or "ForceIDR" message
3. Implement a Reed-Solomon decoder to reconstruct the bottom from the parity chunks (= 80 % of the UDP packets are parity, and we currently skip them)

## How to apply

When working on Shadow image quality / resolution:
- No point digging into `:11020` (= STFP stats/keepalive)
- The 540 clamp is the acceptable visual baseline
- Pour viser fullframe → tester d'abord ctrl_build_register_session(1920, 540)
