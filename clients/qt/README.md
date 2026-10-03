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
./build_qt/halyard-qt --settings --metrics --lang fr   # all three optional
```

Packages: on MSYS2 UCRT64, `qt6-base qt6-multimedia qt6-tools` plus
`qt6-declarative` (MSYS2's `lupdate` links `Qt6Qml.dll` and fails to start
without it - `0xc0000135`, nothing more). On Debian/Ubuntu, `qt6-base-dev
qt6-multimedia-dev qt6-l10n-tools`. On macOS, `brew install qt`.

`--settings` / `--metrics` open those windows at start-up and `--lang <code>`
picks a language for that run without storing it: a window reachable only
through a menu can only be checked by driving the menu.

## Settings

`settings_model.hpp` is the whole settings window as data - one row per
variable, with what core does when it is ABSENT (`def`), read out of each
`getenv` site. The window (`settings_window.cpp`) is generated from it: pages,
sections inside each page, the default preselected and labelled, presets
instead of free numbers, a Reset that REMOVES the variable rather than writing
today's default. `tests/test_qt_settings.cpp` checks the rules and the
table's own invariants.

Choices are saved in an INI file (`QSettings`, `[env]` section) and restored
at the next launch, after `env_override_snapshot()` - so a stored value is
never mistaken for the user's environment - and never over a variable set
outside the application, which always wins.

## Translations

Qt Linguist, nothing home-made:

```bash
cmake --build build_qt --target halyard_lupdate   # extract into clients/qt/i18n/*.ts
# translate in Qt Linguist (or any .ts editor), then just build: lrelease runs
# and the .qm files are embedded under :/i18n
```

- Strings in code: `tr()`. Strings in the settings table:
  `QT_TRANSLATE_NOOP("Settings", ...)`, translated at display time. A `tr()`
  of a VARIABLE is invisible to `lupdate` - use `QT_TR_NOOP` on the array.
- Adding a language is adding its `.ts` to `qt_add_translations` in
  CMakeLists.txt. The selector (Settings > General) lists what is embedded;
  no code names a language.
- The language switches live (`QEvent::LanguageChange`); every window re-sets
  its static texts on that event.
- The default follows the system's ordered preference list
  (`QLocale::uiLanguages()`), and English FIRST stays English even when a
  French catalogue exists - `i18n_match.hpp` explains why, and
  `tests/test_qt_i18n.cpp` pins it. The start-up log says what was chosen
  from what: `i18n: requested '', system [...], catalogues [fr] -> en`.
- Shipped: English (source) and French. Borealis also has Russian and
  Simplified Chinese; they are a `.ts` each away.

## What is here

| File | What it does |
|---|---|
| `main.cpp` | start-up order: environment snapshot, stored settings, language, window |
| `main_window.{hpp,cpp}` | the four states (pairing, machines, connecting, streaming) and three workers |
| `auth_worker`, `bootstrap_worker`, `session_worker` | OAuth, the seven bootstrap steps, the session thread and its frame sink |
| `video_widget.{hpp,cpp}` | a `QVideoFrame` surface fed by the worker, YUV->RGB on the GPU |
| `settings_model.hpp`, `settings_window`, `settings_store` | the settings as data, the generated window, persistence |
| `i18n.{hpp,cpp}`, `i18n_match.hpp`, `i18n/*.ts` | the translation engine, the language choice, the catalogues |
| `theme.{hpp,cpp}` | every colour, spacing and font, derived from the palette; the drawn app mark |
| `about_dialog`, `metrics_window`, `step_list_widget` | identity and build info, live grants, bootstrap progress |
| `ft_worker.{hpp,cpp}`, `file_manager_window.{hpp,cpp}` | the SFTP worker thread and the two-pane file manager |
| `qt_input_map.hpp` | Qt key -> evdev scancode, physical not character (IN1) |
| `core_scope.hpp`, `plane_copy.hpp` | RAII over core's `_free()` functions; the stride-aware plane copy |
| `stream_overlay.{hpp,cpp}`, `overlay_paint`, `hud_model`, `hud_grade.hpp` | the in-stream overlay, its sparklines, what the HUD shows and how a reading becomes a colour |
| `machine_card.{hpp,cpp}`, `machine_state.hpp` | one card per machine; the server's state words mapped to a pill |
| `pairing_widgets.{hpp,cpp}` | the sign-in QR, the code in cells, the validity bar |
| `account_window.{hpp,cpp}` | the plan, the fair-use standing, what the account may do |
| `shortcuts.hpp` | the seven keyboard commands in one table, with collision detection |
| `probe.{hpp,cpp}` | `--probe`, the headless mode that answers a question and exits |
| `rate_meter.hpp` | counters to live rates, without the 1.8e19/s a reset used to produce |

## File transfer

`View > File transfer` (Ctrl+T, live only during a session) opens a two-pane
manager - this PC on the left, the VM on the right - with Send and Receive
between them. It is the answer to FT5: no third-party client can use the SFTP
credential (a 395-byte PEM used as a password, which no GUI box accepts), so
this is the client that can, calling libssh through core rather than pasting a
credential anywhere. The channel and the credential exist only during a
session, so the menu entry greys out and the window closes when the stream
ends; the worker runs on its own thread (the handle is single-threaded and
transfers block), and the credential never leaves core's memory (fetched,
passed to `shadow_ft_open`, zeroed). Needs `-DSHADOW_FILETRANSFER=ON`, which
the Qt desktop build turns on by default (it is the "future desktop client"
`filetransfer.h` was written for). `--files` opens it with no session, to check
the layout.

## Audio output (Windows)

Settings > General > Audio output lists the WASAPI endpoints
(`QMediaDevices`) and writes the chosen one to `SHADOW_WIN_AUDIO_DEVICE`
(AUD-DEV1); core opens it on the next session and falls back to the default if
it is gone. Not a settings-table row, because the device list is discovered at
runtime, but persisted and overridden by env.txt the same way.

## Input and the stream

Keyboard, mouse and wheel are forwarded through core's `shadow_input` queue
(IN1): keys map PHYSICAL, not by character, so a non-US layout types correctly
(`qt_input_map.hpp` explains why and `tests/test_qt_input_map.cpp` pins it);
mouse coordinates map into the decoded resolution, past the letterbox. The
Windows system-shortcut hook (Alt+Tab, Win, Ctrl+Esc, Alt+Esc) forwards those
keys to the VM while the stream has focus (IN2), with `halyard_ui_keys_blocked()`
defined from Qt focus state as the escape hatch. A stream overlay - a hamburger
button, always mouse-reachable - gives Fullscreen / Settings / File transfer /
Disconnect, and F11 toggles fullscreen locally (IN3); both are guaranteed ways
out of a fullscreen stream even with the hook swallowing Alt+Tab.

## Headless: `--probe`

```bash
halyard-qt --probe caps [--vm <id>] [--out report.json]
```

Signs in with the saved session, asks about the account and exits. No window,
so it runs from a script. **It starts no VM**: `/vms/{id}/capabilities`
answers for a machine that is asleep, and starting one would spend the
account's hours to learn nothing more.

It exists because `halyard-cli` is not built on Windows, which would have left
the platform asking a question unable to run the answer. It earned its keep the
first time it ran: that one call found `max_monitor_count` (the display ceiling
the server enforces) and the whole `usage` block (the session countdown the
official client shows), both in a reply the client had been fetching for months
and reading four values out of.

The report is machine metadata and can carry a machine identifier. No token
passes through it.

## What is deliberately NOT here yet

**No microphone forwarding**: KB §3.37 has the mic channel (`+32`) identified
but its format UNPROVEN - the official client opens the socket and sends no
bytes - so implementing it would be shipping unmeasured protocol, which the
house rules forbid.

**One screen.** DISP1 established that the protocol is per-display (an
`outputId` on each DisplayConfig, a ceiling the server enforces, a second
screen being a second channel registration) and that this account's machine
allows two. What is missing is the protobuf FIELD NUMBER of `outputId`,
inferred at f6 from the generated class layout and not read: the binary
carries no `.proto` descriptor. Asking for a second screen on a guess would be
exactly the unmeasured protocol the mic is being held back for.

**The SSE stream is read but quiet.** SSE1 parses the taxonomy and surfaces
`bsod` and `get-out`; expect nothing else. The RE measured one event in a
235-second session and refuted the idea that the stream drives anything
mid-session, so the machine cards still have no live run state - `/vms`
answers `status: null` and that remains the whole story.

The framework-latency measurement still stands: end-to-end latency of this path
against what the Borealis client does today.
