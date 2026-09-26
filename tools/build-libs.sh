#!/usr/bin/env bash
# Requires bash (uses [[ ]]); not POSIX sh.
# build-libs.sh - compile the fetched libraries, per target.
#
# WHY THIS EXISTS, and it is a licence matter as much as a convenience one.
# `tools/bootstrap-libs.sh` fetches each library at a pinned commit and applies
# our patches, but it only ever BUILT jansson-for-Vita. Every other archive -
# wolfSSL for three targets, libopus - was produced by hand, and the recipes
# lived in prose: one of them in `docs/PSVITA_PORT.md`, the other two nowhere.
#
# GPLv3 defines the "Corresponding Source" of a binary as including "the scripts
# used to control compilation and installation". A build invocation that lives
# in a comment does not satisfy that. This file is that script.
#
# THE OPTIONS BELOW ARE RECOVERED, NOT INVENTED. They were read out of the
# `CMakeCache.txt` of the three build trees that produced the archives actually
# linked on 2026-09-13 - the ground truth of how the shipped binaries were made,
# rather than a plausible reconstruction. Where the three targets differ, they
# differ because they were built that way.
#
#   tools/build-libs.sh switch|vita|linux|windows [lib]
#
# `windows` runs from an MSYS2 UCRT64 shell and builds wolfSSL only: the rest
# comes from MSYS2's packages (docs/WINDOWS_BUILD.md).
#
# libopus is built for the Switch alone - the other two targets link the
# SDK's or the system's, verified on their link lines.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="$REPO/third_party"
TARGET="${1:-}"
ONLY="${2:-}"

usage() { echo "usage: $0 switch|vita|linux|windows [wolfssl|libopus|curl|ffmpeg]" >&2; exit 2; }
[ -n "$TARGET" ] || usage

# Options common to all three. wolfSSL's own defaults are NOT relied on: an
# upstream default that flips between two pinned commits would change the
# shipped crypto without a line in any diff.
COMMON=(
  -DBUILD_SHARED_LIBS=OFF
  -DWOLFSSL_EXAMPLES=no -DWOLFSSL_CRYPT_TESTS=no
  -DWOLFSSL_DTLS=yes            # the INPUT channel needs it (:base+12), not audio
  -DWOLFSSL_CHACHA=yes -DWOLFSSL_POLY1305=yes   # the stream cipher of the protocol
  -DWOLFSSL_TLS13=yes -DWOLFSSL_OLD_TLS=no
  -DWOLFSSL_SNI=yes -DWOLFSSL_SUPPORTED_CURVES=yes -DWOLFSSL_EXTENDED_MASTER=yes
  -DWOLFSSL_HARDEN=yes
  -DWOLFSSL_ECC=yes -DWOLFSSL_RSA=yes -DWOLFSSL_SHA512=yes -DWOLFSSL_AESGCM=yes
  -DWOLFSSL_OPENSSLEXTRA=no     # deliberately off: see docs/PSVITA_PORT.md
  -DWOLFSSL_CRL=no -DWOLFSSL_ED25519=no
)

build_wolfssl() {
  local out flags="" extra=()
  case "$TARGET" in
    vita)
      : "${VITASDK:?VITASDK must point at the SDK}"
      out="$LIB/wolfssl/build_vita"
      # CUSTOM_RAND_GENERATE_SEED is not optional here and it cost a whole
      # session: without it wolfSSL keeps `wc_GenerateSeed`, which reads
      # /dev/urandom - a file the Vita does not have - so `wolfSSL_new` returned
      # NULL on every socket that had connected. The forward declaration is
      # force-included because wolfSSL deliberately declares nothing, and an
      # implicit declaration is an ERROR under GCC 15's default C23.
      flags="-ffile-prefix-map=$LIB/wolfssl=wolfssl"
      flags="$flags -DNO_WRITEV -DNO_WOLFSSL_DIR -D_POSIX_THREADS=1 -D_REENTRANT"
      flags="$flags -DCUSTOM_RAND_GENERATE_SEED=switch_rand_seed"
      flags="$flags -include $REPO/patches/wolfssl/vita_seed_decl.h"
      extra=(-DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
             -DWOLFSSL_ALPN=no -DWOLFSSL_CURVE25519=no -DWOLFSSL_KEYGEN=no
             -DWOLFSSL_CERTGEN=no -DWOLFSSL_OCSP=no)
      ;;
    switch)
      out="$LIB/wolfssl/build_switch"
      # SINGLE_THREADED and NO_FILESYSTEM must match what the CONSUMER compiles
      # with, or the headers pull <sys/uio.h> and opendir, which HOS has not.
      flags="-ffile-prefix-map=$LIB/wolfssl=wolfssl"
      flags="$flags -DNO_WRITEV -DSINGLE_THREADED -DNO_WOLFSSL_DIR -DNO_FILESYSTEM"
      flags="$flags -DWOLFSSL_AES_ECB -DHAVE_AES_ECB"
      flags="$flags -DCUSTOM_RAND_GENERATE_SEED=switch_rand_seed"
      flags="$flags -include $LIB/wolfssl_switch_decls.h"
      extra=(-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake
             -DWOLFSSL_ALPN=yes -DWOLFSSL_CURVE25519=yes -DWOLFSSL_KEYGEN=yes
             -DWOLFSSL_CERTGEN=yes -DWOLFSSL_OCSP=yes)
      ;;
    linux)
      out="$LIB/wolfssl/build_linux"
      flags=""
      extra=(-DWOLFSSL_ALPN=yes -DWOLFSSL_CURVE25519=no -DWOLFSSL_KEYGEN=yes
             -DWOLFSSL_CERTGEN=yes -DWOLFSSL_OCSP=no)
      ;;
    windows)
      # MSYS2's own package has `#undef WOLFSSL_DTLS`, and the INPUT channel
      # is DTLS (docs/WINDOWS_BUILD.md, WIN1). These are the options WIN1
      # verified on Windows on 2026-09-10 (TLS 1.3, ChaCha20-Poly1305, ECC,
      # curve25519, SNI, plus DTLS), now applied to the pinned, patched tree
      # every other target uses - they used to be applied to a separate clone
      # of v5.9.1, without our patch. Installed, because CMake finds it
      # through the .pc file (PKG_CONFIG_PATH).
      out="$LIB/wolfssl/build_windows"
      flags=""
      extra=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$out/install"
             -DWOLFSSL_DTLS13=yes -DWOLFSSL_CURVE25519=yes -DWOLFSSL_ED25519=yes
             -DWOLFSSL_ALPN=yes -DWOLFSSL_HKDF=yes)
      ;;
    *) usage ;;
  esac
  [ -d "$LIB/wolfssl" ] || { echo "wolfssl not fetched - run tools/bootstrap-libs.sh" >&2; exit 3; }
  echo "== wolfssl -> $out"
  # A build tree remembers the toolchain of its FIRST configure and silently
  # ignores a different one later. Removing the cache is what makes this script
  # reproduce a build rather than rubber-stamp whatever was already there.
  rm -f "$out/CMakeCache.txt"
  cmake -S "$LIB/wolfssl" -B "$out" -G Ninja \
        "${COMMON[@]}" "${extra[@]}" -DCMAKE_C_FLAGS="$flags" >/dev/null
  cmake --build "$out"
  if [ "$TARGET" = windows ]; then
      cmake --install "$out" >/dev/null
      echo "   installed: $out/install (export PKG_CONFIG_PATH=$out/install/lib/pkgconfig)"
  fi
  # Verified BY SYMBOL, not by the build's exit code: the seed function is the
  # one thing whose absence is silent until a socket fails at runtime. (Linux
  # and Windows use wolfSSL's own system RNG.)
  if [ "$TARGET" = switch ] || [ "$TARGET" = vita ]; then
      local nm="/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
      # `[ ... ] && nm=...` would return 1 on the switch target and `set -e`
      # would abort the function right here. An `if` says the same thing and
      # cannot.
      if [ "$TARGET" = vita ]; then nm="$VITASDK/bin/arm-vita-eabi-nm"; fi
      # NO PIPELINE, and that is the point. `grep -q` exits at the first match,
      # whatever feeds it takes SIGPIPE and returns non-zero, and `set -o
      # pipefail` then fails the pipeline EVEN THOUGH THE SYMBOL WAS FOUND.
      # This check reported a good build as broken three times: once with `nm |
      # grep -q`, once with `printf | grep -q` - moving the producer changes
      # nothing, because the trap is grep -q inside a pipeline. Bash can match
      # the string without a pipe at all.
      local syms; syms="$("$nm" "$out/libwolfssl.a" 2>/dev/null || true)"
      if [[ "$syms" == *"U switch_rand_seed"* ]]; then
          echo "   ok: the archive calls switch_rand_seed (and not wc_GenerateSeed)"
      else
          echo "   FAILED: switch_rand_seed is not referenced - CUSTOM_RAND_GENERATE_SEED" >&2
          echo "   did not take, and wolfSSL will look for /dev/urandom at runtime." >&2
          exit 4
      fi
  fi
}

build_libopus() {
  # ONLY the Switch links our copy. Checked on the real link lines: the Vita
  # takes `-lopus` from the vitasdk and the desktop from pkg-config, so
  # building it for those targets produces a `build_<target>` directory that
  # nothing reads - dead work that looks like a dependency.
  if [ "$TARGET" != switch ]; then
      echo "== libopus: skipped ($TARGET links the SDK's, not ours)"
      return 0
  fi
  local out="$LIB/libopus/build_$TARGET" extra=()
  [ -d "$LIB/libopus" ] || { echo "libopus not fetched - run tools/bootstrap-libs.sh" >&2; exit 3; }
  extra=(-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake)
  echo "== libopus -> $out"
  # `-ffile-prefix-map` normalises the build directory out of the objects.
  # Without it, libopus bakes its absolute source paths into assert strings,
  # and they survive into the shipped `.nro`: 28 of them, each carrying the
  # build machine's username. Nobody downloading a release needs to know the
  # layout of the machine that produced it.
  cmake -S "$LIB/libopus" -B "$out" -G Ninja "${extra[@]}" \
        -DCMAKE_C_FLAGS="-ffile-prefix-map=$LIB/libopus=libopus" \
        -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF >/dev/null
  cmake --build "$out"
}

build_curl() {
  # ONLY the Vita links our copy. The vitasdk's libcurl is built on OpenSSL
  # 1.0.2, statically, and the OpenSSL 1.x licence (its advertising clause) is
  # one the GPL does not allow: with it the .vpk could not be redistributed.
  # This is the vitasdk's own recipe (vitasdk/packages, curl/VITABUILD, 8.17.0
  # pkgrel 2) with ONE change: Mbed TLS 3.6.5, which the vitasdk also ships
  # (Apache-2.0), in place of OpenSSL. Every other option is theirs, so the
  # client sees the same curl it had - same version, same features.
  #
  # Entropy needs nothing from us, unlike wolfSSL's: Mbed TLS reads
  # getentropy(), and the vitasdk's newlib implements that with
  # sceKernelGetRandomNumber (checked on libc.a).
  #
  # The Switch does not need this: its devkitPro libcurl talks to the console's
  # own TLS service (`Curl_ssl_libnx`), with no TLS library linked at all. The
  # desktop uses the system's.
  if [ "$TARGET" != vita ]; then
      echo "== curl: skipped ($TARGET links the SDK's or the system's)"
      return 0
  fi
  : "${VITASDK:?VITASDK must point at the SDK}"
  local out="$LIB/curl/build_vita"
  [ -d "$LIB/curl" ] || { echo "curl not fetched - run tools/bootstrap-libs.sh" >&2; exit 3; }
  echo "== curl -> $out"
  rm -rf "$out"
  cmake -S "$LIB/curl" -B "$out" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$out/install" \
        -DCMAKE_C_FLAGS="-ffile-prefix-map=$LIB/curl=curl" \
        -DBUILD_CURL_EXE=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
        -DENABLE_IPV6=OFF -DCURL_DISABLE_SOCKETPAIR=ON -DHAVE_FCNTL_O_NONBLOCK=OFF \
        -DENABLE_THREADED_RESOLVER=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
        -DENABLE_CURL_MANUAL=OFF -DCURL_CA_BUNDLE="vs0:data/external/cert/CA_LIST.cer" \
        -DCURL_USE_LIBPSL=OFF \
        -DCURL_USE_OPENSSL=OFF -DCURL_USE_MBEDTLS=ON >/dev/null
  # The vitasdk recipe does the same: newlib declares pipe2, the Vita has none.
  sed -i '/HAVE_PIPE2/d' "$out/lib/curl_config.h"
  cmake --build "$out"
  cmake --install "$out" >/dev/null
  # Verified by symbol, as for wolfSSL: the backend is the whole point, and a
  # cmake that quietly fell back to OpenSSL would still "succeed".
  local syms; syms="$("$VITASDK/bin/arm-vita-eabi-nm" "$out/install/lib/libcurl.a" 2>/dev/null || true)"
  if [[ "$syms" == *"U mbedtls_ssl_handshake"* && "$syms" != *"U SSL_connect"* ]]; then
      echo "   ok: libcurl calls Mbed TLS, and not OpenSSL"
  else
      echo "   FAILED: libcurl is not on Mbed TLS (or still references OpenSSL)" >&2
      exit 4
  fi
}

build_ffmpeg() {
  # ONLY the Vita, and only a FLAC decoder. Video is SceAvcdec there; libavcodec
  # serves the FLAC audio path and nothing else (checked on the objects: 20
  # av*/avcodec_* symbols, none from swresample, swscale or avformat).
  #
  # WHY NOT THE SDK'S (2026-09-26). The vitasdk's FFmpeg is LGPL and pulls LAME
  # and mpg123 (LGPL too) into the .vpk - 221 and 161 symbols. Shipping LGPL
  # code statically means shipping ITS exact source, and vdpm serves whatever
  # version is current: 9.0.1 was installed here the day its recipes already
  # said 9.0.2. A release could not name the source of what it contained. This
  # builds the FFmpeg the Switch already pins (tools/pins.env, the release
  # tarball by SHA-256, bundled with every release), flac decoder only: LGPL,
  # exact source, no LAME, no mpg123. Target flags are the vitasdk recipe's.
  if [ "$TARGET" != vita ]; then
      echo "== ffmpeg: skipped (the Switch's is tools/build-ffmpeg-switch.sh; the desktop uses the system's)"
      return 0
  fi
  : "${VITASDK:?VITASDK must point at the SDK}"
  . "$REPO/tools/pins.env"
  local dir="$LIB/ffmpeg" out="$LIB/ffmpeg/build_vita"
  local tarball="$dir/ffmpeg-$FFMPEG_VERSION.tar.xz" src="$dir/src-vita"
  mkdir -p "$dir"
  [ -f "$tarball" ] || curl -sSfL -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
  echo "$FFMPEG_SHA256  $tarball" | sha256sum -c - >/dev/null
  echo "== ffmpeg $FFMPEG_VERSION (flac decoder only) -> $out"
  rm -rf "$src" "$out"; mkdir -p "$src" "$out"
  tar -xJf "$tarball" -C "$src" --strip-components=1
  ( cd "$src"
    ./configure --prefix="$out/install" \
        --enable-cross-compile --cross-prefix="$VITASDK/bin/arm-vita-eabi-" \
        --arch=armv7-a --cpu=cortex-a9 --target-os=none \
        --disable-armv5te --disable-armv6t2 --disable-runtime-cpudetect \
        --disable-shared --enable-static --enable-small --disable-debug \
        --disable-programs --disable-doc --disable-network --disable-autodetect \
        --disable-everything --enable-decoder=flac \
        --disable-avdevice --disable-avformat --disable-avfilter \
        --disable-swscale --disable-swresample --enable-pthreads \
        --extra-cflags="-std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -O2 -ftree-vectorize -fomit-frame-pointer -D_BSD_SOURCE -ffile-prefix-map=$src=ffmpeg" \
        >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null )
  local syms; syms="$("$VITASDK/bin/arm-vita-eabi-nm" "$out/install/lib/libavcodec.a" 2>/dev/null || true)"
  if [[ "$syms" == *"ff_flac_decoder"* ]]; then
      echo "   ok: libavcodec carries the flac decoder"
  else
      echo "   FAILED: no ff_flac_decoder in libavcodec.a" >&2; exit 4
  fi
}

case "$ONLY" in
  wolfssl) build_wolfssl ;;
  libopus) build_libopus ;;
  curl)    build_curl ;;
  ffmpeg)  build_ffmpeg ;;
  "")      build_wolfssl; build_libopus; build_curl; build_ffmpeg ;;
  *)       usage ;;
esac
echo "== done ($TARGET)"
