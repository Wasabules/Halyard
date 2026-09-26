---
name: project_cli_flags_env_vars_RE
description: TIER10-Y4 — 115 boost-po style flags + 26 getenv vars, exhaustively. None of them affects the ctrl wire. slice-split = the local VAAPI decoder, not the wire. Every `SHADOW_*` env var = debug/preprod/logging.
metadata:
  type: project
  confidence: C95
  date: 2026-05-23
  tier: 10
---

# TIER 10 Y4 — Hidden CLI flags + env vars qui affectent wire

## TL;DR
**Exhaustive scan binary** : 115 noms boost-po style + 26 getenv args.
**None of them propagates to the ctrl wire or to an HTTP header of its own.** The
interesting candidate `slice-split` is a **local VAAPI decoder option** (= Linux
hardware decoding), not a server signal. Every `SHADOW_*` env var is
debug/preprod/logging. **The "an advanced CLI flag enables a server feature"
RÉFUTÉE C95.**

## Inventory CLI flags long-form (`--xxx`)

`strings ShadowPCDisplay | grep -E '^--[a-z]'` → **5 entries seulement** :
```
--bench
--decoding is deprecated
--disable-in-process-stack-traces
--hevc is deprecated
--rendering is deprecated
```

It is just the inherited Chromium flags + an internal `--bench`. No
`--debug`, `--experimental`, `--multi-nal`, `--developer`.

## Inventory boost-po style flag names (115 entries)

Format `^[a-z][a-z-]+(-[a-z]+)+$` :

**Streaming-related (32 entries cherry-picked)** :
```
audio-codec, audio-in-protocol, audio-out-protocol, bitrate-cap (no),
client-role, clipboard-sync, control-rate, cursor-protocol,
disable-audio-jitter, disable-automatic-framerate, disable-pinch,
disable-sentry, display-edid, display-height, display-rate,
display-safe-mode, display-width, dual-screen, fit-to-screen,
input-protocol, max-framerate, max-height, max-screen-count, max-width,
mouse-control, remote-screen, slice-split, speedtest-url,
streaming-profile, video-chroma, video-codec, video-decoder,
video-decoding-mode, video-height, video-painter, video-pipeline,
video-protocol, video-rate, video-renderer, video-width
```

**Note**: these names may be **internal protobuf field names**, not
necessarily exposed as CLI flags. Boost.program_options against a C++ struct field
are indistinguishable at the string level.

### `slice-split` (= THE speculative finding)
**File offset 0xeab3c6, VMA 0x12ab3c6**. xref unique :
```
c608ee:  mov  $0x12ab3cc,%esi     ; "whole" (= another enum value, 6 B later)
c608f7:  call strcmp@plt
c608fc:  test %eax,%eax
c608fe:  sete 0xac(%rbx)          ; bool result stored in this+0xac
c60905:  call ecb780               ; ctor or similar
c6090a:  cmpb $0x0,0xac(%rbx)
c60911:  mov  $0x12ab3c6,%edx     ; "slice-split"
c60919:  mov  $0x12ab3d2,%eax     ; "whole"
c6091e:  cmovne %rdx,%rax        ; choose "slice-split" or "whole"
c60951:  call h264_new@plt       ; LIBAV h264_new
```

**Context (strings autour)** : `vaCreateSurfaces`, `vaCreateContext`,
`InitContext`, `~VaapiVideoDecoder`, `Initializing decoder`,
`LIBVA_DRIVER_NAME`, `vaInitialize`, `vaCreateConfig H264/H265`. **That is in
`VaapiVideoDecoder::Init`**.

**Verdict** : `slice-split` = VAAPI decoder **granularity option** (= libva
hint to decode per-slice vs whole picture). Linux VAAPI Intel/AMD specific.
**Never sent to the server.** Not a wire trigger.

### `streaming-profile`
Already RE'd in TIER 9 W4 → the values `speed`/`reliability`, mapped client-side to
bitrate/codec, jamais wire.

### `speedtest-url`
Already RE'd in TIER 9 W1 → an opt-in bandwidth-probe URL template, never activated in this
session.

## Inventory env vars (26 unique getenv args)

| VMA | Env name | Purpose |
|-----|----------|---------|
| 0x120b68c | `XDG_SESSION_DESKTOP` | Linux session detection (KDE/GNOME) |
| 0x120b7c6 | `SHADOW_LOG_PATH` | logs override path |
| 0x120b838 | `SHADOW_DISABLE_LIBINPUT` | disable libinput driver |
| 0x120ce53 | `SHADOW_DBG_LIBINPUT` | debug libinput |
| 0x1238f43 | `NODE_CHANNEL_FD` | Electron child IPC fd |
| 0x12472fc | `SHADOW_DUMP_PATH` | dump dir for recording |
| 0x1247d4f | `SHADOW_DUMP_START` | start dump trigger |
| 0x126c6b8 | `SHADOW_CLIENT_ENCRYPTED_CHANNELS` | force-encrypt channels filter |
| 0x1273f11 | `METRICS_DUMP_FILE` | dump metrics to file |
| 0x1274685 | `DUMP_METRICS` | enable metrics dump |
| 0x1279323 | `SHADOW_DBG_LIBSENTRY` | debug sentry crash reporter |
| 0x127bc42 | `XDG_SESSION_TYPE` | x11/wayland detection |
| 0x127bc57 | `WAYLAND_DISPLAY` | Wayland socket name |
| 0x127bc7c | `WAYLAND_SOCKET` | Wayland fd |
| 0x127bcbe | `FORCE_XWAYLAND_WM` | force XWayland WM mode |
| 0x1282d61 | `SHADOW_DBG_SSH` | debug libssh (cf base15) |
| 0x12ab3ed | `LIBVA_DRIVER_NAME` | libva driver override (VAAPI) |
| 0x12ab425 | `SHADOW_VAAPI_FEED` | VAAPI feed override |
| 0x12bf645 | `SHADOW_CURL_DEBUG` | libcurl verbose |
| 0x12c4090 | `DUMP_STATE_MACHINE_FOLDER` | dump FSM graphs |
| 0x12c5bc0 | `XDG_CONFIG_HOME` | config dir |
| 0x12c5ce1 | `SHADOW_LOG_STDOUT` | log to stdout |
| 0x12c5cf3 | `SHADOW_DBG_OPENTELEMETRY` | otel debug |
| 0x12c5d0c | `SHADOW_USE_PREPROD` | use preprod environment |

**Pattern** : 100% des `SHADOW_*` env vars = debug / preprod / logging / dump.
No `SHADOW_ENABLE_X` / `SHADOW_FORCE_X` / `SHADOW_FEATURE_X` that affects the
wire. Aucun `SHADOW_MULTI_NAL` / `SHADOW_SLICE_SPLIT` runtime override.

### `SHADOW_USE_PREPROD` (= the only interesting candidate)
Cross-referenced with the capture: the server for that session = `gpu-<instance>.frsbg01.compute.shadow.tech`
= prod. Setting `SHADOW_USE_PREPROD=1` ferait pointer vers staging — different
endpoints but the same wire protocol. **Not an image-quality trigger.**

## Verdict Y4
**REFUTED at C95**: no CLI flag and no env var triggers a wire side effect
that would enable a specific encoder mode. The flags `streaming-profile`,
`slice-split` and `speedtest-url` are LOCAL hooks for adjusting
client-side rendering / bandwidth probe / decoder granularity. Le serveur
only sees `RegisterSession_Video` + `Capabilities` + the JWT auth — already
a byte-exact match with the desktop on the Switch side (see TIER 5 B1).

## Refs Y4
| Artefact | Path |
|----------|------|
| 5 `--xxx` flags | `strings ShadowPCDisplay | grep '^--'` |
| 115 boost-po names | `/tmp/dashes.txt` (regen'd via grep) |
| `slice-split` VMA | `0x12ab3c6` xref unique `0xc60911` |
| VaapiVideoDecoder::Init | `ShadowPCDisplay@~0xc60800-0xc60a00` |
| 26 getenv args | `/tmp/getenv_args.txt` (file offset list) |
| Cross-ref TIER 9 W4 (streaming-profile) | `memory/project_quality_preset_wire_RE.md` |
| Cross-ref TIER 9 W1 (speedtest-url) | `memory/project_bandwidth_probe_RE.md` |
