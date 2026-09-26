---
name: project-g37-idr-spam-nvdec-phantom
description: The residual GUI video artefact was a self-sustaining IDR spam driven by PHANTOM NVDEC errors, with zero real packet loss. Plus G38, G40 and G43, which found the actual root cause.
metadata:
  node_type: memory
  type: project
---

# G37 - the residual artefact was IDR spam on phantom decoder errors

**DECISIVE 2026-08-22.** The residual video artefact in the Linux GUI - "a fog
of blocks in motion, about 1 %" - was neither network loss nor a reassembly bug.

A real 185 s GUI session read `lost=0/467714, abandoned=0, skipped=0, ok=100 %`
- zero real loss, perfect reassembly - while the IDR counter climbed 1 -> 228,
about 1.2 per second. All of them came from **G18**.

**Cause**: the bitstream-error counter was **cumulative and never reset**, and
the **NVDEC/CUDA** path logs `decode_slice_header error` and `Frame num change`
at ERROR level on a **valid** stream - the same bytes decoded in software give
**zero** errors. So G18 counted phantoms, asked for an IDR every 1.2 s, and
re-created the autofocus pulsing of [[project_G5_autofocus_forced_idr_FIX]]. The
loop sustains itself: an IDR resets `frame_num` -> "Frame num change" -> another
error -> another IDR.

**Fix**: `SHADOW_IDR_ON_BITSTREAM_ERR` defaults to **0**. The counting stays, as
a diagnostic. Recovery from REAL loss is G26 (loss/abandon) and G36 (subchannel
gap), both silent at `lost=0`.

Offline proof also established: reassembly is correct on a clean link, `max`
never varies within a frame, and the server duplicates ONLY the `idx == max`
tail (~30 %). Emitting the complete prefix of an incomplete frame is **worse**
than dropping it - the decoder conceals and produces a parasitic block.

## G38 - the decode queue overflows (GUI only)

After G37 a rare but severe artefact remained. `lost=0, abandoned=0`, yet
`NAL top=12289` against `decoded=12062`: **227 assembled frames were never
decoded**. The decode thread's queue (16 deep) overflowed on the jerks a dark,
moving scene produces. A dropped frame breaks a reference -> fog -> an IDR ->
more contention -> a spiral. Queue raised to 64 (`SHADOW_DEC_QUEUE`).

## G40 - the real root cause: two pictures fused into one access unit

Software decoding artefacted **too**, which cleared NVDEC. Dumping the exact
access unit at each error showed all 65 faulty ones were
`P(mb0)+P(mb1760)+P(mb0)+P(mb1760)` - **two distinct pictures in one access
unit**. Cause: the emitter flushed on a change of timestamp, and two consecutive
pictures with the SAME timestamp (frequent in heavy motion) fused. ffmpeg
decodes them without error because it re-splits on `first_mb == 0`.

**Fix**: take the access-unit boundary from the STRUCTURE (`first_mb == 0` means
a new picture), not from the timestamp. `SHADOW_AU_BY_TS=1` restores the old
behaviour.

## G43 - frames whose tail was lost were being thrown away

Counters showed frames disappearing into a subchannel gap (~1.7 %), **always**
with only the last chunk missing (the bottom slice's ~489-byte tail). Those
nearly complete frames never reached the flush, so they were abandoned, leaving
a `frame_num` gap and a stale reference (`max_num_ref_frames=1`) - global drift,
worst in heavy motion.

Offline validation: emitting them **truncated** gives a mean absolute error near
zero, against a corrupted block of up to 200 when dropping. Fix: flush the
incomplete frame N-1 when frame N's start arrives, and emit a tail-loss
truncated rather than discarding it. IDRs excluded.

Related: [[project_video_artifacts_solved]],
[[project_image_bug_SOLVED_offbyone_G4]]. KB §9.
