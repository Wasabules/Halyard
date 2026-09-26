---
name: project-timesync-impact-RE
description: TIER7 T4 — the 124 B TimeSync reply is a one-shot server push with constant values (f6=29970, f7=600). Probably a fixed server configuration, not a runtime measurement. Not used for gE's avg_delay_us. Not a taskbar trigger.
metadata:
  type: project
---

# TIER7 T4 — TimeSync: what does the desktop do with the reply?

> **Mission**: we send a time_sync_request (= in fact just an implicit
> heartbeat) and the server replies with 124 B. Find the parser, identify where the
> the value is stored, and see whether it is injected into gE / the nonce / anything else.
>
> **Verdict C70** : TimeSync = server-push one-shot, valeurs constantes
> across the capture (= f6=29970, f7=600). Probably a server config
> fixed per VM template, not a runtime measurement. Not used for gE
> avg_delay_us.
>
> **Not a taskbar trigger.**

## The TimeSync wire, byte-exact (= already in TIER6 M1.6)

```
SSL_READ ctrl :8011 L26961 t=0.997 len=124 :
  08 0d                       ← f1=13 (echo seq)
  1a 1a 1a 18 12 16           ← Reply.f3.f3.f2 sub(22) = TimeSync
     0a 00                    ← f1 empty
     12 0c                    ← f2 sub(12) = TimeData
        08 d4 01              ← f1 = 212
        20 05                 ← f4 = 5
        30 92 ea 01           ← f6 = 29970   ← probable expected_RTT_us
        38 d8 04              ← f7 = 600     ← probable jitter_budget_us
     1a 00 22 00 2a 00        ← f3, f4, f5 empty
```

**Observed only once** in the whole 235 s capture = a one-shot at bootstrap.

## TimeSync request = ?

There is no explicit "TimeSync request" client message. The 93 B@seq=13 sent by
the desktop is a **normal heartbeat** (`12 04 1a 02 12 00` = an empty
Request.f3.f2). The server pushes the 124 B reply **spontaneously** at seq 13.

**Conclusion**: no client action to emit.

## IDA static : aucun string match

```bash
grep -i "timesync\|time_sync\|clock_offset\|delay_us" \
  06-shadow-recon-linux/dumps/strings_display.txt
  → 0 hits
```

Not statically locatable without a new targeted dump.

## The f6/f7 → gE avg_delay_us hypothesis?

The prompt suggests that the gE packet's `avg_delay_µs` (our V14's is 0) would be
computed from that TimeData. Refuted because:

1. **Constant values** across the whole capture → not a runtime measurement
2. `gE` is emitted by `RemoteBitrateEstimatorIOChannel` (= a mixin of
   `VideoUdpChannel`) and `avg_delay_µs` is probably the measurement
   of the video UDP inter-arrival delay, not derived from a clock offset.
3. f6=29970µs ≈ 30ms RTT = match latence WAN typique
4. f7=600µs ≈ jitter budget = match parameters HOS

**More likely**: `TimeData.f6/f7` = server parameters for QoS
budget (= `expected_rtt` + `jitter_budget`) that the server TELLS the client
to calibrate its local estimates.

## A parser recipe (= optional, hygiene only)

Si on veut extraire les values :
```c
static int64_t g_server_expected_rtt_us = -1;
static int64_t g_server_jitter_budget_us = -1;

void ctrl_parse_timesync_reply(const uint8_t *body, size_t len) {
    for (size_t i = 0; i + 6 < len; i++) {
        if (body[i]==0x1a && body[i+2]==0x1a
         && body[i+4]==0x12 && body[i+5]==0x16) {
            /* parse TimeData fields f6, f7 from body[i+10..i+10+12] */
            /* ... pb_read varint loop */
            return;
        }
    }
}
```

**Effort**: ~50 LoC. **Expected gain**: logging only, no behaviour change.

## Verdict

**C70**: TimeSync is a one-shot server config, not a runtime signal.
Our `gE.avg_delay_µs = 0` is suboptimal but not a taskbar trigger.

**Recommandation** : laisser tel quel. Si on veut vraiment fix `delay_us`,
mesurer **inter-arrival timing UDP video** runtime au lieu d'utiliser
TimeSync.

## Refs

- TimeSync reply hex : capture L26961
- The TIER6 M1.6 decode: `tools/ida/out/TIER6_RE_2026-05-16.md` §M1.6
- gE packet RE : `tools/ida/out/REMOTE_BITRATE_ESTIMATOR_RE.md`
- gE build code : `ctrl_session.c:1868-1888`
- Doc : `tools/ida/out/TIER7_RE_2026-05-16.md` §T4

## Cross-refs

- [[project-all-server-replies-RE]] (= TIER6 M1)
- [[project-remote-bitrate-estimator-RE]] (= gE/rG packets)
- [[project-V14-gE-bytes-rx-fix]] (= our V14 bytes_rx fix; the delay stays at 0)
