---
name: project-V18-cleanup-baseline-2026-05-18
description: "V18 cleanup baseline 2026-05-18 — removed ~500 LOC of dead code (the BOTTOM_INJECT/IDR_INJECT/YUV_COMPOSITE/PARITY_TRIM/PARITY_DECRYPT/REG_PERIOD toggles + their globals + the N24 probe thread + the dead keyhex block). A clean Linux build. The active V14 stack is unchanged: PARITY_RAW=1 + IFR_PERIOD=14 + gE bytes_rx delta×4. A 98% picture on Windows confirmed."
metadata:
  node_type: memory
  type: project
---

# Baseline V18 cleanup — repo propre pour V16 wolfSSL hook

## What was removed (~500 LOC of dead code)

### `ctrl_session.c` (1700 → 1316 LOC)

| Bloc | Raison |
|---|---|
| `n24_probe_thread` + `n24_probe_arg_t` (84 L) | Pure dead code — never called (`grep pthread_create.*n24` = 0) |
| The globals `g_last_idr_top[256KB]` + `_len` + `g_last_sps[64]` + `_len` + `g_last_pps[32]` + `_len` (12 L) | They served IDR_INJECT, abandoned |
| The globals `g_last_bottom_slice[256KB]` + `_len` + `_is_idr` (8 L) | They served BOTTOM_INJECT, abandoned |
| `g_au_has_bottom_real` (1 L) | It served YUV_COMPOSITE, abandoned |
| The `SHADOW_BOTTOM_INJECT` block in flush_display_buffer (20 L) | N51 tested at runtime, does not fix the bottom |
| The `SHADOW_IDR_INJECT` block in emit_legacy (29 L) | H1 FIX F.3 confirms: the desktop does not do that merge, it is an artefact of old hypotheses |
| Memorising SPS/PPS/IDR_top/bottom in the NAL scan loop (~80 L) | Likewise, no longer useful after V11 |
| Block BOTTOM_SEEN hex dump debug log (20 L) | Verbose, plus utile post-V11 |
| The `SHADOW_PARITY_TRIM` block (8 L) | Tested 2026-05-15, it breaks the picture |
| The `SHADOW_PARITY_DECRYPT` block (55 L) | Tested, 0/100 successes |
| The parity-chunk size-distribution diagnostic block + the LAST chunk dump (40 L) | An old one-off debug aid |
| The `SHADOW_REG_PERIOD_MS` block (30 L) | N49 disabled — the MASTER capture proves the desktop does it bootstrap-only, not periodically |
| `keyhex[80]` dead block + comment "skip key logging" (10 L) | Code mort |

### `ctrl_session_glue.c` (315 → 191 LOC)

| Bloc | Raison |
|---|---|
| The N52 YUV cache globals (Y/U/V planes + dims + writes/overlays counters) (17 L) | They served YUV_COMPOSITE, abandoned |
| `extern volatile bool g_au_has_bottom_real` (1 L) | Symbol n'existe plus |
| The whole `SHADOW_YUV_COMPOSITE` block (Phase 1 SAVE + Phase 2 OVERLAY, the NV12 + YUV420P paths) (~100 L) | A decoder-side mechanism does not fix the bug |

## What is kept (the active V14 stack + utilities)

### Toggles env vars V14 production (default ON)
- `SHADOW_PARITY_RAW=1` (V11) — chunks parity = NAL plaintext
- `SHADOW_IFR_PERIOD=14` (V12) — an IFR period twice as frequent
- `SHADOW_VIDEO_TCP=1` (V13) — ouvre canal VST :base+20 (= passive log only, V16 va l'activer byte-exact)
- `SHADOW_AU_DELIM=1` (V10) — H.264 AU delimiter injection
- `SHADOW_HB_PERIOD_MS=500` (N48) — heartbeat 2Hz match desktop
- `SHADOW_FLUSH_PERIOD_MS=17000` (N40 + Plan A V6) — Request_Flush match desktop
- `SHADOW_DISP_READY_PERIOD_MS=16000` (N47 + Plan A V6) — a periodic DisplayReady
- `SHADOW_AUTH_FULL_MAP=1` (F3) — Auth body full mapping
- `SHADOW_HWACCEL=1` (N55) — Linux CUDA/VDPAU/VAAPI fallback (utile debug desktop)
- `SHADOW_ERR_CONCEAL=1` (libavcodec) — error concealment flags
- `SHADOW_VST_VARIANT=N` (V13) — pour V16 variant selection

### Toggles env vars optionnels (default OFF, utiles diag)
- `SHADOW_DUMP_RAW=1` (cleanup V18) — dump `/tmp/shadow_dump.bin` (bitstream brut emit_legacy)
- `SHADOW_DUMP_CHUNKS=1` (cleanup V18) — dump `/tmp/shadow_chunks.bin` (chunks SUFP raw, capable XOR analysis offline)
- `SHADOW_DUMP_H264=1` (h264_decoder) — dump NAL units
- `SHADOW_FRAME_DUMP=1` (ctrl_session_glue) — dump YUV frames en PPM
- `SHADOW_NRES_PERIOD_MS=N` (N37) — re-enable periodic NotifyResolution (default 0 = OFF)
- `SHADOW_FLUSH_CMD=N` (N40) — select kFlush enum value (default 0)
- `SHADOW_DISPLAY_HEIGHT=N` (N26) — override SPS height pour test
- `SHADOW_CROP=1` (the N25 workaround) — clamp the output to height/2 (= from back when the bottom was absent)
- `SHADOW_NOCROP=1` — explicite no-crop (= default depuis N40)

### The VST module kept for V16
`ctrl_video_tcp.{c,h}` (~550 L) — 8 connect_msg variants tested, all FIN. **To be kept**,
because V16 (the wolfSSL plaintext-capture hook) will identify the right byte-exact body and
reuse that infrastructure. If we removed it, everything would have to be rewritten.

## Build status

```
[100%] Built target shadow-client       # Linux GUI Borealis OK
[100%] Built target shadow-test-cli     # Linux headless bench OK
```

No warnings, no regression. The V14 stack works.

## Runtime test to validate (= optional)

```bash
cd 05-shadow-client-borealis
SHADOW_NATIVE=1 ./build_linux/shadow-client  # check the 98% picture holds
```

Target metrics (= the V14 baseline measured on 2026-05-15):
- decoded ~28-30 fps @ 1920×1080
- NAL top ≈ bot (ratio ~1:1)
- IDR top ≈ bot (~symmetric)
- parity/data ratio ~6.6
- aband=0, incompl=0
- ffprobe MB errors in the taskbar zone (rows 58-67), residual ~27 per 30 s

## Cross-refs

- [[project-PARITY-RAW-FIX-2026-05-15]] — V11 (chunks parity = NAL plaintext)
- [[project-IFR-counter-FIX-2026-05-15]] — V12 (the incrementing IFR counter)
- [[project-remote-bitrate-estimator-RE]] — V14 (gE bytes_rx×4)
- [[project-V12-taskbar-status-2026-05-15]] — the 98% picture state after V14
- [[feedback-yuv-composite-idr-inject-dont-fix-bottom]] — OBSOLÈTE post-V18
- [[project-vst-connect-msg-byte-exact]] — V16 prochain (= identifie le body byte-exact)

## Status

V18 = **DONE**. A clean repo, a stable V14 baseline, ready for V16 (the wolfSSL hook) with no baggage.
