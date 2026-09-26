---
name: feedback-yuv-composite-idr-inject-dont-fix-bottom
description: "OBSOLETE 2026-05-18 (the code was removed in the V18 cleanup — the toggles are gone). History: SHADOW_YUV_COMPOSITE and SHADOW_IDR_INJECT (decoder-side caching) did NOT fix the 50% image bug — the root cause was server-side. Resolved by V11 PARITY_RAW + V12 IFR counter + V14 gE bytes_rx (= a 98% picture)."
metadata:
  node_type: memory
  type: feedback
---

# [OBSOLETE, V18] YUV_COMPOSITE + IDR_INJECT do not fix the 50 % image bug

> **2026-05-18, the V18 cleanup**: this finding became obsolete after V11 PARITY_RAW.
> Code removed (= the SHADOW_YUV_COMPOSITE, SHADOW_IDR_INJECT and SHADOW_BOTTOM_INJECT toggles deleted).
> The real fix was: (1) the "parity" chunks are plaintext NAL (V11), (2) the IFR counter
> incrementing (V12), (3) the real gE bytes_rx (V14). The picture reaches 98 %, and all that remains is
> the taskbar bug — see [[project-V12-taskbar-status-2026-05-15]].
> Keep this file for the RE's historical traceability (= 50 %→98 % unblocked at the end of May).

## Mechanisms tested
- **SHADOW_YUV_COMPOSITE=1** (= `ctrl_session_glue.c::on_frame::composite_enabled`): caches the lower half of the decoded frame when the AU contains a bottom NAL (`g_au_has_bottom_real == true`), and overlays it on subsequent frames. NV12-safe after the V8 fix.
- **SHADOW_IDR_INJECT=1** (= `ctrl_session.c::emit_legacy`): prepend a cached `SPS + PPS + IDR_top` in front of an isolated IDR_bottom → building a complete AU for the decoder.

## Result
**No visual improvement at all**. The picture stays at 50 % top + 50 % bottom, either in vertical bands (= libavcodec's error_concealment) or as static cached content (= it does not refresh).

## Why : root cause est server-side
- Our pipeline receives ~17 bottom NAL slices in 70 s (= 0.24/s).
- The official desktop receives ~84 bottom slices in 240 s (= 0.35/s) — not so different in RATE.
- **BUT**: the desktop's bottom/(top+bottom) ratio = 11 %. Ours = 0.81 %.
- The differential: the server pushes 10× fewer bottom slices to us than to the desktop.

## Why decoder-side mechanisms are not enough
- At 0.24 bottom/sec, we get one bottom slice every 4 s. If we cache that, the lower picture is "frozen" for 4 s between refreshes = not a real stream.
- IDR_INJECT only helps when we have an IDR_bottom (= 1 in 70 s, negligible).
- The problem is not the decoder dropping our bottom slices — it is that we do not have enough bottom slices to emit.

## What: not fixable on our side without changing what the server pushes
The real lead = understand how the desktop manages to receive 11 % bottom slices:
1. **Capabilities f5 sent differently**?
2. **Output_id non-zero** ?
3. **Audio/cursor channel feedback** que server attend ?
4. **A specific ctrl-msg** we do not send?

## Conclusion
Keep the mechanisms (= configurable through env vars) but default them OFF. The picture stays capped at 50 % top as long as we do not change the **server-side behaviour**.

Refs : [[project-image-50pct-state-2026-05-15]] [[project-image-100pct-proof]]

Cf. `tools/ida/out/H1_V8_unexplored.md` for the remaining server-side leads to explore.
