---
name: Switch homebrew threads created by libjuice do not support TLS (__thread)
description: static __thread on non-libnx-pthread threads → a Data Abort at the TLS offset address (typically 0x20).
type: feedback
---

On Switch (libnx pthread), `static __thread uint8_t buf[N]` works ONLY on threads created directly through pthread_create with a full setup. Threads created internally by libraries such as **libjuice** (its ICE/STUN polling worker thread, created by their own code using the Switch socketpair shim) **have no TLS initialised** → any read/write to `buf` accesses the relative offset (typically 0x10-0x40) with no TLS base, i.e. NULL+offset → a Data Abort.

**Why:** observed on 2026-05-02 on shadow2switch, during optimisation #7 (reducing stack allocation in `ice_recv_dispatcher`). An Atmosphère crash:
```
Type: Data Abort
Fault Address: 0x0000000000000000
PC: shadow-client + 0xe672a8 (memcpy)
X[0] (dst) = 0x20  ← TLS offset, pas TLS+base
```
addr2line resolved it to `memcpy` called from `ice_recv_dispatcher:313` with `static __thread uint8_t copy[1500]`. The equivalent stack allocation (`uint8_t copy[1500]`) works perfectly.

**How to apply:**
- For per-call buffers inside callbacks invoked from libjuice/libsrtp/wolfSSL/etc → **always stack-allocate**, never `__thread`/`thread_local`.
- If you genuinely need a persistent per-thread buffer, use `pthread_key_create` + `pthread_getspecific`/`setspecific` (which work on libnx pthread). But for buffers of 4 KB or less the stack is faster than a TLS lookup anyway.
- On a thread created by our own `pthread_create` (e.g. the main_activity worker), `__thread` works normally — it is libnx that sets TLS up through the libnx `Thread*` wrapper. The problem is specific to threads created "by hand" by certain ported libraries.
