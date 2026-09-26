---
name: project-image-bug-solved-offbyone-g4
description: "SOLVED 2026-06-02 - the artefacted bottom of the picture was an off-by-one: max_chunks (SUFP bytes 4-5) is the INDEX of the last chunk, not a count. We dropped the last chunk (the bottom slice's tail) of EVERY frame. max_chunks+1 took offline ffmpeg from 761 errors to 0."
metadata:
  node_type: memory
  type: project
---

# Image bug SOLVED - the chunk-count off-by-one (G4)

DECISIVE 2026-06-02. After months on false leads (FEC, parity, loss, hwaccel),
the root cause of the illegible, artefacted bottom of the picture in motion was
a **trivial off-by-one** in the counting of video UDP chunks.

## The cause

`max_chunks` (SUFP v3 header, bytes 4-5) is the **INDEX of the last chunk**
(0-based), **not a count**. The server sends indices `0..max_chunks`, i.e.
`max_chunks+1` chunks. The historical guard `chunk_idx >= max_chunks`
(`core/protocol/ctrl_session.c`, `on_video_packet`) **silently dropped the last
chunk** - `idx == max`, always `flag10=0`, a raw continuation carrying the tail
of the bottom slice - **on every frame**.

So the bottom slice was truncated by about one chunk, giving a CABAC error at
the bottom of each frame, which then **propagated across the GOP** through the
P-frames (each one predicting from the corrupted bottom) until the next key
frame. Very visible in **motion**, nearly invisible on a static picture - which
is exactly why earlier "clean picture" readings, all taken on static content,
were wrong.

## The proof, and the method is the point

1. **Offline ffmpeg** decode of the bytestream we assemble (`SHADOW_DUMP_H264=1`)
   -> 761 MB errors, **100 % in the lower half** (rows 34-67, worsening
   downward), zero at the top. So: not a decoder, GUI or hwaccel bug.
2. The bottom slice of a key frame, extracted alone, decoded rows 34 -> **64**
   and then ran out - short by **exactly one chunk**.
3. A diagnostic on the guard: `[DIAG] REJECT chunk idx==max` fired on **every**
   frame (idx=11/max=11, idx=4/max=4, idx=6/max=6...), always `flag10=0`.
4. Visually: a perfect top, a sharp boundary at ~50 %, a smeared bottom, worse
   at the end of a GOP (frame 55 far worse than frame 2) - the signature of
   propagation.

## The fix

In `on_video_packet`, right after parsing: `if (max_chunks > 0 && max_chunks <
VID_MAX_CHUNKS) max_chunks += 1;` (`SHADOW_MAX_COUNT_PLUS1=0` reverts).
Everything downstream - the guard, the completion count, the flush loop, the
NACK - then works from the right count.

**Result**: offline ffmpeg **761 -> 0 errors**, 971 frames clean, 0 rejects.
Confirmed at runtime on moving content.

## G3, fixed alongside (bitrate)

`vec_bitrate_bps` defaulted to 1024561 (~1 Mb/s - the desktop client's INITIAL
value, before a congestion-control ramp we do not implement) -> **15 Mb/s**. At
1 Mb/s, 1080p in motion is massive noise. `SHADOW_BITRATE_MBPS` overrides it.

## The false lead this replaces

The previous version of this analysis concluded "the bug is the NVDEC hardware
path, the bytestream is fine". **Wrong**: that work had analysed a dump of
**static** content, where a short bottom slice barely shows. Motion revealed the
off-by-one. What it did establish and that still holds: there is no FEC (F31),
trimming parity is a regression, and the software path is a good reference.

## Cross-ref

- KB §3.16 (solved), §3.9 (the UDP wire), §3.15 (NO FEC, F31)
- [[project_image_bug_reframed_hardware_path]] - SUPERSEDED
- [[project_F23_F24_FEC_unsolved]], [[project_F30_fec_brute_force_EXHAUSTED]] - SUPERSEDED
- [[feedback_automation_priority]]
