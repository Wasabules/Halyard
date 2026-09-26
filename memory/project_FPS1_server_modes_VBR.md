---
name: project-fps1-server-modes-vbr
description: The apparent 30-32 fps cap is VBR frame-skip on a mostly static desktop, not a plan limit - the server's own reply lists twenty modes, several at 120 fps.
metadata:
  node_type: memory
  type: project
---

# FPS1 - the server's mode list, and why 32 fps is not a cap

DECISIVE 2026-05-18: understanding the 30-32 fps ceiling while the plan supports
120 Hz.

## Decoding the server's reply (channel announcement, index 0)

The server answers with a protobuf **listing twenty supported modes**:

```
Reply
  field 1 varint = 4
  field 3 = ServerInfo
    field 10 = StreamingProfiles
      field 2 = CurrentMode      { f1=w, f2=h, f3=fps fixed32 }
      field 3 = factor (1.0)
      field 4 x N = SupportedModes [ { f1=w, f2=h, f3=fps fixed32 } ]
      field 4 = ServerVersion  "6.2.0" "ShadowStreamer"
```

Modes on this plan included 1920x1080 @ 143.85 (the current one, our request),
1920x1080 @ 59.94, 2214x1080 @ 119.88, 1280x720 @ 59.93, 1920x1200 @ 120,
1920x1080 @ 120, 1680x1050 @ 120 - twenty in all, several at 120 fps.

**The plan does support 120 fps**, and the server accepts our requested mode.

## The apparent 32 fps is VBR frame-skip

A 30 s headless run with `SHADOW_FPS=120` and `SHADOW_BITRATE_MBPS=80`:
1696 packets/s, **17.04 Mb/s average** (so the bitrate request is honoured),
**31.0 fps decoded**, 32.7 top NALs/s, 49.6 % bottom.

The server honours the request and sends 17 Mb/s of quality, but the effective
frame rate is 32 because the desktop image is largely static - cursor, clock,
a little UI. The encoder produces about 32 *distinct* frames per second.

**Conclusion: the frame rate is a maximum, not a target.** Judging it on a
motionless desktop measures the content, not the pipeline.

**Superseded in part**: CFG-4 later established that the live message has had no
frame-rate field since S18, so `SHADOW_FPS` travels only in the channel
announcement and a frame rate never changes mid-session.
