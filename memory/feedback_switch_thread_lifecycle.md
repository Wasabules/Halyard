---
name: Switch homebrew threads must exit cleanly before main returns
description: On Switch, a thread still alive when main() returns = a force-killed process = a corrupted HOS (a reboot is then mandatory). Every long-running thread must poll an abort flag.
type: feedback
---
Background threads that run on a long `nanosleep`/`select` must **poll an abort flag at a fine granularity** (~100 ms max) and break as soon as it is set. Otherwise, when the user quits the app through `Application::exit()`, `main()` returns, the process terminates, and any thread still alive is force-killed by the kernel — its sockets, mutexes and HOS handles leak → the console stays in a degraded state that forces a reboot.

**Why:** observed on 2026-05-02 on shadow2switch. The M13 streaming loop slept `nanosleep(1s)` × 60 iterations = 60 s. When the user pressed `+` to quit at t=18 s, `~MainActivity` set `alive=false` but the webrtc thread never saw it (blocked in nanosleep). Process exit → the thread brutally killed → sockets/SRTP/wolfSSL state left inside HOS → "it crashes/hangs" on the next run, reboot mandatory. Cf. also feedback_switch_hos_degraded.md (the failing `*_init`s are the symptom of that leak).

**How to apply:**
- The pattern to follow: `params.abort_flag` = a `volatile int *` polled by the loop. The C++ owner stores a `std::shared_ptr<volatile int>` in the Activity, sets it to 1 in the destructor, and captures it by value in the async thread's lambda (the shared_ptr outlives the Activity's destruction).
- Any long `nanosleep` loop: 100 ms granularity at most, check abort_flag on every round.
- For libraries we do not control (`wolfSSL_accept`, `juice_get_state`): a short timeout + retry, never a blocking wait of several seconds.
- Never rely on `Application::exit()` to stop native threads cleanly — all it does is leave the Borealis main loop.
