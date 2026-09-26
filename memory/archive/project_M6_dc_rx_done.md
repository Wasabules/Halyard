---
name: M6 — DC RX callbacks done
description: 2026-05-06, onMessage callbacks wired onto the 4 Shadow DCs. Per-stream_id counters exposed in dc_session_stats. An optional C callback, `dc_session_rx_cb`, lets shadow_input.c (or the caller) process the bytes received.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the libdatachannel path, abandoned 2026-05-06.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

## Architecture

```
libdatachannel DC.onMessage(rtc::message_variant)
    │ (thread interne libdatachannel)
    ▼
make_rx_handler(stream_id) lambda dans dc_session.cpp
    │ atomic compteur ++ (state_->dc_rx_input/cursor/controller/clipboard)
    │ atomic bytes total +=
    │
    ├─ params_.rx callback C (optionnel) → caller (Borealis StreamView)
    │
    └─ (no-op si rx callback non set)
```

## API publique

`dc_session.h` :

```c
typedef void (*dc_session_rx_cb)(uint16_t stream_id, const uint8_t *data,
                                  size_t len, void *user);

typedef struct {
    ...
    dc_session_rx_cb rx;       // optionnel
    void            *rx_user;
} dc_session_params;

typedef struct {
    ...
    uint32_t dc_rx_input;       // stream 1
    uint32_t dc_rx_cursor;      // stream 3
    uint32_t dc_rx_controller;  // stream 5
    uint32_t dc_rx_clipboard;   // stream 7
    uint64_t dc_rx_bytes_total;
} dc_session_stats;
```

## Caller usage

In `connecting_activity.cpp` or a future cursor renderer:

```c
static void on_dc_rx(uint16_t stream_id, const uint8_t *data, size_t len, void *user) {
    if (stream_id == 3) {  // shadow-cursor
        // Decode the FlatBuffer cursor → push to the StreamView overlay
    }
}

dc_params.rx = on_dc_rx;
dc_params.rx_user = my_state;
```

## État infrastructure end-to-end

```
Borealis input → shadow_input.c → DcInputBridge.send → libdatachannel DC.send → wire ✅ M5
                                                             ▲
                                                             │ wire response
libdatachannel DC.onMessage → dc_session rx callback → caller ✅ M6
```

The **TX** inputs are wired (M5). The **RX** ones are wired (M6) as far as the caller's callback. Decoding the content (the cursor bitmap, clipboard sync) is still to be implemented IF the user wants a visual cursor overlay — for basic gameplay, RX is not needed (the cursor is handled server-side, the gameplay video is decoded directly).

## Build verified

```
shadow-client     28.3 MB ✅
dc-shadow-cli      4.5 MB ✅
shadow-test-cli    1.7 MB ✅
dc-smoke           3.0 MB ✅
```

## Overall state of the migration

| Step | Status | Benefit |
|---|---|---|
| M1 build libdatachannel Linux | ✅ | Lib disponible |
| M3 signaling wrapper | ✅ | dc-shadow-cli connected to Shadow |
| T4 TWCC sender | ✅ | The server's BWE unblocked |
| T6 1080p + hid-sync + ICE-mid | ✅ | 18 Mbps peak atteint |
| T7 RTX pairs | ✅ | Anti-saccade |
| M4 h264 bridge | ✅ | YUV frames decoded |
| M4 GUI migration | ✅ | shadow-client utilise dc_session |
| M5 DC bridge inputs | ✅ | Mouse/keyboard/gamepad TX |
| M6 DC RX callbacks | ✅ | The cursor/clipboard infrastructure is ready |
| **Test gameplay Linux** | ⏳ | Attendu utilisateur |
| M2 Switch port | ⏳ | À venir |

The Linux shadow-client GUI is in a complete, usable end-to-end state for gameplay (video + audio + inputs). Remaining: a real test + the Switch port.
