---
name: 🏆 Native Shadow streaming — pipeline stable bout-en-bout (run 15)
description: 2026-05-09 ~02:00 — The native bootstrap works perfectly with the cleanup BEFORE create + port_base = vm.port + 7000 + the SSE /stream + /status. H.264 1920x1080 streaming at a sustained ~32 fps, 100% chacha20 decrypt OK. SUFP disabled (it was saturating the window).
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records a May 2026 snapshot of 'it is stable now'.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

## TL;DR

DECISIVE 2026-05-09 ~02:00: on the fresh VM `lyric-upper-3b95fedf`, instance=6, port=6000 → base=13000, the native bootstrap runs **byte-exact end to end**:
- M8.connect to `:13011` immediately (= no retry)
- Capabilities → Auth → Encryption → RegisterSession → 8 channel announcements
- M15.streaming live
- ~32 fps soutenu (3160 packets en 10s, 3.7 MB), 100% decrypt OK

## Critical conditions (= all of them necessary)

1. **Clean up the zombies BEFORE proximus_create_*_client** (the order is essential — otherwise our JWTs are invalidated → a 401 on the SSE)
2. **port_base = atoi(vm.port) + 7000** (validated on 3 VMs: self-belt 2000→9000, revere-cool 3000→10000, lyric-upper 6000→13000)
3. **A JWT.instance rewrite** of the proximus_url (`/<slot>/` → `/<jwt.instance>/`)
4. **DUAL SSE**: `/N/stream` (launcher_jwt) + `/N/status` (main_jwt) in parallel (not two `/stream`s)
5. **Force-kill** the previous shadow-prod processes to avoid session conflicts on `:port_base+11`

## Components delivered (in the shadow-client Borealis Linux GUI)

- `streaming/ctrl_session.{h,c}` — port-based control + UDP register `:port_base+10/+13/+30`
- `streaming/ctrl_session_glue.{h,c}` — wrapper h264_decoder bridge
- `streaming/ctrl_rest.c::ctrl_rest_clean_my_zombies` + `clean_all_clients` — a DELETE filtered on the device-id, or forced
- `shadow/launcher.c::launcher_jwt_instance` + `launcher_rewrite_url_instance`
- `shadow/proximus.c::proximus_sse_start_ex` (suffix custom "stream" ou "status")
- `webrtc/h264_decoder.c::h264_decoder_feed_annexb` (split start codes + flush AU per call)
- `activity/connecting_activity.cpp` — toggle SHADOW_NATIVE=1 + SHADOW_NATIVE_FORCE_CLEAR=1

## Choix de design

- **Per-packet chacha20 + per-PTS reassembly** (= the legacy mode, what worked in run 7) instead of SUFP. SUFP_WINDOW_SIZE=32 saturated with ~250+ concurrent frames and gave `decoded=0`. The per-packet mode gives ~32 fps immediately.
- **Cabac decode warnings** observed (= lost / out-of-order UDP chunks not aggregated). Minor visual glitches but a fluid stream. A correct SUFP mode (= subchan-aware, a large window) is still TODO if we want zero loss.

## Stats observed (run 15, a fresh VM, t=10 s)

| t | video pkts | bytes | frames decoded | decrypt OK |
|---|---|---|---|---|
| 5s | 1913 | 2.2 MB | 179 | 192/192 |
| 10s | 3160 | 3.7 MB | 321 | 334/334 |

= ~32 fps moyen, ~3 Mbps, 100% chacha20.

## Use

```bash
cd $REPO/05-shadow-client-borealis/build_linux
SHADOW_NATIVE=1 SHADOW_NATIVE_FORCE_CLEAR=1 ./shadow-client
```

`SHADOW_NATIVE_FORCE_CLEAR=1` is recommended for debugging (= it kills every concurrent session including the official desktop app). In production, remove it so as not to kill the user's other sessions.

## TODO

- M3 — Validation visuelle (= user confirme image clean)
- A correct SUFP subchan + a larger window (256+) if the glitches become annoying
- P15 — Audio DTLS+Opus
- P12 — Cursor render
- P16 — Inputs uplink
- Phase 4 — the Switch port (everything is portable C + wolfSSL/jansson cross-compiled)
