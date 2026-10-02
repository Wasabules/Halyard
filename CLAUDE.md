# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Halyard is an unofficial homebrew client for the Shadow cloud PC service, on Nintendo Switch (Atmosphère, `.nro`) and PS Vita (HENkaku, `.vpk`). It reimplements Shadow's closed, reverse-engineered streaming protocol. The core is C, the UI is C++ on Borealis (nanovg). Linux and Windows desktop builds exist only as development and test targets.

## Libraries come from scripts, not from git

`third_party/` is empty in a fresh clone, on purpose: no third-party source is redistributed, and there are no submodules. Nothing configures until you run:

```bash
tools/bootstrap-libs.sh          # clone each library at its pinned commit and apply patches/
tools/build-libs.sh linux        # or: switch | vita
tools/bootstrap-libs.sh --check  # verify the vendored fixes are present; changes nothing
```

Changes to a vendored library go in `patches/<lib>/` as a real patch. Editing `third_party/` directly is lost on the next bootstrap.

## Build

```bash
# Linux desktop (the reference platform; apt package list in docs/BUILD.md / build-linux.yml)
cmake -S . -B build_linux -DPLATFORM_DESKTOP=ON [-DSHADOW_BUILD_TEST_CLI=ON] [-DSHADOW_ASAN=ON]
cmake --build build_linux -j$(nproc) --target halyard        # GUI
cmake --build build_linux --target halyard-cli               # headless bench binary (cli/main_test.c)

# Switch (devkitPro + FFmpeg 7.1.5/nvtegra from tools/build-ffmpeg-switch.sh)
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-cmake -S . -B build_switch -DPLATFORM_SWITCH=ON
cmake --build build_switch -j$(nproc) --target halyard.nro

# PS Vita (vitasdk)
cmake -S . -B build_psv -G Ninja -DPLATFORM_PSV=ON -DUSE_GXM=ON -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build_psv                                      # -> halyard.vpk
```

Windows uses MSYS2 UCRT64 only, with a locally built wolfSSL that has DTLS enabled (`docs/WINDOWS_BUILD.md`). The `.github/workflows/build-*.yml` files show the exact commands for each target.

`core/` is a **library target**, `halyard-core`, and the app links it. Sources are still collected with `file(GLOB_RECURSE …)` — `core/*` into the library, `clients/*` into the app — so adding a file needs no CMake edit, only a re-configure. The library is created LATE in `CMakeLists.txt`, just before `program_target`: the per-platform blocks above it are what fill `APP_PLATFORM_INCLUDE/OPTION/LIB`, and a library created earlier gets them empty (which is how `core/media/audio.c` stopped finding `<opus.h>`).

The split is enforced, not hoped for: `tools/check-core-independence.py` refuses any `#include` from `core/` naming `clients/`, borealis, nanovg or glfw, and `cmake -DSHADOW_BUILD_EXAMPLES=ON` builds `examples/minimal_client.c`, which links the library alone. When `core/` genuinely needs something only a client can answer, declare it **weak** — `shadow_link_info` and `halyard_ui_keys_blocked` are the two, and the decoded picture leaves by a registered callback (`ctrl_session_glue_set_frame_sink`) rather than by a link-time symbol named after a Borealis class. After you add a file, re-run the CMake configure step. `clients/borealis/devui/` is excluded from the Switch build. The version comes from the latest `vX.Y.Z` git tag at configure time (`-DHALYARD_VERSION` overrides it). The app identity is declared once near the top of `CMakeLists.txt`, and everything else is derived from it: `build_id.h`, NACP, `param.sfo` and LiveArea. To change the LiveArea text, edit `template.xml.in`, never the generated file.

## Tests and lint

```bash
./tests/run_tests.sh             # offline suite: no console, no VM, no network
```

There is no test runner beyond that script. Each suite is a standalone `test_<module>.c` that `run_tests.sh` compiles with `gcc -Wall -Wextra -Werror -O1` and runs under a 30 s timeout. To run a single suite, copy its `run` line from the script, for example:

```bash
cd tests && gcc -Wall -Wextra -Werror -O1 -o /tmp/t test_vid_wire.c \
  ../core/protocol/vid_wire.c ../core/protocol/proto.c ../core/protocol/msgframe.c ../core/protocol/gamepad_wire.c && /tmp/t
```

- Some suites run a second time with a toggle set to `0` (for example `SHADOW_FLUSH_PREV_INCOMPLETE=0`, `SHADOW_SFX_HANDOFF=0`). The second run asserts the old defective behaviour, as a counter-case. If you add a toggle-reverted fix with a test, follow the same pattern.
- `test_ctrl_msgs`, `test_encryption` and `test_vid_reasm` need `third_party/wolfssl/build_linux/`. Without it they print SKIPPED, which is not a pass.
- Only pure modules can be tested here: no global state, no I/O, no `getenv`. Header-only logic is tested by including the header.

Lint guards (CI `lint.yml`; the test script already runs the first two):
- `tests/verify_i18n.py`: every i18n key the code asks for exists in the catalogues. A missing key renders the key itself, and nothing else catches it.
- `tools/check-z-formats.py`: no `%z` in format strings. Vita's newlib does not consume the argument, so every later conversion reads the wrong slot.
- `tools/check_french.py`: fails on comment blocks that were half-translated from French.
- `tools/client/devlink.py --autotest`, plus `py_compile` and `bash -n` on every tracked script.

## Architecture

**`core/`** (C, shared by every platform)
- `protocol/` holds the wire protocol.
  - `ctrl_session.c` orchestrates a session. `ctrl_session_glue.c` connects its video callback to the decoder and the stream view, and is what the UI calls.
  - Each channel has its own module: `ctrl_tcp`, `ctrl_audio_dtls`, `ctrl_gamepad`, `ctrl_input_tcp`, `ctrl_video_tcp`, `cursor_*`.
  - The wire codecs (`proto.c` protobuf, `vid_wire`/`vid_reasm` for video chunks, `sufp`, `msgframe`, `encryption` ChaCha20-Poly1305) are kept pure so they can be tested.
  - `ctrl_msgs.c` holds the byte-exact message bodies captured from the official client.
- `services/` holds everything around the stream:
  - account and machine access: OAuth device grant (`oauth`), VM start (`launcher`), proximus credentials and SSE (`proximus`), `http`, `tinag`;
  - the log (`journal`, with redaction and masking);
  - settings support: `env_override`, `atomic_file`, `applock`.
- `media/` holds the platform decoders and audio outputs (`h264_decoder` for libavcodec/nvtegra, `*_vita.c` for SceAvcdec and Vita audio, `audio_out_win.c` for WASAPI).
- `input/` maps pads, touch and mouse.

**Session sequence:**
1. OAuth device grant.
2. Start the VM and get its address.
3. Get the service tokens.
4. Open both SSE streams. Without both, the control port never opens.
5. Open the TLS control channel on `port_base+11`: Capabilities → Authenticate → Encryption → RegisterSession, then the channel announcements.
6. Each media channel runs on `port_base+N` with its own transport.

The port map is in `KB.md` §3.37. Earlier notes got three rows wrong, so check there before relying on any port number.

**`clients/borealis/`** (C++ UI)
- Each screen is an `*_activity.cpp` + `*_view.cpp` pair in `activity/`, with headers in `include/activity/`.
- Screens are drawn by the in-house framework in `ui/`, not by Borealis XML. `ui::Screen` makes `draw` final, and subclasses implement `paint(vg, …, t)` with one clock read per frame. The pause menu is a nanovg overlay inside `StreamView`.
- `main.cpp` starts the app in this order: read `env.txt` → `env_override_snapshot()` → `Settings::applyToggles()` (settings become `SHADOW_*` env vars) → `BootActivity`.
- `devlink/` lets a PC drive the UI over the outbound log-sink socket: screenshots, button presses, state. See `tools/client/DEVLINK.md`.
- `device_caps.h` declares `SHADOW_HAS_*` 0/1 macros for each platform capability. Use those instead of `#ifdef __SWITCH__` / `#else`: a subtractive platform condition has broken the Vita port more than once.

**Settings and toggles.** Every setting maps to a `SHADOW_*` environment variable, and roughly 260 toggles gate the streaming path. On consoles they come from `env.txt` in the data directory (`/switch/halyard/`, `ux0:data/halyard/`). `env.txt` takes priority over the settings screen. Some older keys are still French (e.g. `SHADOW_JOURNAL_NIVEAU`); do not rename existing keys.

## House rules

- **Code and comments are in English**, including new code in files that are not yet fully migrated.
- **A behaviour change ships with a `SHADOW_*` toggle that reverts it**, plus a comment saying what was measured to choose the default. Comments explain why, and cite the dated campaign ID (`G43`, `REASM-1`, `2026-09-26`). The reasoning lives next to the code, so match that density.
- **Switch runtime constraints** (from `memory/`):
  - Long-running threads poll their abort flag at ≤100 ms. Otherwise process exit leaks into HOS and only a reboot fixes it.
  - Never hold a mutex across blocking I/O.
  - Don't use `static __thread` in callbacks running on library-created threads.
  - Use libavcodec with `thread_count=1` and never `FF_THREAD_SLICE`.
  - Never keep per-session state in function statics. That has caused several "works only on the first session" bugs.
- **Reproduce on Linux desktop first.** Measurements are taken on Linux or on hardware, never on Windows. On a Windows checkout you can read and edit, but you cannot verify: several suites don't compile on MinGW, and `third_party/` may be empty.
- **Logs, captures and dumps contain live credentials** (JWTs, streaming token, a server-issued private key). Never commit them, and never paste them into an issue (`SECURITY.md`; gitleaks runs in CI). Screenshots come from the desktop build with `SHADOW_DEMO=1` (`tools/showcase-screenshots.sh`).

## Protocol knowledge

- `KB.md` is the single source of truth for the protocol. Each entry carries a confidence tag (`C60`…`C100`) and its source.
  - §9 is a reverse-chronological findings log. Read it from the top: refuted hypotheses are marked, not deleted, so they are not tried a third time.
  - Check a hypothesis against the KB before acting on it.
  - Paths under `captures/`, `dumps/` or `/tmp/…` record where a finding came from. They are not files in this repo.
- `memory/` holds one note per finding, indexed in `memory/MEMORY.md`. Notes in `memory/archive/` describe past states, such as the abandoned WebRTC/libdatachannel path; do not act on them.

## Releases

1. Add a `## [X.Y.Z] - date` section to `CHANGELOG.md`, written for users. `release.yml` refuses a tag without one.
2. Commit, then `git tag -a vX.Y.Z` and push the tag.
3. CI reads the version back out of the NACP, `param.sfo` and LiveArea, and refuses to publish on a mismatch.

Never tag a commit whose message contains `[skip ci]`: GitHub skips the tag push too, and the release never starts. If that happens, run `gh workflow run release.yml --ref vX.Y.Z`.

The website and user guide (`site/`, `docs/INSTALL.md`, `docs/guide/*.md`) are rendered by `tools/build-site.py` into `_site/`. Links between guide pages should point at the `.md` files.
