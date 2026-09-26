---
name: project-registersession-video-bools-re
description: H2 deep — RegisterSession_Video 5 bools byte-exact named via RegisterStreaming log format; field 8 dead, multi-NAL trigger probablement high_color_fidelity
metadata:
  type: project
---

# H2 deep — RegisterSession_Video bool fields BYTE-EXACT

**Session** : 2026-05-16 IDA Pro 9.3 + Hex-Rays. Doc :
`tools/ida/out/H2_H3_RE_2026-05-16.md` §H2-deep.

## TL;DR — table byte-exact

| Tag | Field# | Offset | Nom (C95) | Desktop value | Notre val | Recommandation |
|-----|--------|--------|-----------|---------------|-----------|----------------|
| 8   | 1  | +24 | `codec_config` sub-msg | nested {f4 {f2=codec, f4=profile}} | nested | OK |
| 16  | 2  | +32 | `resolution` sub-msg   | {w, h, fps_fixed32} | OK | OK |
| 24  | 3  | +40 | **`video_codec_enum`** u32 | 2 (H.264) | absent | `emit 2` |
| 32  | 4  | +44 | **`cursor_merged`** bool | TRUE | skip | **`emit 1`** |
| 40  | 5  | +45 | **`high_color_fidelity`** bool | TRUE | skip | **`emit 1`** |
| 48  | 6  | +46 | **`hdr`** bool | FALSE | skip | OK |
| 56  | 7  | +48 | **`max_bitrate_bps`** u32 | 163742208 | 163742208 | OK |
| 64  | 8  | +47 | **DEAD** (never set) | absent | skip | **leave skipped** |
| 80  | 10 | +52 | **`vr`** bool | FALSE | skip | OK |
| 90  | 11 | +16 | `vrSystem` bytes | absent | skip | OK |

Field 9 absent (= proto schema sans field 9).

## Method — the source of truth

The log format string in `sub_853770 = CtrlChanV2Manager::RegisterStreaming`
(L496-497) :

> `"Requesting {}-{} video streaming session: {}x{}@{}fps, max bitrate: {}Mbps,
>  codec: {}, cursor merged: {}, high color fidelity: {}, hdr: {}, vr: {}"`

And the code just before it (L434-454):
```c
*(_BYTE *)(v30 + 44) = *(_BYTE *)(a2 + 44);   // cursor_merged
*(_BYTE *)(v30 + 45) = *(_BYTE *)(a2 + 45);   // high_color_fidelity
*(_BYTE *)(v30 + 46) = *(_BYTE *)(a2 + 46);   // hdr
*(_BYTE *)(v30 + 52) = *(_BYTE *)(a2 + 56);   // vr (a2+56 → v30+52)
v195 = (double)*(int *)(a2 + 48) / 1000000.0; // max_bitrate / 1e6 = Mbps
```

**No write to `v30+47`** → field 8 is never set by RegisterStreaming.

## Field 8 — pourquoi dead

- Scan exhaustif sur 24,280 fonctions : seul `sub_10DCE80` touche `+47`,
  and it is a **proto descriptor string builder** (= it emits a 55-byte string
  "...erStream"), not a setter on the video struct.
- The log's ordering skips from `hdr` straight to `vr` → no name for field 8.
- Hypothesis: a deprecated field in this proto version. **Do not emit it.**

## Multi-NAL trigger — narrowed C70

Among the 4 real booleans (f4/f5/f6/f10), the most likely is:
1. **`high_color_fidelity` (f5)** — gating direct sur encoder chroma/precision
   mode. C70.
2. **Combo `cursor_merged + high_color_fidelity`** — l'app desktop set les
   2 ensemble par default. C70.
3. `hdr` (f6) = FALSE en desktop, peu probable.
4. `vr` (f10) = FALSE en desktop, contre-productif (= HMD-specific).

## The implementation already in place

`ctrl_msgs.c::build_video_chan_body` (L573+) already emits the 5 bools through env
`SHADOW_RE8_F{4,5,6,8,10}`. **No code change required** — just flip
les defaults :
```c
vparams.re8_f4  = 1;   // cursor_merged       (desktop default ON)
vparams.re8_f5  = 1;   // high_color_fidelity (desktop default ON)
vparams.re8_f6  = 0;   // hdr                 (desktop OFF)
vparams.re8_f8  = 0;   // DEAD, DO NOT emit
vparams.re8_f10 = 0;   // vr                  (desktop OFF)
```

The caller (= `ctrl_session_glue.c` probably) must set the params before
d'appeler `ctrl_build_channel_announcement_ex(..., chan_idx=0, &vparams)`.

## ⚠️ A critical cross-check — the definitive RE9-RE10 verdict

[[project-RE9-RE10-field-semantics-2026-05-18]] has already A/B tested live
**chacun des 5 bools** :
- f4 (cursor_merged) → NO effect (49.95% bottom = baseline)
- f5+f6 (HCF+hdr combo) → bad effect (0 frames, server change mode)
- f8 (dead) → NO effect
- f10 (vr) → bad effect (0 frames)

**Empirical conclusion**: flipping those bools does NOT enable multi-NAL on
the server. The H2 deep RE gives the C95 NAMES (= useful for documentation/clarity) but does not
hand over a new exploitable bool.

The remaining 2 % taskbar bug is NOT gateable through those booleans. Leads
restantes :
- Capabilities-strings advertisement (= 7 strings de sub_697050)
- Plaintext capture CI_BODY_5 desktop byte-exact
- Client-side libavcodec EC bug sur P-frames partial bottom

## Refs

- `tools/ida/out/H2_H3_RE_2026-05-16.md` §H2-deep
- `tools/ida/out/h2h3/ctx_sub_853770.c` (= RegisterStreaming filler decompile)
- `tools/ida/out/h2h3/ctx_sub_10E60A0.c` (= RegisterSession_Video serializer)
- `tools/ida/out/h2h3/H2_SETTERS.md` (= the setter-scan result)

## Cross-links

- [[project-multinal-chunks-RE]] (= TIER1 §B narrowed 5 candidats)
- [[project-setstreamingprofile-re]] (= H3, closed, a complement)
- [[project-ctrl-oneof-enum-verified]] (= field 8 RegisterSession dans Request)
- `05-shadow-client-borealis/demo/src/streaming/ctrl_msgs.c::build_video_chan_body`
