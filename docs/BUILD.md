# Building Halyard

Two things before any target.

**`third_party/` is empty in a fresh clone, on purpose.** No third-party source
and no compiled archive is redistributed here — git tracks zero of both. The
libraries are fetched at pinned commits and built by two scripts:

```bash
tools/bootstrap-libs.sh          # clone each library at its pinned commit, apply our patches
tools/build-libs.sh <target>     # switch | vita | linux
```

`bootstrap-libs.sh --check` verifies what is present and changes nothing. Our
modifications live in `patches/` as real patches, tracked — which the GPL and
the MPL both require.

**There are no git submodules.** `git clone --recurse-submodules` does nothing
useful here.

---

## The project's identity lives in one place

`CMakeLists.txt` declares it once, near the top:

```cmake
set(VERSION_MAJOR/MINOR/ALTER)     # 0.3.0
set(APP_TITLE       "Halyard")
set(APP_AUTHOR      "Wasabules")
set(APP_URL         "https://github.com/Wasabules/halyard")
set(APP_LICENSE     "GPL-3.0-or-later")
set(APP_DESCRIPTION "Unofficial Shadow PC client for …")
```

Everything downstream is **derived** from it, never retyped:

| Consumer | What it gets |
|---|---|
| `build_id.h` (regenerated every build) | version, build date, git hash **with a `-dirty` suffix**, the exact tag if HEAD sits on one, plus the name, author, URL and licence |
| Settings › About, on the console | all of the above — on a console there is no `--version` to run, so this screen is the only place a licence and a source URL can be read |
| Switch NACP | title, author, version — what the homebrew browser lists |
| PS Vita `param.sfo` | title and `PSN_VERSION`, **computed** from the version numbers (0.3.0 → `00.30`) |
| PS Vita LiveArea bubble | `template.xml`, configured from `template.xml.in` with the title, version and description |

Two of those used to be hand-typed copies and are now computed: `PSN_VERSION`
(typed `00.30` beside a `0.3.0`, so the first bump would have shipped a package
still numbered 00.30) and the LiveArea text. **Edit the generated
`build_psv/sce_sys/livearea/contents/template.xml` and you edit a build
artefact** — change `template.xml.in` instead.

The `-dirty` suffix matters more than it looks: a hash that does not move lies,
and this project has twice tested a stale binary believing otherwise. A build
made from a modified tree now says so on the About screen.

---

## Linux desktop

The development and benchmarking target. Runs the same protocol stack, so most
logic can be exercised without a console.

```bash
# the same list the CI installs (.github/workflows/build-linux.yml)
sudo apt-get install build-essential cmake ninja-build pkg-config git \
    libssl-dev libcurl4-openssl-dev libjansson-dev libopus-dev \
    libqrencode-dev libavcodec-dev libavformat-dev libavutil-dev \
    libswresample-dev libswscale-dev libasound2-dev \
    libgl1-mesa-dev xorg-dev

tools/bootstrap-libs.sh
tools/build-libs.sh linux

cmake -S . -B build_linux -DPLATFORM_DESKTOP=ON
cmake --build build_linux -j$(nproc) --target halyard
./build_linux/halyard
```

The window carries its icon, but GNOME's dock (and most others) only shows the
icon of a `.desktop` entry whose `StartupWMClass` matches the window — without
one, a generic icon. `tools/linux-desktop-entry.sh` installs that entry and the
icon under `~/.local/share` for the build in `build_linux/` (or the directory
given), which also puts Halyard in the application menu; `--remove` undoes it.

The headless benchmark binary is gated behind a flag:

```bash
cmake -S . -B build_linux -DPLATFORM_DESKTOP=ON -DSHADOW_BUILD_TEST_CLI=ON
cmake --build build_linux --target halyard-cli
```

Address sanitiser, for a debug session (about 2× slower):

```bash
cmake -S . -B build_asan -DPLATFORM_DESKTOP=ON -DSHADOW_ASAN=ON
```

---

## Nintendo Switch

**Requires** devkitPro with the Switch toolchain, plus FFmpeg 7.1.5 with the
`nvtegra` hardware decoder. The CI workflow
(`.github/workflows/build-switch.yml`) does all of this and is the executable
reference for the commands below.

### 1. devkitPro

<https://devkitpro.org/wiki/Getting_Started> — install the `switch-dev` group.

### 2. FFmpeg 7.1.5 + nvtegra

Built once, into the devkitPro portlibs, from the official release tarball
(pinned by SHA-256 in `tools/pins.env`) and `patches/ffmpeg/nvtegra-7.1.5.patch`.
This reproduces, byte for byte, the libraries the working `.nro` links:

```bash
sudo -E tools/build-ffmpeg-switch.sh     # or DESTDIR=... to stage it elsewhere
```

Not Averne's newer FFmpeg + Envideo: that `envideo` generation takes ~5 s per
picture on this console and returns empty surfaces (`patches/ffmpeg/README.md`).

### 3. The `.nro`

```bash
tools/bootstrap-libs.sh
tools/build-libs.sh switch

/opt/devkitpro/devkitA64/bin/aarch64-none-elf-cmake -S . -B build_switch -DPLATFORM_SWITCH=ON
cmake --build build_switch -j$(nproc) --target halyard.nro
```

Every target runs the same native protocol stack (`core/protocol/`). The
WebRTC path abandoned in May 2026 was deleted on 2026-09-26, with the
libraries only it needed (libdatachannel, libjuice, libsrtp).

### Deploying and reading logs

```bash
tools/switch-sync.sh push        # send the .nro over FTP (ftpd.nro running)
tools/switch-sync.sh logs        # pull the logs back
tools/switch-logsink.sh listen   # live log over the network, no ftpd needed
```

The log mirror runs **outbound**: the app dials the PC at startup, reading
`host:port` from `logsink.txt` in its data directory. Start the listener
*before* launching the app, or there is no channel for that session.

Crash reports land in Atmosphère's `crash_reports/`; resolve an address with:

```bash
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line \
    -e build_switch/halyard.elf -f -C 0xADDR
```

---

## PS Vita

**Requires** [vitasdk](https://vitasdk.org), with its `mbedtls`, `opus`,
`zstd` and `zlib` packages (the CI's list is in
`.github/workflows/build-psvita.yml`). `tools/build-libs.sh vita` builds
wolfSSL, libcurl on Mbed TLS and a FLAC-only FFmpeg: the SDK's own curl and
FFmpeg are deliberately NOT used - they would put OpenSSL 1.0.2, LAME and
mpg123 in the package (see `THIRD_PARTY_NOTICES.md`).

```bash
export VITASDK=$HOME/vitasdk PATH="$VITASDK/bin:$PATH"

tools/bootstrap-libs.sh
tools/build-libs.sh vita

cmake -S . -B build_psv -G Ninja -DPLATFORM_PSV=ON -DUSE_GXM=ON \
      -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build_psv                    # -> build_psv/halyard.vpk
cmake --build build_psv --target halyard.self   # just the executable
```

`tools/vita-survey.sh` reports how much of the core compiles for
`arm-vita-eabi` and names any regression.

### Deploying

With [vitacompanion](https://github.com/devnoname120/vitacompanion) installed —
it keeps an FTP server and a command channel running in the background, so a
round trip costs one command and no gestures on the console:

```bash
tools/vita-push.sh ip 192.168.1.x    # once
tools/vita-push.sh run               # kill, upload, relaunch
tools/vita-push.sh logs              # pull the logs
tools/vita-push.sh dumps             # pull crash dumps
```

`tools/vita-sync.sh companion` installs the plugin (once, over USB).

### One trap worth knowing

`vita-elf-create` writes its SCE metadata into the gap between the code and data
segments. When the gap is short it prints a message containing **no** `error:`
and then segfaults, leaving the *previous* `.self` in place — which is how three
deployments in a row shipped the wrong build while every step reported success.
The build now checks the gap and fails loudly (`tools/vita-check-gap.py`), and
`vita-push.sh` refuses to send a `.self` older than the ELF it comes from.

---

## Windows

Native path only, MSYS2 UCRT64. See [`WINDOWS_BUILD.md`](WINDOWS_BUILD.md) — in
particular, wolfSSL must be built locally **with DTLS**, because MSYS2's package
has it compiled out and the channel that needs DTLS is the *input* channel.

A Windows checkout can read, edit and reason. It cannot conclude: no console, no
`.nro`, and several test suites do not compile on MinGW. Every measurement in
this project was taken on Linux or on hardware — keep it that way.

---

## Tests

The protocol logic is covered offline: no console, no VM, no network.

```bash
./tests/run_tests.sh             # 51 suites, 139,329 checks
```

Each suite carries the **counter-case** that broke something once — the exact
input that produced the defect — so a failure tells you what you just undid, not
merely that something is wrong.

Two guards run with them: `tools/check-z-formats.py` (the Vita's newlib does not
know `%z`, and an unknown modifier does not consume its argument, so the next
conversion reads the wrong slot) and `verify_i18n.py` (every key the code asks
for exists in the catalogues).

Runtime validation needs a prior GUI login, because the device-grant OAuth
writes the token into the data directory. `docs/TESTS_PLAN.md` has the manual
checklist.

---

## Continuous integration

`.github/workflows/` builds all four targets, runs the tests, lints, and scans
for secrets. `release.yml` publishes the `.nro`, the `.vpk` and the GPL
Corresponding Source on a `v*` tag — behind a job that refuses to publish
anything the project has no right to redistribute
(`tools/check-redistributable.py`, which reads files, not names).
