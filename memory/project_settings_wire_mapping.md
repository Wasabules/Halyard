---
name: project-settings-wire-mapping
description: "DECISIVE 2026-08-21 — Mapping the client menu's settings to the wire. Bitrate = kUpdateSession f13 applied live (f1{f2=794816, f4=bps}); codec = an unregister + a re-announcement; 4:4:4 colour = kRestart f5 (empty) + a full restart."
metadata:
  type: project
---

The guided capture `captures_settings_20260821_180409`
(`tools/capture_scroll_resize_run.sh settings` + `tools/analyze_settings.py`).

**Three distinct mechanisms** depending on which setting changes:

| setting | message | effect |
|---|---|---|
| bitrate | `kUpdateSession` (f13) | applied **live** |
| codec H.264 → HEVC | `kUnregisterSession` + a new channel announcement | the video channel is renegotiated |
| codec back / Color 4:4:4 | `kRestart` (f5, **empty**) | a **full restart** (re-auth) |

Structure du bitrate : `f13 { f1 { f2 = 794816, f4 = bitrate_bps } }`.
`f2` constant; the bitrate in **f4**. Measured: low → 1000000, unlimited → 70000000.

**Our message was malformed**: the bitrate written in f2, plus an f3 float fps
non-existent. So the server never saw a value in f4 — our bitrate requests
bitrate never got through. That **invalidates the "server ceiling" measurement
~14 Mbps" measurement from [[project-quality-params-validated]]. Corrected (S18).

It also corrects K15, which had switched f13 off believing the desktop does not emit
never: that was only true in captures where no setting was changed.

**The account's real configuration**: 2560×1440 @ 60 fps @ 55 Mbps (70 when unlimited),
where our `CI_BODY_5` is frozen at 1920×1080 @ 143.9 fps @ 20 Mbps — values
from an old capture on another machine.

`KB.md` §3.24.
