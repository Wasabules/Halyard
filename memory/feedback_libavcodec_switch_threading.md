---
name: libavcodec H.264 on Switch — no slice threading
description: FF_THREAD_SLICE produces all-zero AVFrames (the chroma is never written) on Switch. thread_count=1, thread_type=0 is mandatory.
type: feedback
---
On Switch (switch-ffmpeg through dkp-pacman, libavcodec 7.1), configuring the H.264 decoder with `thread_type = FF_THREAD_SLICE` and `thread_count = 2` produces frames coming out of `avcodec_receive_frame` with **all 3 Y/U/V planes at zero** (the buffer is allocated but never written by the slice threads). In BT.601, YUV(0,0,0) → pure green → the user sees a green screen instead of the video.

**Why:** observed on 2026-05-02 on the M13 H.264. Frame #5 dumped to the SD card (`/switch/shadow-client/frame_5_1280x720_yuv420p.yuv`) had 1,382,400 bytes all at 0. The symptom = "a green picture with artefacts", as reported by the user. The frames were emitted correctly (the right 1280x720 size, fmt=AV_PIX_FMT_YUV420P), only the content was empty. Probable cause: libavcodec returns the frame before the slice threads have finished writing, or the Switch pthread threads (libnx) have a scheduling bug in that mode.

**How to apply:** on the Switch, always configure libavcodec H.264 as single-threaded:
```c
ctx->thread_count = 1;
ctx->thread_type  = 0;
```
A Tegra X1 decodes 720p at 30 fps single-threaded without trouble. `FF_THREAD_FRAME` (the other multi-threaded mode) adds ~100 ms of latency — not viable for interactive use. Do not waste time trying FF_THREAD_SLICE hoping for performance, it produces corrupt content.

For debugging future "zero frames": dump the frame with the correct linesize into a .yuv file and analyse it in Python to see whether Y/U/V really are 0 — that is the distinctive sign of an unwritten buffer (against real video, which has U/V around 128 even for black).
