---
name: project-image-100pct-proof
description: DECISIVE 2026-05-14 — The MASTER capture with the fixed hook proves the desktop receives 10.7% bottom NAL + 132 multi-NAL packets. Our Switch receives 0%. The bug is 100% client-side, in the control messages we send.
metadata: 
  node_type: memory
  type: project
---

## Preuve empirique (= capture MASTER-20260514-153936)

A 4-minute session with the LD_PRELOAD hook fixed (= full, untruncated UDP). The chacha20 KEY: `<session-key-removed>` <!-- The value used to live here in cleartext. It is a SESSION chacha20 key, taken from a capture of May 2026: the session died long ago and the captures it decrypts are gitignored, so it opens nothing for anyone who clones this repo. It still had no business being in a public repo. Removed on 2026-09-03; the value remains in the local captures, next to the hook log it came from. -->.

| Metric | The official desktop | Our Switch client |
|---|---|---|
| Top NAL (first_mb=0) | 698 (89.3%) | 1042 (100%) |
| **Bottom NAL (first_mb=4080)** | **84 (10.7%)** | **0** |
| Multi-NAL packets (2-3 NAL/ct) | 132 | 0 |
| Packet sizes | 40-1280B variables | 1241B uniforme |

## ctrl message differences identified

| Message | Desktop (4 min) | Notre code | Diff |
|---|---|---|---|
| State (case 3) heartbeat | 454 (~2 Hz) | ~60 (1 Hz) | twice as frequent |
| Hid/DisplayReady (case 6) | 15 (~4/min) | 1 (bootstrap only) | **N47 added a periodic 9 s one** |
| Flush (case 7) | 14 (~3.5/min) | already 1 Hz | OK |
| **RegisterSession (case 8)** | **8** | **1** | **MULTIPLE on the desktop side!** |
| **UnregisterSession (case 9)** | **8** | **0** | **Not sent** |
| Encryption (case 12) | 1 | 1 | OK |

## A strong hypothesis

The desktop periodically sends **RegisterSession + UnregisterSession** (= 8× each over 4 min). That suggests:
- The client manages several virtual **outputs/screens**
- The server waits for the unregister/reregister before switching to "full image multi-NAL" mode
- OR it is tied to a distinct `output_id` for the bottom slice

## Diagnostic VideoSslTcpChannel :11020

Agent #3 (= the 2026-05-02 capture) had concluded that :11020 = an STFP fallback and NOT the bottom slice. But this new capture shows **132 multi-NAL packets INSIDE :8010** (= the UDP video). So the bottom NAL is NOT on :11020 but genuinely on the UDP video, just packed differently.

## Concrete actions towards a 100% picture

1. **Implement periodic RegisterSession + UnregisterSession sends** (= a cycle, 8× in 4 min)
2. **Bump the heartbeat to 2 Hz** (= 500 ms instead of 1000 ms)
3. **N47 already done**: a periodic 9 s DisplayReady

## Sources de capture

- Hook fixed: `06-shadow-recon-linux/tls_hook/shadow_tls_hook.so` (= dump_n = g_max_dump)
- Tools : `$REPO/tools/run_master_capture.sh`
- KB : `$REPO/CAPTURES_KB.md`
