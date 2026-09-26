---
name: project-Q1-quality-params-2026-05-18
description: "Q1 2026-05-18 — Bitrate/fps/resolution/codec/profile exposed through env vars (SHADOW_BITRATE_MBPS, SHADOW_FPS, SHADOW_CODEC, SHADOW_PROFILE_ID) + CLI flags (--bitrate, --fps, --width, --height). A dynamic CI_BODY_5 builder, byte-exact with the desktop for the defaults. Pre-V18: everything was hardcoded at 1920×1080 @ 144 fps @ 20 Mbps."
metadata:
  node_type: memory
  type: project
---

# Quality params overridables — bitrate/fps/resolution/codec/profile

## Pre-Q1: everything hardcoded in CI_BODY_5

Le 5e channel announcement (= video) contenait :
```
0x0a 0x1a                              # outer f1 sub, len=26
  0x0a 0x06 0x22 0x04 0x10 0x02 0x20 0x01  # codec_config { codec=2, profile=1 }
  0x12 0x0b 0x08 0x80 0x0f 0x10 0xb8 0x08 0x1d e1 da 0f 43  # resolution { 1920x1080 @ 143.85 fps }
  0x38 0x80 0xda 0xc4 0x09             # max_bitrate = 20000000 bps = 20 Mbps
```

The bytes were 100 % hardcoded. Our client announced a 20 Mbps maximum to the server while the official desktop app lets the user choose 5-100 Mbps through the UI.

## Q1 fix

`ctrl_msgs.c` : nouveau `ctrl_build_channel_announcement_ex()` + builder dynamique
`build_video_chan_body()`, which produces a byte-exact CI_BODY_5 for custom values.

**Test byte-equality** : avec values defaults (1920×1080, 143.85f, 20Mbps, codec=2,
profile=1) → the dynamic builder produces `MATCH — 115 identical bytes` with the
hardcoded original.

### Exposed through 3 paths

**1. Params struct** (= GUI / programmatic) :
```c
ctrl_session_params {
    uint32_t max_bitrate_mbps;  /* 0 = default 20 */
    float    target_fps;        /* 0 = default 143.85 */
};
ctrl_session_glue_params {
    uint32_t max_bitrate_mbps;
    float    target_fps;
};
```

**2. CLI flags `shadow-test-cli`** :
```bash
--bitrate=50       # Mbps
--fps=60           # frames/sec target
--width=1280       # display width
--height=720       # display height
```

**3. Env vars** (= highest priority, override params struct) :
```bash
SHADOW_BITRATE_MBPS=50
SHADOW_FPS=60
SHADOW_CODEC=2     # 2=H.264 assumed, to be confirmed in Q2
SHADOW_PROFILE_ID=1  # 1=speed/low-latency assumed, 2=quality?
```

## Validation runtime

Test 2026-05-18 ~01:53 avec `SHADOW_BITRATE_MBPS=50 ./tools/auto-test.sh 30 health-check` :
- Log : `[Q1] channel video params: 1920x1080 @ 143.9 fps @ 50.0 Mbps codec=2 profile=1`
- Result : bootstrap_ok=true, fps=38.4, decrypt=100%, bottom NAL=49.88%
- A 5 s session (= the VM is slow to start, unrelated to Q1)

The server **accepts** the new parameters without rejecting them. The visual effect on quality
to be validated through a real bench_runner.py A/B (= the single-run variance is enormous).

## Unknown / to RE with Q2

| Parameter | Desktop default | Possible values | RE method |
|---|---|---|---|
| codec | 2 | 0=AV1?, 1=H.265?, 2=H.264?, 3=? | A Q2 capture with a different codec setting |
| profile | 1 | 1=speed/low-latency?, 2=reliability/quality? | A Q2 capture with the profile changed |
| max_bitrate_bps | 20 Mbps | up to? (= the desktop app's UI should reveal the max) | Q2 |
| target_fps | 143.85 | the desktop default with no UI change = ~144; tested at 60 / 120? | Q2 |

## A limited effect as long as the CAPABILITIES are not extended

A critical note: we only advertise `OCapture` as a capability. The desktop app
annonce 7 capabilities : `display_management, gamepads, network_notifications,
dynamic_bitrate, multiscreen, streaming_profile, audio_out_codec`
(cf `tools/ida/out/H1_V9_server_discrimination.md`).

Without `dynamic_bitrate` advertised, the server may treat us in static mode
(= it ignores bitrate changes after bootstrap). But the CI_BODY_5 parameters
the INITIAL one should be taken into account (= before the stream starts).

## Cross-refs

- [[project-A18-automation-infra-2026-05-18]] — auto-test.sh permet A/B rapide Q1
- [[project-V18-cleanup-baseline-2026-05-18]] — the pre-Q1 cleanup
- [[project-iter-2026-05-06-bandwidth-caps]] — the previous bitrate iteration (= WebRTC, an invariant 500 Kbps server cap, but on a different path)

## Status

Q1 = ✅ DONE technique (= byte-exact match + env vars + CLI).
Q2 (= capture 2 desktop profiles) is to be done to identify the codec/profile values.
Q3 (= the Borealis GUI panel) is to be done after Q2 (= it needs valid values).
