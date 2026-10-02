# The Qt desktop client

A second client for Linux, Windows and macOS, on top of the `halyard-core`
library. The Borealis client stays what it is: the console homebrew, and the
reference for behaviour.

This directory is a **skeleton that compiles and runs**, not a product. It
exists so the risky part is settled before any screen is written.

## Why Qt, decided 2026-10-03

Three constraints of this stack, in the order that mattered.

**The video path carries 187 MB/s.** 1080p NV12 is 3.1 MB per picture, sixty
times a second. That rules out anything crossing a webview IPC bridge —
Tauri, and Flutter's platform channels — because the parade (a native window
beside the webview) gives up the reason for choosing them. Qt accepts a
`QVideoFrame` in `Format_NV12` or `Format_YUV420P` and converts on the GPU, and
`QOpenGLWidget` lets the existing NV12→RGB shader be reused as it is.

**`halyard-core` is C.** Qt is C++: one `extern "C"` and it is done. Tauri,
Flutter and Slint each need an FFI layer to write and maintain, and that layer
would have to carry the threading contract below — which is exactly where a
mistake is expensive.

**Halyard is GPLv3.** Qt's open-source edition is LGPLv3/GPLv3, so it is
compatible. One thing to keep in mind: linking Qt **dynamically** (the default)
carries no LGPL condition; static linking does. This project has already paid
for a licence mismatch once — `.gitattributes` records libcurl-on-OpenSSL-1.x
being dropped on Vita over the advertising clause.

Widgets rather than Quick: the interface is a machine list, a connection
screen, about thirty settings, a video surface and a pause menu. That is forms.
QML would add a language and a runtime to draw checkboxes.

## Qt version

**Floor: Qt 6.8 LTS**, whose standard support runs to 2029-10-08. Built against
whatever the platform ships — MSYS2 UCRT64 currently has 6.11.0, which is a
regular release, not an LTS. The next LTS is 6.12 (one every four minor
versions since 6.8).

So `find_package(Qt6 6.8 …)`: an LTS floor, no pin to a version that falls out
of support in months.

## The three contracts

They are not visible from the headers, and `examples/minimal_client.c` states
them too.

**1. One session per process.** `ctrl_session_glue_run()` takes no handle, and
`ctrl_session_set_bitrate()`, `ctrl_session_request_idr()` and
`ctrl_session_active()` are global. One stream at a time, and no two windows
sharing the library.

**2. Never call core from the Qt event loop.** `ctrl_session_glue_run()` blocks
for the whole session and its callbacks arrive on core's OWN threads — the
frame sink on the decode thread. The planes die with the call. So: a worker
thread, a copy in the sink, and a queued signal towards the UI. That is what
`SessionWorker` is.

**3. Configuration is the environment.** Around 260 `SHADOW_*` variables gate
the streaming path, read with `getenv` at first use and CACHED in statics.
`clients/borealis/settings.cpp::applyToggles()` is the worked example. Most
settings therefore take effect on the NEXT session.

## Build

```bash
cmake -S . -B build_qt -DPLATFORM_DESKTOP=ON -DSHADOW_BUILD_QT=ON
cmake --build build_qt --target halyard-qt
```

On MSYS2 UCRT64, `pacman -S mingw-w64-ucrt-x86_64-qt6-base
mingw-w64-ucrt-x86_64-qt6-multimedia`. On Debian/Ubuntu, `qt6-base-dev
qt6-multimedia-dev`. On macOS, `brew install qt`.

## What is here

| File | What it does |
|---|---|
| `main.cpp` | `QApplication`, the window, and the one place that owns the worker |
| `session_worker.{hpp,cpp}` | the thread that runs a session, and the frame sink that copies out of it |
| `video_widget.{hpp,cpp}` | a `QVideoFrame` surface fed by the worker, YUV→RGB on the GPU |

## What is deliberately NOT here yet

No OAuth screen, no machine list, no settings, no pause menu, no input. The
next step is the measurement the choice of framework hangs on: end-to-end
latency of this path against what the Borealis client does today. If that
number holds, the rest is forms.
