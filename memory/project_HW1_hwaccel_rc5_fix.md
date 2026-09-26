---
name: project-hw1-hwaccel-rc5-fix
description: Root-cause fix for hwframe_transfer FAIL rc=-5 (NVDEC/CUDA on Linux) - software-only libavcodec flags conflict with any hwaccel.
metadata:
  node_type: memory
  type: project
---

# HW1 - the root cause of hwaccel rc=-5

DECISIVE 2026-05-18. A deep fix for `hwframe_transfer FAIL rc=-5 (Input/output
error)`, which was producing stuttering audio, slow-then-fast playback, and half
the frames dropped.

## Root cause

`AV_CODEC_FLAG_OUTPUT_CORRUPT`, `AV_CODEC_FLAG2_SHOW_ALL` and
`error_concealment=259` are **software-only** in libavcodec - implemented only
in the CPU path of `h264dec.c`.

Under any hwaccel (CUDA / VDPAU / VAAPI / NVTEGRA) the hardware decoder produces
partially decoded surfaces, with the missing macroblocks left undefined in GPU
memory. Forcing those frames out with `OUTPUT_CORRUPT` and then calling
`av_hwframe_transfer_data` makes the driver see an invalid surface: `rc=-5` and
`Failed to sync surface 0xNN`.

That is exactly the Shadow case, where P-frames arrive without their bottom
slice: with those flags libavcodec emits the partial frame and NVDEC cannot DMA
a surface that is not consistent.

## The fix, in three layers

1. **Turn the software-only flags off under hwaccel** - this is the root cause.
   `if (d->hwaccel_active)` then `error_concealment=0`, `err_recognition=0`, no
   `OUTPUT_CORRUPT` and no `SHOW_ALL`. The CPU path keeps them, because there
   they produce the "sharp top, stretched bottom" picture rather than nothing.
2. **Retry once** on a single failure (a momentarily busy GPU):
   `av_frame_unref(sw_frame)` then a second `av_hwframe_transfer_data`. On
   success the failure streak resets.
3. **A sticky fallback across sessions** after five failures: write a marker in
   the data directory (`hwaccel.disabled`), read at `h264_decoder_create()`, and
   skip hwaccel from then on. Remove the file, or set `SHADOW_HWACCEL=1`, to
   undo it.

## The subtlety that bites after the sticky disable

The decoder context stays configured for hwaccel, so frames keep arriving in GPU
memory. Skipping on `d->hwaccel_active` alone would send `frame->data[]` full of
device pointers down the CPU path and crash the view. A separate `is_gpu_format`
check drops the frame instead.

## Files

`core/media/h264_decoder.c` - the marker path, the failure streak, the
conditional flags, the retry and the sticky logic.

## Env

- `SHADOW_HWACCEL=0` - no hwaccel at init (bypasses the marker)
- `SHADOW_HWACCEL=1` - force it (overrides an existing marker)
- `SHADOW_ERR_CONCEAL=N` - override concealment, CPU path only

## Deliberately not done

- **Recreating the context on the CPU at sticky-disable**: too heavy at runtime
  (flush, re-open, reload extradata). Dropping silently for the rest of the
  session and fixing it at the next launch is enough.
- **Probing the GPU before committing to hwaccel**: needs a dummy H.264 decode;
  the sticky marker achieves the same for less.

## Refs

- [[project_runtime_issues_2026-05-18]] - the initial symptom
- [[project_image_100pct_PROOF]]
- [[project_N54_ffmpeg_nvdec_BUILT]]
