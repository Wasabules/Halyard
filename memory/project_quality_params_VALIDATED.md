---
name: project-quality-params-validated
description: A/B/C of the client's bitrate and fps requests - bitrate scales to a server ceiling, fps is not honoured at all on this plan.
metadata:
  node_type: memory
  type: project
---

# Quality parameters - what the server actually honours

DECISIVE 2026-05-18. An automated test (`tools/test-quality-params.sh`) answering
"does the server honour the bitrate and fps we ask for?"

## Method

Three 30 s headless runs with `SHADOW_BITRATE_MBPS` and `SHADOW_FPS`: low
(5 / 30), mid (30 / 60), ultra (80 / 120). Measured: real average Mb/s from the
bytes received, and real fps from the frames decoded.

## Results, on that account and VM

| Config | requested | measured Mb/s | measured fps |
|---|---|---|---|
| low | 5 / 30 | **3.12** | **29.17** |
| mid | 30 / 60 | **11.01** | **27.93** |
| ultra | 80 / 120 | **14.01** | **23.59** |

## Conclusions

**Bitrate: partially honoured.** It scales linearly from 5 to 30 (3.1 -> 11.0
Mb/s), then hits a **server ceiling around 14 Mb/s** - asking for 80 achieves
nothing. Small values are respected, large ones are capped.

**Frame rate: not honoured.** About 28-30 fps whatever is asked, on this plan
and VM. Ultra even measures *fewer* fps than low.

**Careful with that second conclusion.** FPS1 later showed the desktop being
streamed is largely static, so the encoder emits about 32 *distinct* frames per
second regardless - the measurement was of the content, not of a cap. And DEBIT-1
later showed the desktop bench is structurally blind to the bitrate defect: a
motionless desktop never asks for more, so every step from 25 to 100 Mb/s
measures the same 6.5-7.4 Mb/s. **Measure quality parameters on moving content
or not at all.**

Related: [[project_FPS1_server_modes_VBR]].
