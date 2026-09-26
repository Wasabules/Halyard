---
name: Switch HOS state can degrade between homebrew runs
description: When curl_global_init / nifm / sockets fail cleanly (without a crash), suspect first an HOS state polluted by earlier runs before debugging the code
type: feedback
---
On Switch (Atmosphère + libnx), a homebrew that was working can suddenly fail on system inits — with no crash, no `fatal_report` — just clean `init -> false` results. Observed on `curl_global_init`, which returned CURLE_FAILED_INIT after several consecutive launches of the app, with a binary identical to the one that worked a few minutes earlier.

**Why:** HOS leaks service/socket handles between successive launches of the same NRO. It is likely that `nifm`, `socket` and `csrng` handles are not released cleanly by libnx at applet exit. The more you relaunch the app, the more slots you consume → an init that used to succeed starts failing.

**How to apply:** before diving into a code diff or suspecting a newly linked library (a wolfSSL/ngtcp2 addition breaking curl, say), ask for **a full reboot of the console** and retest. On 2026-05-01 this cost us ~30 min diagnosing a false regression on `curl_global_init` during M7 (adding the QUIC stack) — when a simple Switch reboot fixed everything. Typical symptoms:
- `curl_global_init` / `socketInitialize` / `nifmInitialize` retournent != 0 sans crash
- The pristine pre-regression binary fails too → it must be on the device's runtime side
- No recent `fatal_report` in `/atmosphere/crash_reports/`