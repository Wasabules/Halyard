---
name: project-B1-GUI-quality-panel-2026-05-18
description: "B1 2026-05-18 — a Borealis GUI 'Stream quality' panel reachable with the Y button in vm_list_activity. 5 SelectorCells (bitrate / fps / resolution / codec / profile) persisted in Settings and passed to ctrl_session_glue_params when the stream starts. Auto = use the desktop defaults (= the value 0)."
metadata:
  node_type: memory
  type: project
---

# The Borealis GUI "Stream quality" panel

## What was delivered

### Nouvelle Activity
- `demo/src/activity/quality_settings_activity.{hpp,cpp}` (= ~150 LOC C++)
- `resources/xml/activity/quality_settings.xml` (= layout AppletFrame + ScrollingFrame avec 5 SelectorCells)

### Settings extended
`demo/include/settings.hpp` + `demo/src/settings.cpp` :
- Nouveau fields : `max_bitrate_mbps`, `target_fps`, `codec`, `profile_id`, `display_width`, `display_height`
- All default to 0 (= leave the Q1 / desktop defaults) except width/height (= 1920×1080)
- Persisted in `/switch/shadow-client/settings.txt` (or the Linux equivalent)

### GUI → stream wiring
`connecting_activity.cpp` lit `Settings::instance()` avant ctrl_session_glue_run :
```cpp
auto& cfg = Settings::instance();
np.display_width   = cfg.display_width  > 0 ? cfg.display_width  : 1920;
np.display_height  = cfg.display_height > 0 ? cfg.display_height : 1080;
np.max_bitrate_mbps = cfg.max_bitrate_mbps;
np.target_fps       = (float)cfg.target_fps;
```

### Bouton GUI
`vm_list_activity.cpp`: a new Y button "Quality" → pushes `QualitySettingsActivity`.

## Options offered in the panel

| Selector | Options | Settings value | Effect |
|---|---|---|---|
| Max bitrate | Auto / 5/10/15/20/30/40/50/80/100/150 Mbps | `max_bitrate_mbps` | CI_BODY_5 f7 max_bitrate + VideoEncodingConfig f2 |
| Framerate | Auto / 30/60/90/120/144 fps | `target_fps` | CI_BODY_5 f3 fps + VideoEncodingConfig f3 |
| Resolution | 720p / 900p / 1080p / 1440p | `display_width/height` | CI_BODY_5 f1/f2 + RegisterSession resolution |
| Codec | Auto / H.264 / H.265 / AV1 | `codec` | CI_BODY_5 inner f2 codec_enum (= hypothetical values, to be confirmed in Q2) |
| Encoder profile | Auto (min latency) / Min latency / Max quality | `profile_id` | CI_BODY_5 inner f4 profile (= a hypothesis, to be confirmed in Q2) |

"Auto" = the value 0 = use the desktop's hardcoded defaults (= do not override).

## Validation

Build Linux clean post-cmake-reconfigure :
```
[100%] Built target shadow-client
```

Regression test post-B1 (= same default settings, no Q1 env vars) :
```
session_seconds = 28
fps = 30.32 (min=20)
decrypt = 100% (min=95%)
bottom NAL = 50.0% (min=30%)
✅ HEALTH CHECK PASSED
```

The stream pipeline is identical to pre-B1 (= empty Settings → np.max_bitrate_mbps=0
→ ctrl_session.c voit 0 → use desktop hardcoded 20 Mbps).

## Limitations connues

1. **The codec/profile enum values are assumed**: we offer H.264=2 / H.265=1 / AV1=3 and
   profile speed=1 / reliability=2. Those values are guesses; Q2 (a desktop
   capture with the settings changed) is needed to confirm them.
2. **The server may ignore them**: without the `dynamic_bitrate` capability being
   advertised (= cf. the project_V8_unexplored RE document), the server may treat our
   client in static mode. But the initial CI_BODY_5 is accepted before streaming starts.
3. **No Save button**: the settings save themselves on every change
   (= through `Settings::instance().save()` in the callback).

## Cross-refs

- [[project-Q1-quality-params-2026-05-18]] — Backend Q1 que B1 utilise
- [[project-A18-automation-infra-2026-05-18]] — auto-test.sh permet de tester Q1+B1 quickly
- [[project-V18-cleanup-baseline-2026-05-18]] — the pre-Q1 baseline

## Status

✅ DONE — Linux + Switch builds (= CMakeLists does a recursive GLOB, no manual add).
On Switch, the Y button in VmListActivity will open the Quality screen.
Sur Linux GUI, idem.
On shadow-test-cli (= no GUI), the env vars SHADOW_BITRATE_MBPS etc. keep
to work (= bypassing Settings, reading the env directly).
