---
name: project-n56-udp-rcvbuf-fix
description: ROOT CAUSE of the blurry taskbar on the native path - the default 208 KB SO_RCVBUF saturates. 4 MB gives 0 HOLES over 90 s of stream.
metadata:
  node_type: memory
  type: project
---

# N56 - the blurry taskbar was a saturated 208 KB UDP kernel buffer

DECISIVE 2026-05-18. Root cause of "the bottom of the picture is unclean, with
artefacts", found and fixed through an automated, iterative test.

## Symptom

The wallpaper displayed cleanly while the **taskbar at the bottom** (MB rows
60-67) stayed blurry with constant artefacts. libavcodec errors in bulk:
`error while decoding MB X 60-67, bytestream -5..-18`, plus
`decode_slice_header error`.

## Diagnosis, in order

1. **First hypothesis**: `VID_MAX_CHUNKS=256` too small -> raised to 512. **No
   effect** (NALs are not longer than 256 chunks at 1080p).
2. Added a `HOLES first_hole_idx=N` log to `vid_reasm_flush` when chunks are
   missing.
3. **Observation**: 76 % of the holes had `first_hole_idx=0` - chunk 0, the
   slice header. Not uniformly distributed, so not random UDP loss.
4. **Second hypothesis**: switch the reassembly to frame_id instead of
   start-of-frame detection. **Disaster**: 2282 frames abandoned in 30 s,
   because packets of several frames interleave on the same reassembly and it
   switched on every packet. **Reverted.**
5. Debug logs showed each frame is on **its own subchannel** (subchan 0 max=25,
   subchan 1 max=1, subchan 2 max=3...), so one reassembly per frame is correct
   and nothing was being mixed.
6. **The real hypothesis**: if chunk 0 is systematically missing while the rest
   arrive, that is a **FIFO drop in the kernel buffer** - the first packets of
   the burst.
7. **Check**: no `setsockopt SO_RCVBUF` on the video UDP socket at all. Linux
   defaults to `net.core.rmem_default` = **208 KB**. At 1080p and 1.2 MB/s, that
   fills in **170 ms**.

## Fix

`setsockopt(s, SOL_SOCKET, SO_RCVBUF, 4 MB)` in `udp_register()`
(`core/protocol/ctrl_session.c`). `SHADOW_UDP_RCVBUF=<bytes>` overrides it. At
4 MB there are about three seconds of margin before a FIFO drop - enough to
absorb a GUI or decode pause.

## Benchmark (`tools/test-N56-holes.sh 3 30`)

| Metric | 208 KB baseline | 4 MB fix | Delta |
|---|---|---|---|
| avg fps | 28.8 | **30.2** | +5 % |
| frames | 817 | **854** | +5 % |
| HOLES total | **12** | **0** | -100 % |
| first_hole_idx=0 | 7 (58 %) | **0** | eliminated |
| incomplete | 3 | 0 | -100 % |
| reassemblies abandoned | 18 | 2 | -89 % |
| bottom NAL % | 49.0 | 49.0 | unchanged |
| decrypt OK | 100 % | 100 % | unchanged |

Zero holes across 90 s of aggregated stream.

## Note on the console

HOS does NOT honour this the way Linux does: it caps at whatever the socket
driver reserved, and Borealis never reads the size back. See KB §3.30 - the same
fix needed a different mechanism there.

## Refs

- [[project_runtime_issues_2026-05-18]] - the initial symptom
- [[project_HW1_hwaccel_rc5_fix]] - the fix that made a 67 s session possible
- [[project_BUG2_decoder_race_fix]]
- [[project_image_100pct_PROOF]]
