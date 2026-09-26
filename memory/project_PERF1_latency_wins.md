---
name: project-perf1-latency-wins
description: Post-N56 latency batch - TCP_NODELAY was missing on three TCP sockets, costing up to 40 ms per send on the control and input paths.
metadata:
  node_type: memory
  type: project
---

# PERF1 - latency wins after N56

DECISIVE 2026-05-18. A full hot-path analysis after the SO_RCVBUF win
([[project_N56_udp_rcvbuf_FIX]]).

## The hot path, stage by stage

1. UDP receive - tight polling, no sleep, adds nothing
2. chacha20-poly1305 decrypt - about 50 us per packet
3. SUFP reassembly - a memcpy and a malloc per chunk
4. display-buffer concatenation - a memcpy
5. libavcodec send/receive - flags already optimal (`LOW_DELAY`, `FAST`,
   `thread_count=1`, `has_b_frames=0`)
6. `av_hwframe_transfer_data` - 1 to 2 ms at 1080p
7. the frame callback - a push into the pacing queue
8. the draw at vsync - +16.6 ms at 60 Hz

**Bottlenecks, by impact**: no `TCP_NODELAY` on three TCP sockets (+40 ms per
send); no receive buffer on the audio socket (a possible underrun on a burst);
the pacing queue at 4 (+66 ms, but deliberate - it absorbs decoder bursts, do
not touch); per-chunk allocation at 180-300/s (low return); the audio output's
own latency (hardware); and the 60 Hz vsync floor.

## What was fixed

**`TCP_NODELAY` on the control socket.** The control channel sends small
packets - feedback, auth, channel announcements. Nagle buffers them for 40 ms
before sending, so a 20 Hz feedback stream can accumulate a long queue for
nothing.

**`TCP_NODELAY` on the input socket.** Mouse and keyboard events are 96, 128 or
136 bytes at 30-60 Hz - exactly the traffic Nagle is designed to delay, and
exactly the traffic that must not be delayed.

**A receive buffer on the audio socket**, by symmetry with N56.

**The lesson**: three sockets were copied from a fourth, and the copies inherited
its shape without its `setsockopt` calls. That is the same failure mode as the
unbounded connect in `tls_chan.c` - a socket copied "the same as" another must be
DERIVED from it, not retyped.
