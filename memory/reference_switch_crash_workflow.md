---
name: Switch homebrew crash debug workflow
description: Atmosphère writes readable crash reports to the SD card; the addresses resolve with aarch64-none-elf-addr2line against the build's .elf
type: reference
---
When a homebrew crashes on Switch, **Atmosphère writes a text report** into `/atmosphere/crash_reports/<timestamp>_<title_id>.log` on the SD card. For NROs launched through HBL, the `title_id` is the host's (often `010029b0118e8000` = HBL).

Le rapport contient :
- `Result:` (ex `0x4A8 (2168-0002)` = userland Data Abort)
- `Type:` (Data Abort / Undefined / SVC / etc.) + `Address:` (souvent `0x0` = NULL deref)
- Registres complets X[0..28], FP, LR, SP, **PC** (instruction qui crash)
- The stack trace: 8 ReturnAddresses encoded as `<binary> + <offset>`

The resolution workflow:
1. Pull le `.log` via `gio copy "mtp://Nintendo_Nintendo_Switch_XTJ10221245951/SD Card/atmosphere/crash_reports/<file>" /tmp/crash.log`
2. Collect the PC + ReturnAddress[00..N] offsets (the `+ 0xXXXXX` after the module's name)
3. Resolve them:
   ```
   /opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line \
     -e build_switch/shadow-client.elf -f -C -i \
     0x<PC> 0x<RA0> 0x<RA1> ...
   ```
4. addr2line gives the demangled function + file:line for each address.

**Why:** a very good ROI for debugging on Switch. On 2026-05-02 it let us identify in under 2 minutes a Data Abort on `Application::currentFocus->onFocusGained()` (a NULL deref) at `borealis/library/lib/core/application.cpp:529` — the cause being that StreamView was not focusable. Without it we would have wrongly dug into the webrtc thread.

**How to apply:**
- Path complet : `/run/user/1000/gvfs/mtp:host=Nintendo_Nintendo_Switch_XTJ10221245951/SD Card/atmosphere/crash_reports/`
- Fatal reports (more serious panics): `atmosphere/fatal_reports/` — often empty for userland crashes.
- No crash report = a "clean" exit (such as `Application::exit()` being called) or a kernel kill with no Data Abort. If the user says "it crashes" but no new crash report appears → suspect a freeze/deadlock or an HOS state leak (cf. feedback_switch_hos_degraded.md), not a Data Abort.
- The unstripped ELF is in `build_switch/shadow-client.elf`, the stripped NRO in `build_switch/shadow-client.nro`.
