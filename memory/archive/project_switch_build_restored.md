---
name: project-switch-build-restored
description: 2026-05-14 — build_switch passes again after the __SWITCH__ guards + the DC exclusions. A 22.8 MB NRO ready to push.
metadata: 
  node_type: memory
  type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records a build that broke and was fixed, in May 2026.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.


## Contexte

Before this session, build_switch had been broken since the native pivot (see [[project-pivot-native-protocol]]). Causes:
- `dc/dc_input_bridge.cpp` + `dc/dc_session.cpp` + `dc/nack_sender.cpp` includent `<rtc/rtc.hpp>` (libdatachannel) → pas dispo Switch
- `webrtc/wss.c` uses `pthread_rwlock_*` → libnx does not have it
- `shadow/proximus.c`, `shadow/telemetry.c` includent `<sys/utsname.h>` → pas dispo libnx
- `shadow/qr_helper.c` includes `<qrencode.h>` → libqrencode is not ported to the Switch
- `streaming/ctrl_msgs.c` uses `rand()` without `<stdlib.h>`
- `connecting_activity.cpp` calls `dc_session_run()` even when `SHADOW_NATIVE != 1`

## The fix applied

```cmake
# CMakeLists.txt :
if (PLATFORM_SWITCH)
    file(GLOB DC_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/demo/src/dc/*.cpp)
    list(REMOVE_ITEM MAIN_SRC ${DC_SRCS})
    list(REMOVE_ITEM MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/demo/src/webrtc/wss.c)
endif ()
```

```c
/* proximus.c, telemetry.c : */
#ifndef __SWITCH__
#include <sys/utsname.h>
#endif

/* qr_helper.c : stub return false; */
#ifdef __SWITCH__
bool qr_generate_bmp(...) { return false; }
#else
... real impl ...
#endif

/* ctrl_msgs.c : */
#include <stdlib.h>   /* rand() */
```

```cpp
/* connecting_activity.cpp : */
#ifdef __SWITCH__
bool use_native = true;
#else
const char *native_env = std::getenv("SHADOW_NATIVE");
bool use_native = native_env && native_env[0] == '1';
#endif

#ifndef __SWITCH__
#include "dc/dc_session.h"
#endif

#ifndef __SWITCH__
... dc_session_run() block + sse_ka stop + ws_ok check ...
#endif
```

## Result

- `build_switch/shadow-client.elf` 75MB (= debug symbols + glibc-style)
- `build_switch/shadow-client.nro`, 22.8 MB (= packaged for Atmosphère)
- No Linux regression (= `build_linux/shadow-client`, a 30 MB build, is fine too)

## Left to do

- Push the NRO to the Switch through `gio` (see the `switch-dev` skill)
- Live test: connect to the Shadow VM and see whether the 1920×540 picture comes out on the TV screen
- On a crash: collect the crash_reports + run addr2line against shadow-client.elf

## How to apply

When bumping a new desktop dependency (libdatachannel, libqrencode, etc.), remember to guard it with `#ifdef __SWITCH__` before including the header. The Switch port follows the native pivot (not WebRTC).
