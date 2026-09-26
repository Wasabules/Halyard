---
name: project-bug2-decoder-race-fix
description: SEGV on closing the window - the decoder is freed on the main thread while a libdatachannel worker is still draining pending messages into it.
metadata:
  node_type: memory
  type: project
---

# BUG2 - decoder teardown races the worker draining its queue

DECISIVE 2026-05-18. A SEGV when closing the desktop window.

## Symptom

```
ERROR: AddressSanitizer: SEGV on unknown address 0x000000000008 (T43 RTC worker)
    #3 av_hwframe_transfer_data (libavutil)
    #5 flush_access_unit (h264_decoder.c)
    #7 h264_decoder_feed_rtp (dc_session.cpp)
    #12 rtc::impl::Track::flushPendingMessages
```

## Root cause

A cleanup racing a libdatachannel thread-pool worker:

1. The main thread calls `finalize()` when the window closes.
2. `finalize()` calls `pc_->close()`.
3. **But** `close()` does not guarantee the pool's workers have finished
   draining their pending messages.
4. The main thread carries on and frees the decoder, hardware device context
   included.
5. The worker is still inside `flushPendingMessages`, calls our callback, and
   reaches `av_hwframe_transfer_data` with a context that has just been freed.

## Fix

A mutex around the two ends, and a double check inside the callback:

- a `decoder_mtx` in the session state;
- each `onMessage` lambda tests the pointer, takes the lock, tests it **again**,
  and feeds inside the lock;
- `finalize()` takes the same lock before destroying the decoders and nulling
  the pointers.

The lock is contended only at cleanup, once per session. On the receive path it
is uncontended - about 30 ns.

## Does the native path have the same race?

Probably not: `core/protocol/ctrl_session.c` owns its threads and polls an
abort flag every 100 ms ([[feedback_switch_thread_lifecycle]]). But the pattern
applies anywhere a decoder freed on one thread can still be reached from a
worker - `ctrl_session_glue.c`'s cleanup is worth auditing on that basis.

## Refs

- [[feedback_wss_mutex_deadlock]] - the same class of race around blocking I/O
- [[project_runtime_issues_2026-05-18]]
- [[project_HW1_hwaccel_rc5_fix]]
