# Windows build (the halyard GUI)

Goal: get `halyard` (the Borealis GUI) compiling on Windows. The binary produced runs the
native Shadow protocol - the same path as on the Switch, and since 2026-09-26 the only one.

State (2026-09-10): **the client compiles, links and starts.** A GLFW window,
OpenGL 4.6, the boot screen displayed, `TINAG OK http=200` — so it is already
talking to the Shadow infrastructure. Binary: 37 MB.

The build had stopped working between 24 May and 9 September, on **four**
independent portability regressions: `setenv`/`unsetenv` missing from the CRT, no
`<poll.h>`, a `setsockopt` that wants a `DWORD` of milliseconds, and a wolfSSL
without DTLS. Fixed under the `WIN1` campaign. Nothing was watching for it — no
loop builds Windows, so the breakage lasted months unseen. That is this target's
real cost: it does not maintain itself.

## Toolchain

**MSYS2 / MinGW-w64 UCRT64.** Not MSVC (the code uses pthread, BSD sockets,
`__attribute__`, C99 designators — porting that to MSVC = days more work with no
added value).

### 1. Install MSYS2

1. <https://www.msys2.org/> → install `msys2-x86_64-*.exe`
2. Launch **MSYS2 UCRT64** (not "MSYS2 MSYS" and not "MINGW64" — UCRT64
   specifically).
3. Update:
   ```bash
   pacman -Syu          # close the window when asked, then relaunch UCRT64
   pacman -Syu          # 2nd pass
   ```

### 2. Install the dependencies

One single line:

```bash
pacman -S --needed \
    base-devel git mingw-w64-ucrt-x86_64-toolchain \
    mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-pkgconf \
    mingw-w64-ucrt-x86_64-wolfssl \
    mingw-w64-ucrt-x86_64-curl \
    mingw-w64-ucrt-x86_64-jansson \
    mingw-w64-ucrt-x86_64-opus \
    mingw-w64-ucrt-x86_64-qrencode \
    mingw-w64-ucrt-x86_64-ffmpeg \
    mingw-w64-ucrt-x86_64-glfw \
    mingw-w64-ucrt-x86_64-glew \
    mingw-w64-ucrt-x86_64-glm \
    mingw-w64-ucrt-x86_64-freetype \
    mingw-w64-ucrt-x86_64-harfbuzz \
    mingw-w64-ucrt-x86_64-fribidi
```

> On the MSYS2 `wolfssl` build: for the ChaCha20/Poly1305 + TLS 1.3 that the
> native protocol requires, the default package has everything needed
> (`HAVE_CHACHA`, `HAVE_POLY1305`, `WOLFSSL_TLS13` enabled as standard). If you
> doubt it: `pacman -Qi mingw-w64-ucrt-x86_64-wolfssl` and inspect the build
> options.

### 3. The vendored libraries

```bash
cd /path/to/halyard
tools/bootstrap-libs.sh          # Borealis, wolfSSL, libopus... at pinned commits, patched
```

`third_party/` is not in git; this fetches it. It is the same command on every
target, and the same one the CI runs.

### 3b. wolfSSL with DTLS — mandatory, once

The `mingw-w64-ucrt-x86_64-wolfssl` package is built **without DTLS**. Verified
three ways: its `options.h` carries `#undef WOLFSSL_DTLS`, the prototypes in
`ssl.h` and `wolfio.h` are behind `#ifdef WOLFSSL_DTLS`, and `libwolfssl.a`
contains no `wolfSSL_set_dtls_fd_connected` symbol.

And the **input** channel — mouse and keyboard — is DTLS. The filename
`ctrl_audio_dtls.c` says "audio", but that premise was refuted in August (§3.37):
`:base+12` carries the input. **So excluding that file from the build would cost
the mouse and the keyboard, not the sound.** You need a wolfSSL that has DTLS.

```bash
tools/build-libs.sh windows wolfssl
```

It builds the pinned, patched wolfSSL fetched above with the options verified on
Windows on 2026-09-10 (TLS 1.3, chacha20-poly1305, ECC, curve25519, SNI, plus
DTLS) and installs it under `third_party/wolfssl/build_windows/install`. Until
2026-09-26 this page cloned a separate wolfSSL (v5.9.1) for it, without the patch
the other targets carry, and the CI used a third recipe; there is one now. The
check, which must return `1`:

```bash
nm -g third_party/wolfssl/build_windows/install/lib/libwolfssl.a | grep -c set_dtls_fd_connected
```

### 4. Build

From MSYS2 **UCRT64**:

```bash
cd /path/to/halyard/.
mkdir build_windows && cd build_windows

# PKG_CONFIG_PATH points at OUR wolfSSL (step 3b) rather than MSYS2's.
# Without it the link fails on wolfSSL_set_dtls_fd_connected and EmbedSendTo, and
# since both packages announce the same version (5.9.1), nothing in cmake's output
# says which one was picked: check WOLFSSL_INCLUDEDIR in CMakeCache.txt, not the
# version number.
export PKG_CONFIG_PATH=$PWD/../third_party/wolfssl/build_windows/install/lib/pkgconfig

# -G "Ninja": faster than MinGW Makefiles. If pacman's ninja is missing:
# drop -G and use mingw32-make instead of ninja.
cmake .. -G "Ninja" -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release

# Build:
ninja halyard     # = halyard.exe
```

Output: `build_windows/halyard.exe` + a `resources/` directory next to it.

For step-by-step debugging in VS Code/CLion, use `-DCMAKE_BUILD_TYPE=Debug` and
`-DSHADOW_WIN_STATIC=OFF` (otherwise winpthreads' debug symbols do not follow the
binary).

### 5. Run

`halyard.exe` looks for and writes its data in `./halyard-data/`
relative to the CWD. The directory is created on the first run
(`ensure_token_dir()` in `oauth.c`). To send the Borealis logs to the console
instead of a file:

```cmd
set SHADOW_LOG_TO_STDERR=1
halyard.exe
```

There is one protocol path and nothing to choose (`SHADOW_NATIVE` is no longer
read: the WebRTC path it selected against was deleted on 2026-09-26).

Settings can also be placed in a file, without going through the environment:
`halyard-data/env.txt`, one `SHADOW_KEY=value` per line. It is the same
mechanism as on console, and it takes precedence over the settings screen.

## What does **not** compile on Windows

`halyard-cli` is built ONLY on Linux: CMake sets `SHADOW_BUILD_TEST_CLI=OFF`
when `WIN32` is detected. Its original blocker (`webrtc/wss.c` and
libdatachannel) went with the WebRTC path on 2026-09-26; whether it now builds
under MinGW has not been tried.

~~Audio playback is also disabled (`HAVE_ALSA=0`, `HAVE_AUDOUT=0`) — Opus decodes
but the PCM goes nowhere. To be implemented through WASAPI if we need it.~~
**[SUPERSEDED on 2026-09-11 (OUT-3)]**: sound now plays through WASAPI
(`media/audio_out_win.{c,h}`, shared event-driven mode, behind ALSA's ring and
playback thread), linked against `ole32` and `avrt`. `SHADOW_WIN_AUDIO=0` restores
the decode-only behaviour of before; `SHADOW_WIN_AUDIO_MMCSS=0` leaves the playback
thread at normal priority. The device opens on the playback thread, and a failed
open leaves decoding alone (`audio: WASAPI indisponible`). Its figures hold only
for that engine. A new `.c` is only seen by `file(GLOB_RECURSE ...)` at
configuration time: re-run cmake in `build_windows` after a pull that adds one. See
KB §9 (OUT-3).

## Known limits (Windows only)

- **MSG_DONTWAIT** != Winsock: replaced by a non-blocking socket through
  `shadow_set_nonblocking()`. If you see a `recv()` that never returns on Windows,
  it means somewhere forces MSG_DONTWAIT = 0 inside a combined flag. Cf.
  `core/services/sockets_compat.h`.
- **The telemetry/proximus body** forces `os-family="Linux"` even on Windows. That
  is deliberate: the Shadow server checks the User-Agent against a whitelist and
  `Linux;x64;App 9.9.10388;...` is what the official desktop client sends. Claiming
  Windows = the server treats the session as a web client and blocks :13011.
- **No historical `clock_gettime(CLOCK_MONOTONIC)` before UCRT** — UCRT64 (our
  target) provides it. If you see a linker error about it, check you really are
  compiling under UCRT64 (not plain MSYS, not legacy MINGW64).

## Checking that it works

Before running the full auth/streaming mode, do a minimal smoke test:

```bash
./halyard.exe --version 2>&1 | head -5
```

It should print the Borealis version. If it segfaults at start-up, check:
1. `ldd halyard.exe` (= MSYS2's cygcheck) — are all the DLLs found?
2. Can `./halyard-data/` be created from the CWD?
3. WSAStartup OK: add `printf("wsa init=%d\n", shadow_sockets_init())` at the top
   of `main()` to confirm.

For the rest (Shadow auth then streaming), it is the same as Linux: the OAuth
device flow → VM selection → connection. Cf. the root `README.md`.

