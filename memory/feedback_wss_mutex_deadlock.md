---
name: WSS deadlock mutex famine — fix obligatoire
description: dc_session.cpp held wss_mtx_ across the whole of wss_recv (a 5 s SO_RCVTIMEO), blocking send_ws indefinitely. A pattern to avoid on any long-running I/O.
type: feedback
---
A bug from 2026-05-06 (the M4 GUI migration): `dc_session.cpp` used a single `wss_mtx_` for both TX and RX. `ws_recv_loop` held the lock across `wss_recv` (which blocks until SO_RCVTIMEO=5 s). During that time `send_ws()` starved → `send_token_msg()` never returned → the whole session stuck at `M8.dc`.

**Symptom in the logs**:
- `wss_open OK` in webrtc.log
- Repeated server PING/PONGs (the recv loop works)
- NO `wss TX text` at all (send_ws blocked) → the server never receives our token/offer
- The app stays on "Waiting for video..." indefinitely then crashes silently

**Why**: a single `std::mutex` for both TX and RX of a blocking transport forces total serialisation. With `wss_recv` able to hold for 5 s, the sender queues for the full timeout window on every cycle.

**How to apply** :
- On any blocking transport (WSS, HTTP, a bare socket), TX and RX must have SEPARATE mutexes, OR the RX must happen without a lock (a single reader thread, no read race).
- wolfSSL is not thread-safe for concurrent writes → a mutex INTERNAL to the transport (`wss_session.write_mtx`) to synchronise PONG (from recv) and send_text (from main).
- **NEVER** hold an application mutex across a blocking call (recv/read).

**Fix applied**:
- `dc_session.cpp`: `wss_mtx_` removed from `ws_recv_loop` (a single reader is fine), a new `wss_send_mtx_` for TX only.
- `wss.c`: a `pthread_mutex_t write_mtx` added to `wss_session`, lock/unlock around every `send_frame` (PONG + send_text + CLOSE).

A healthy pattern, to be respected throughout this project for any blocking I/O.
