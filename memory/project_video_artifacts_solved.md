---
name: project-video-artifacts-solved
description: Video artefacts - two client-side causes (full-range colour, and chunk loss at high bitrate because receive and decode share a thread), plus the G23/G25/G28 follow-ups.
metadata:
  node_type: memory
  type: project
---

# Video artefacts SOLVED (2026-08-22)

**Method**: dump the assembled H.264 and decode it **offline** with our own
decoder, compare frame by frame against the live decode through a luma
fingerprint, and dump the rendered screen (`SHADOW_DUMP_RENDER`, glReadPixels).

What that proved, without assuming anything:

- the stream we assemble is **clean** (ffmpeg: 0 errors; our decoder: 0 errors);
- the live decode is **bit-exact** with the offline one (2600/2600
  fingerprints), so neither the stream nor the decoding was at fault - a lead
  followed wrongly for a long time;
- the rendering is faithful.

**Two real defects, both on the client side:**

1. **Washed-out colour.** The Shadow stream (a Windows desktop) is **full
   range** (luma 0-255, no VUI), but the GL shader applied the limited-range
   conversion `1.164*(Y-16)`. Fixed with a full-range shader and BT.709;
   `SHADOW_COLOR_RANGE` and `SHADOW_COLOR_MATRIX` select otherwise.
   *(Superseded on 2026-09-10 by COL1, which measured three known colours and
   found the stream is limited-range BT.709 after all. See KB §9.)*
2. **Accumulating blocks.** `ctrl_session.c` did UDP receive **and** synchronous
   decode on the SAME thread. Nothing is read while decoding, so at high bitrate
   the socket buffer overflows in bursts, slices are lost, and stale macroblocks
   drift until the next IDR. Measured: 50 Mb/s = 1.3 % loss (picture stuck),
   12 Mb/s = 0.14 % (clean). Fixed by G19 (adaptive bitrate on measured loss;
   the server honours `kUpdateSession` live) and G18 (request an IDR on decoder
   errors - *later set to 0, see [[project_G37_idr_spam_nvdec_phantom]]*).

**A trap worth keeping**: `glReadPixels` in `GL_RGB` needs
`glPixelStorei(GL_PACK_ALIGNMENT, 1)`, or the dump shears into horizontal
stripes - a fake artefact.

## G23 / G24 / G25 - what the official client does differently

It has no artefacts because it (1) decodes on a **dedicated thread** and (2)
holds a **~300 ms reorder buffer**, emitting in order, only complete, with an
IDR on real loss and no NACK or FEC. Everything else - parsing, `max` as the
last index, stripping the 28 AEAD bytes, concatenation, queue dedup - is
byte-exact with us.

Shipped: **G23** (disable the anti-green post-processing that repainted dark
16x16 macroblocks as stale - blocks on dark scenes) and **G25** (a dedicated
decode thread; `on_video` enqueues, the thread dequeues and decodes, joined
before destroy). **G24** (the reorder buffer) stays opt-in
(`SHADOW_REORDER_BUFFER=1`): our losses are real (~1.2 %), not jitter, so its
head-of-line blocking stutters more than it helps.

**Key lesson**: validate what is DISPLAYED (post-decode), not only the
bitstream. The anti-green pass ran after decoding and was invisible to ffmpeg.

## G28 - clean per-subchannel reassembly, abandons 2 % -> 0 %

The residual artefacts in motion came from frames **wrongly discarded**: the old
start-of-frame machinery misrouted a late tail (`idx == max`, sent after the
next frame had started) instead of letting it fall into its own frame's
per-subchannel buffer. G28 replaces all of it with plain per-subchannel logic
and emits on completion. Three runs: 0 abandoned, 0 ffmpeg errors.
`SHADOW_LEGACY_REASM=1` restores the old machinery.
