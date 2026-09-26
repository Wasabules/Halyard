---
name: M5 — Data channel bridge inputs done
description: 2026-05-06, shadow_input.c / shadow_controller.c bridged onto the 4 libdatachannel DataChannels through DcInputBridge. Inputs (mouse/keyboard/gamepad) work in the shadow-client GUI without touching the custom sctp.c.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records the libdatachannel path, abandoned 2026-05-06.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

## Architecture

```
Borealis StreamView (mouse/keyboard event)
    │ shadow_input_post_mouse_move(dx, dy)
    ▼
shadow_input.c queue (thread-safe ring buffer)
    │
    ▼ drained at 60 Hz by a thread inside dc_session
shadow_input_drain_queue()
    │ → shadow_input_send_mouse_move(MOCK_SCTP, dx, dy)
    │   → builds FlatBuffer 144b
    │   → sctp_send_msg(MOCK_SCTP, stream_id=1, ppid=53, data, len)
    ▼ (extern "C" symbols dans dc_input_bridge.cpp)
shadow::DcInputBridge::send(stream_id, data, len)
    │ lookup stream_id → shared_ptr<rtc::DataChannel>
    │ buf = rtc::binary; memcpy; dc->send(buf)
    ▼
libdatachannel SCTP/DTLS/SRTP → wire
    ▼
Server Shadow VM
```

## Mapping stream_id → DC label

| stream_id | label              | usage                        |
|-----------|--------------------|------------------------------|
| 1         | shadow-input       | mouse moves, clicks, kbd     |
| 3         | shadow-cursor      | cursor state echo            |
| 5         | shadow-controller  | gamepad announce + inputs    |
| 7         | shadow-clipboard   | clipboard sync               |

## Init handshake (Shadow quirks)

`onOpen` callbacks dans `dc_session.cpp` :
- `shadow-input` opens → `shadow_input_send_connect(MOCK_SCTP)` (sending Connect 96 B + Hello 128 B + PointerEnter 136 B — cf. the memory note `Shadow PointerEnter unlock`) then `shadow_input_set_active_sctp(MOCK_SCTP)` to enable the drain queue.
- `shadow-cursor` ouvre → `shadow_cursor_send_connect(MOCK_SCTP)`.
- `shadow-controller` ouvre → `shadow_controller_send_announce(MOCK_SCTP)` (GamepadPluggedInputV4Model 136b).
- `shadow-clipboard`: no init (the browser does not either).

`MOCK_SCTP` = an opaque non-null pointer, `(sctp_assoc *)0x1`. shadow_input.c treats the pointer as opaque and never dereferences it.

## Fichiers livrables

| Fichier | Action |
|---|---|
| `demo/src/dc/dc_input_bridge.hpp` | NEW — singleton C++ avec API send + register/clear |
| `demo/src/dc/dc_input_bridge.cpp` | NEW — the implementation + extern "C" for `sctp_send_msg`/`sctp_send_msg_redundant`/`sctp_assoc_is_established`/`sctp_open_dc` |
| `demo/src/dc/dc_session.cpp` | EDIT — register the DCs, onOpen handlers, a 60 Hz drain thread |
| `demo/src/dc/sctp_stubs.c` | DELETED — replaced by the real bridge |

## Build verified

```
shadow-client     28.3 MB ✅ (avec inputs fonctionnels)
dc-shadow-cli      4.5 MB ✅
shadow-test-cli    1.7 MB ✅
dc-smoke           3.0 MB ✅
```

## Residual caveats

1. **RX (Shadow's echo) is not wired** — the messages coming from the DCs (a 104 B ack, a 4232 B cursor bitmap) are not dispatched to shadow_input.c's handler. shadow_input.c used a custom callback that is no longer invoked. **Consequence**: no visible cursor update, no clipboard sync. For a viewer + basic inputs that is acceptable. For M6, `dc->onMessage` has to be hooked → forwarding to shadow_input's handler through a new API.

2. **Gamepad**: shadow_controller.c uses SDL gamepad reading (separately). The announcement goes through the bridge. The regular gamepad inputs also go through shadow_controller_send_*.

3. **shadow-controller announce timing**: if the DC opens before the pc state reaches Connected, the send can fail silently (the DC is not "open" yet). The bridge does `if (!dc->isOpen()) return 0` → no crash, just a no-op. No retry is implemented → a potential issue.
