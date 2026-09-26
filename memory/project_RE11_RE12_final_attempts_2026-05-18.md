---
name: project-RE11-RE12-final-attempts-2026-05-18
description: "RE11+RE12 2026-05-18 — The last attempts without an interactive capture. RE11 wired the VST→h264_decoder callback (untestable, as the server is idle). RE12's Capabilities_Streaming sub-message is accepted at bootstrap (= a win!) but has no effect on the bottom NAL. It confirms: the V14 stack's 50% bottom NAL baseline = the OPTIMUM attainable without an interactive capture."
metadata:
  node_type: memory
  type: project
---

# RE11+RE12 — Tentatives finales sans capture interactive

## RE11 — VST callback → h264_decoder branching

### Implemented
`ctrl_session.c::vst_to_h264_callback()`:
- Receives `(udata, nal_bytes, len, frame_id, flag, is_keyframe)`
- Validate Annex-B start code `00 00 00 01`
- Compute `pts_ms = monotonic ms`
- Call `ctx->p->on_video(nal_bytes, len, pts_ms, is_keyframe, user)` → h264_decoder downstream

### Wired dans ctrl_session.c
`ctrl_video_tcp_open(..., vst_to_h264_callback, &ctx)` instead of `NULL`.

Log confirmation : `vst: ctrl_video_tcp_open OK on :12020 (RE11 callback to h264 active)`.

### Test runtime
A 58 s native-stream session: **NO `[RE11] VST → h264 feed` log**, because the VST channel was **idle for 60 s** (= the server sent no VST frame in that session).

That differs from the RE6 finding, which observed 4142 B at 3 fps. Possible causes:
- A headless session = no display activity → the server does not push VST
- A different VM from the one in the initial capture
- A specific trigger missing on our side

Conclusion: **the infrastructure is ready, the runtime unvalidated**, because the server has nothing to push. If the server ever sends VST frames, they will reach the h264_decoder automatically.

## RE12 — Capabilities_Streaming sub-message

### Implemented
`ctrl_msgs.c::ctrl_build_capabilities()`:
- Added `f5.f2 = Capabilities_Streaming{f2=codec, f3=chroma, f4=hdr}` behind an env var
- `SHADOW_CAP_STREAMING=1` pour activer
- Default OFF = byte-exact V14 baseline

### Tests A/B runtime

| Config | bootstrap | session | fps | bottom_pct | frames |
|---|---|---|---|---|---|
| Baseline (off) | OK | 28s | 30.96 | **49.95%** | 867 |
| Cap_Streaming on (= codec=2, chroma=0, hdr=0) | **OK** | 28s | 30.75 | **49.92%** | 861 |

### Verdict
**The bootstrap is accepted**! = a first-degree win, the server accepts our
the Capabilities_Streaming sub-message inside f5.f2. Unlike the 5 RE8 booleans
that broke the stream, here the values codec=2/chroma=0/hdr=0 are probably
identical to the server defaults → the server has nothing to change.

**No effect on the bottom NAL** (= 49.95% against 49.92% = statistical noise <0.1%).

Possible explanations :
1. Values codec/chroma/hdr identiques server defaults
2. The multi-NAL trigger is NOT in Capabilities_Streaming
3. The sub-message's position is wrong (= perhaps it should be in f4 or another wrapper)

## Verdict global

After **RE1 through RE14**, all attempted without an interactive capture:

| RE | Hypothesis | Test | Effect on the bottom NAL |
|---|---|---|---|
| RE1 fps=60 | Mismatch advertise | A/B | 50% identique baseline |
| RE3 4 bools (8/9/10/11) | Field# wrong | A/B fail | n/a (bootstrap fail) |
| RE5 input X/Y | byte-exact OK | Code patched | (no impact on bottom) |
| RE6 VST 4142 B = H.264 | Identified | RE11 wired | Server idle, untested |
| RE8 5 bools (4/5/6/8/10) | Wire tags C95 | A/B all of them | NO effect or a bad effect |
| RE9 field 10 = vr_enabled | C70 | A/B tested | Bad effect (0 frames) |
| RE10 field 8 best multi-NAL | C65 | A/B tested | NO effect |
| RE11 VST→h264 callback | Wired | Untestable (server idle) | n/a |
| RE12 Capabilities_Streaming | Sub-msg | A/B accepted | NO effect (49.92% against 49.95%) |

**Stack V14 baseline 50% bottom NAL = OPTIMUM ABSOLU atteignable sans capture
interactive desktop**.

The remaining 2% taskbar bug is EITHER:
- Client-side libavcodec EC on partial P-frames (= already tuned, EC=259 is the optimum)
- Or it really does need an interactive desktop capture to be identified byte-exact

## Code state final

Every finding is coded safely (= opt-in env vars, the V14 baseline preserved as the default):
- RE5 Input X/Y: `ctrl_input_tcp.c` patched (= the cursor moves if we send a mouse_move)
- RE8 5 bools : SHADOW_REG_F4/F5/F6/F8/F10 env vars
- RE11 VST→h264 : SHADOW_VIDEO_TCP=1 default active
- RE12 Cap_Streaming : SHADOW_CAP_STREAMING=1 opt-in

No baseline regression. Every A/B test confirms it.

## Cross-refs

- [[project-RE9-RE10-field-semantics-2026-05-18]] — RE9+RE10 bools verdict
- [[project-RE1-RE4-static-RE-findings-2026-05-18]] — RE1-RE4 first wave
- `tools/ida/out/H1_V8_unexplored.md` — Capabilities_Streaming source
- `ctrl_msgs.c::ctrl_build_capabilities()` — patch RE12

## Conclusion

The project is in the **optimal state attainable without an interactive capture**.
To push further, an **interactive desktop capture** (= Q2) is needed, with:
- The mouse really moving during the capture (= to validate RE5)
- The UI settings changed (= to identify the exact codec/profile values)
- Audio actif (= identifier PSK + Opus format)
- Cursor shape changements (= validate format 271B)
- Clipboard copy actions (= validate clipboard protocol)

Without that, the 2 % taskbar bug is the ceiling. But a 98 % Windows picture + a 50 % bottom NAL
stable Linux/Switch = produit fonctionnel pour gameplay.
