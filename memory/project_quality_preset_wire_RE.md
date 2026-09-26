---
name: project-quality-preset-wire-RE
description: TIER9-W4 — the streaming-profile CLI flag has 2 values (speed/reliability) and a DynamicQualityLegacy class exists, but the value is published ONLY in the PUBLIC /1/stats telemetry. No protobuf field on the ctrl wire. A C90 refutation; it cross-confirms H3.
metadata:
  type: project
---

# TIER 9 W4 — Quality settings UI ↔ wire mapping RE

## Verdict C90 — preset client-side mapping, jamais wire

L'UI/CLI Shadow desktop expose **2 valeurs** de "streaming profile" : `speed`
(default) and `reliability`. A whole `DynamicQualityLegacy` class handles
them. **BUT** those values are mapped **client-side** onto parameters
concrete ones (bitrate, codec, chroma) then sent through the fields **ALREADY
**EXISTING** ones in `RegisterSession_Video`. The literal word `speed` or
`reliability` NEVER appears on a ctrl-msg.

## Strings binary — `streaming-profile` 2 values byte-exact

| Line | String |
|------|--------|
| 27817 | `streaming-profile` (= CLI flag name) |
| 27945 | **`streaming profile to use. Values: speed (default), reliability`** (= help text byte-exact) |
| 28233 | `streaming_profile` (= event suffix) |
| 29138 | `Set streaming profile {} (API call)` (= log handler) |
| 29139 | `Set streaming profile failed: {}` (= error) |
| 29423 | `streaming_profile_startup` (= /1/stats event name bootstrap) |
| 29425 | `streaming_profile` (= /1/stats key name) |
| 29682 | `Fallback to default streaming profile` (= log) |
| 29796 | `GetDynamicQualityMetrics` (= API method) |
| 29953-29955 | `streaming_profile_changed` + `streaming_profile` (= event names runtime change) |
| 33414 | `udu/deps/framework/src/common/video/src/dynamic_quality_legacy.cpp` (= source) |
| 33410 | `14DynamicQuality` (= RTTI typeid) |
| 33416 | `20DynamicQualityLegacy` (= RTTI typeid) |
| 30420 | `N20DynamicQualityLegacy9InterfaceE` (= nested Interface) |

Event classes dans `ShadowRenderer::Run` variant :
- `ShadowAppSetStreamingProfileEvent`
- `ShadowClientStreamingProfileChangedEvent`
- `ShadowAppSetBitrateEvent` / `ShadowAppSetBitrateRatioEvent`
- `ShadowAppSetCodecEvent` / `ShadowAppSetVideoChromaEvent`
- `ShadowAppSetAudioOutCodecEvent`
- `ShadowAppSetAutomaticFramerateEvent`

## Capture analysis — `streaming_profile` = telemetry only

`grep -n "streaming_profile\|streaming-profile" tls_plain.log` → matches
UNIQUEMENT dans `/1/stats` body :

**L18829-18832** :
```json
{"event":"streaming_profile_startup","streaming_profile":"speed","type":"renderer"}
```

It is the `/1/stats` event that the launcher pushes **after bootstrap** to
publier la valeur effective. **Privacy = PUBLIC, telemetry ELK only**.

**No ctrl-msg** on `:base+11` contains the word `streaming_profile`,
`speed` or `reliability`. Cross-checked by grepping the ctrl SSL_WRITE peer
plaintext = 0 match.

## Cross-check H3 RE (= 2026-05-16) — toujours valable

The H3 RE concluded at C90: `SetStreamingProfile` does not exist as a literal in the
binary, and every neighbouring xref resolves to 6 CLIENT-side functions
local ones. No SSL_write, no proto serialiser.

**TIER 9 W4 confirme** : la classe `ShadowAppSetStreamingProfileEvent`
DOES exist in the `<ShadowRenderer::Run>` variant (= an event-bus dispatch
an internal client), **but its handler does no ctrl-msg SSL_write at all**. The
seul side-effect wire-side = push une `/1/stats` notification (=
"streaming_profile_changed" event).

H3 reste C90 valable.

## DynamicQualityLegacy class — purpose client-side

`DynamicQualityLegacy` + `GetDynamicQualityMetrics` suggest a
**client-side algorithm** that:
- Measures the runtime metrics (= packet loss / RTT / decoder FPS)
- Compute un "quality score"
- Ajuste **localement** le rendering (= drop frames, change codec, etc.)

The `GetDynamicQualityMetrics` API is probably consumed by CEF (=
Electron renderer) to display the quality indicator in the UI. **Not a signal
wire-side**. `SetDynamicQualityMetrics` n'existe pas (= grep 0 match).

## Routing speed/reliability → params concrets

The streaming-profile=speed|reliability is **NOT** a protobuf wire field.
It only influences (= a client-side mapping in
`DynamicQualityLegacy::Interface::compute_params(profile_enum)`) :
- Quelle valeur de `f7 max_bitrate_bps` envoyer
- Quel codec choisir (= f3 `video_codec_enum`)
- Quel chroma (= 4:2:0 vs 4:4:4)
- Probablement `cursor_merged` / `high_color_fidelity` toggles

All those params are **already** in `RegisterSession_Video` (= 5 bools + 2 u32
+ codec_enum), mapped byte-exact through H2-deep
(`project_registersession_video_bools_RE.md` C95).

## Not a new trigger for the Switch client

Our Switch client already has every adjustable parameter (= the B1 GUI Quality
panel + Q1 env vars `SHADOW_BITRATE_MBPS`, `SHADOW_FPS`, `SHADOW_CODEC`,
`SHADOW_PROFILE_ID`). Cf `project_Q1_quality_params_2026-05-18.md` et
`project_B1_GUI_quality_panel_2026-05-18.md`.

**Not a new trigger to implement** for TIER 9. If we want to expose the
speed/reliability preset in the Switch GUI, we can map it client-side onto
(bitrate=50/codec=H264/chroma=420) for speed and
(bitrate=15/codec=H265/chroma=420) for reliability. But the server
is not told the preset's name itself.

## Verdict W4

**C90**: the "the preset toggle sends multi_nal_enabled=1 or some other special
flag" hypothesis is REFUTED. The ctrl wire carries NO "preset name" field. Cross-
confirme H3 RE 2026-05-16.

## Refs

- Strings streaming-profile : `strings_display.txt:27817, 27945, 28233, 29138-29139, 29423-29425, 29682, 29795-29796, 29953-29955`
- Strings DynamicQualityLegacy : `strings_display.txt:30420, 33410-33416`
- Source path : `udu/deps/framework/src/common/video/src/dynamic_quality_legacy.cpp`
- Capture `streaming_profile_startup` event : `tls_plain.log:L18829-L18832`
- TIER 9 doc : `tools/ida/out/TIER9_RE_2026-05-16.md` §W4
- Cross-ref H3 RE : `tools/ida/out/H2_H3_RE_2026-05-16.md` §H3
- Cross-ref H2 deep : `memory/project_registersession_video_bools_RE.md` (C95)
- Cross-ref Q1 quality params : `memory/project_Q1_quality_params_2026-05-18.md`
- Cross-ref B1 GUI panel : `memory/project_B1_GUI_quality_panel_2026-05-18.md`
