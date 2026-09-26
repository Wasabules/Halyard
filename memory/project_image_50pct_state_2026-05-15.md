---
name: project-image-50pct-state-2026-05-15
description: The picture at 50 % (top) — a complete RE of the SUFP pipeline confirms a server-side bug. A 1 % bottom-slice ratio for us against the desktop's 11 %. The V5 patches are fine, the decoder does receive the bottom NALs but too few for a complete render.
metadata:
  node_type: memory
  type: project
---

# État image streaming 2026-05-15 (= V8)

## Visual summary
- Before the patches: a frozen 30% top + vertical bands at the bottom (= error_concealment)
- After V5 (= the off-by-one fix + a slot_seen bitmap for every chunk + a clean F.1 parity drop): 30 fps decoded, the picture renders ~50% top
- The bottom of the picture = vertical pastel stripes (= error_concealment) OR frozen on the last cached IDR_bottom (= with SHADOW_YUV_COMPOSITE=1)

## Runtime stats confirmed (V8, a t=70 s session)
```
video=29143 pkts (34M B) decoded=2077 ok=2111/2111 parity=22793 aband=0 last=2081 incompl=0 skip=0
NAL top=2084 bot=17 idr_t=3 idr_b=1 sps=3 pps=3
```
- 30 fps decoded ✓
- 100% decrypt success
- 0 frames abandoned
- **Bottom NAL ratio = 17/(2084+17) = 0.81%**
- Desktop ref (project-image-100pct-proof) = **11%**
- **We get ~10× fewer bottom slices than the desktop**

## Diagnostic

### Confirmed byte-exact
1. **SUFP wire** : chunks 0-indexed, count semantics, parity = `byte10 == 0x00`. V4 (highest_idx) interpretation = WRONG, V5 (count) = CORRECT.
2. **byte0's upper nibble** = a marker, `0x1=IDR_keyframe`, `0x2=P_frame`. NOT a SoF flag, as agent V2 believed.
3. **Reed-Solomon is ABSENT from the binary** (= an exhaustive grep, confirmed 4 times independently).
4. **The decoder feed is functionally equivalent** (= libavcodec through avcodec_send_packet; the desktop goes straight to VA-API, but that is just a different path, not a bug).
5. **CI_BODY_5..12 = baked depuis capture LD_PRELOAD 2026-05-08**. CI_BODY_5 (= Video) decoded : f1.f4 (codec hints), f2 (w=1920, h=1080, fps fixed32=143.85), f7 varint (max_bitrate=163742208 ≈ 163Mbps).
6. **The 343 B M13 reply** = a LIST of 18 resolutions the server supports (1920×1080@143.85 native + 17 others). NOT a chosen encoder profile. Server version `"6.1.7" ShadowStreamer`.

### Refuted empirically
- **H2 (CI_BODY_5 over-specified)**: the server confirms a native 143.85 fps in its reply → not over-specified.
- **H5 (EDID byte 126 off-by-one)**: the fix applied (byte 126=0, byte 127=0xc1, a valid checksum) → NO change in the bottom ratio (= 0.81 % against 1.23 % before the fix, within the variance).
- **Plan A (frequencies)** : aligned State 2Hz, Hid 16s, Flush 17s → AUCUN changement.
- **IDR_INJECT / YUV_COMPOSITE**: decoder-side mechanisms enabled → NO notable visual change.
- **VideoSslTcpChannel `:11020`**: confirmed to be an STFP IDR-retransmit fallback, NOT a bottom-slice carrier.

### The strong hypothesis that remains
**The server discriminates our client against the official desktop** on the basis of something in our bootstrap messages. With 10× fewer bottom slices than the desktop, this is not bitrate tuning — it is a binary server-side decision.

Remaining candidates to test:
1. **Incomplete f5 capability flags** (= H3 V8): we only advertise `OCapture`. The binary's strings show the desktop advertising 7+ capabilities (`dynamic_bitrate, gamepads, display_management, audio_out_codec, multiscreen, streaming_profile, network_notifications`).
2. **output_id = 0**: we send 0 everywhere, and perhaps it should be 1 or something else.
3. **No audio/cursor/input feedback sent**: the server may read that as "a passive client" → a degraded mode.

## Patches that work (= do not regress them)
- `streaming/ctrl_session.c::on_video_packet` — the V5 refactor: a slot_seen bitmap for all chunks (data+parity), decrypt+store for data only, a G.1 fast path on chunk_idx==max-1
- `streaming/ctrl_session.c::vid_reasm_t.expect_sof` — drop chunks orphelins post-abandon (G.3)
- `streaming/ctrl_session.c` — counters parity_skip, reasm_abandoned, got_last_chunk, incomplete_at_flush, seq_window_drop, nal_top/bot/idr_t/idr_b/sps/pps
- `streaming/ctrl_session.c` — `chunk_idx >= max_chunks` strict (= count semantics, 0-indexed)
- `streaming/ctrl_msgs.c::SHADOW_EDID_128B` — byte 126 patched from 0xc1 → 0x00, byte 127 added = 0xc1 (cosmetic — it does not help, but the EDID is now standards-conformant)

Refs : [[project-byte0-upper-nibble]] [[project-image-100pct-proof]] [[project-ctrl-oneof-enum-verified]]

See the detailed analyses: `tools/ida/out/H1_SUFP_analysis.md`, `H1_SUFP_deep_v2.md`, `H1_FRAME_COMPLETE_analysis.md`, `H1_PARITY_DEEP_analysis.md`, `H1_MISSING_PIECES.md`, `H1_V8_unexplored.md`.
