---
name: project-framerate-negotiation-RE
description: TIER6-M2 — the fps wire negotiation is 100 % byte-exact with the desktop. No SetFrameRate / kFps / FrameRateRequest in the binary. fps lives in (a) RegisterSession.resolution.f3 = 144.0 Hz (the EDID's native rate), (b) CI_BODY_5 video f1.f12.f3 = 143.85, (c) VideoEncodingConfig seq=17 f13.f3 = 142.0. The server replies with 14 supported modes (a 343 B DisplayConfig reply). Not a taskbar trigger.
metadata:
  type: project
---

# Project — Framerate negotiation RE (TIER6-M2)

## Verdict

**C95**: there is no separate ctrl message for negotiating fps. fps is static in
3 endroits wire :

| Sender | Field | Wire byte-exact | Value desktop | Value Switch |
|--------|-------|-----------------|---------------|--------------|
| Client→Server (DisplayConfig seq=3) | RegisterSession.resolution.f3 | `1d 26 07 10 43` | 144.0277 (= EDID DTD) | byte-exact |
| Client→Server (CI_BODY_5 seq=5) | inner f12.f3 | `1d e1 da 0f 43` | 143.85 | 143.85 (env override SHADOW_FPS) |
| Server→Client (the Channel-5 reply, seq=5) | f2.f3 | `1d a0 da 0f 43` | a 143.85 echo | parsed, `[FPS1]` log, ctrl_session.c:1716 |
| Client→Server (VideoEncodingConfig seq=17) | f13.f3 | `1d 00 00 0e 43` | 142.0 (F16) | 142.0 (env override) |

## The 343 B DisplayConfig reply — 14 supported modes

Server advertise 18 modes total (= 1280×720, 1280×768, 1280×800, 1280×960,
1280×1024, 1360×768, 1366×768, 1600×900, 1600×1024, 1600×1200, 1680×1050,
1920×992, 1920×1080, 1920×1488, etc.) all capped at **144.0 Hz**.

Our `[FPS1]` log already extracts and logs those modes (= `ctrl_session.c:1688-1722`).

## No SetFrameRate / kFps / target_fps

Grepping the desktop binary: 0 matches for those strings. fps has **no message
dynamic negotiation** — it is frozen in the 3 places above.

## EDID encode-t-il fps ?

The Detailed Timing Descriptor at offset 54:
- Pixel clock = `0x8FCE × 10kHz = 367.50 MHz`
- Pour 1920×1080 + blanking CVT (= Htotal=2200, Vtotal=1125) → ~148.5 Hz native

⇒ **EDID encode bien ~144 Hz natif**. Match server response.

## Aucun trigger taskbar

Our announced fps is byte-exact with the desktop. No hardcoded 30 fps anywhere
part. Refs : `tools/ida/out/TIER6_RE_2026-05-16.md §M2`.
