---
name: project-packet-timing-patterns-RE
description: TIER7 T1 — the desktop's cadence validated byte-exact on every channel. The 2 Hz heartbeat matches V14; the input `:base+14` cadence is 7-8 ms with bursts of 62 msgs. Not a taskbar trigger.
metadata:
  type: project
---

# TIER7 T1 — Packet timing patterns across channels

> **Mission** : extraire histogrammes timing par ssl pointer (= per-channel)
> to identify timing patterns our V14 missed.
>
> **Verdict** : **C95 byte-exact desktop**. Nos heartbeats 2Hz / DisplayReady
> 16 s / Flush 1 Hz match. Input `:base+14` has a very specific cadence
> (= 7-8 ms + bursts of up to 62 consecutive messages) — to be observed on the Switch side.
>
> **Not a direct taskbar trigger**, but the baseline timing is confirmed.

## SSL pointer → port mapping (capture MASTER)

| SSL ptr | Port | Channel | Writes/s | Format dominant |
|---------|------|---------|----------|-----------------|
| `0x2a3d75c0` | `:8011` | CtrlChanV2 | 2.15 | 93-94B (heartbeat 2Hz) |
| `0x2836e4a0` | `:8014` | InputFlatBuffers | **8.59** | 144B FB (5-10ms cadence) |
| `0x2835d990` | `:8020` | VideoSslTcpChan | 0.16 | 25B init + 1B heartbeat 7.5s |
| `0x283868a0` | `:8015` | ComChan | 0.09 | 10/22/66B activity |

## ctrl :8011 cadence dominante

```
len  count  p50_ms  type
 93   112    530   heartbeat seq<128
 94   342    513   heartbeat seq≥128
100    21     90   request multi-types (= Flush ACK?)
```

**Total 454 heartbeats / 235s = 1.93Hz = match notre V14 SHADOW_HB_PERIOD_MS=500.**

## input :base+14 burst pattern (= DÉCISIF Switch)

- Total 2001 writes / 233s
- **The largest burst: 62 consecutive writes in 456 ms** (= draining the queue)
- p50 interval = 6.79ms (= 125-200 Hz polling)
- 36 % of the intervals in 5-10 ms, 21 % in 0-1 ms (= the drain), 22 % in 1-2 ms

**Implication for the Switch**: if our I1 input channel sends less frequently
que desktop (= GLFW callback rate < 125Hz), server pourrait gate multi-NAL
on the input cadence (= the C60 hypothesis, consistent with 0.81% bottom NAL pre-V11).

## Refs

- Script timing : `tools/ida/tier7_timing.py`
- Script burst : `tools/ida/tier7_8014_burst.py`
- Script ssl→port : `tools/ida/tier7_ssl_to_port.py`
- Capture : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log`
- Doc TIER7 : `tools/ida/out/TIER7_RE_2026-05-16.md` §T1
- V14 heartbeat code : `ctrl_session.c:2150-2165`

## Cross-refs

- [[project-I1-I2-input-audio-skeleton-2026-05-18]] (= I1 input channel impl)
- [[project-CUR1-cursor-format-2026-05-18]]
- [[project-remote-bitrate-estimator-RE]] (= gE/rG packets sur :8010)
