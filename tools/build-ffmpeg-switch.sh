#!/usr/bin/env bash
# build-ffmpeg-switch.sh - the Switch's FFmpeg: 7.1.5 + the nvtegra patch.
#
# Reproduces, byte for byte, the libav* the Switch .nro is linked against
# (proven 2026-09-26, patches/ffmpeg/README.md). The CI runs it; so can you.
#
#   tools/build-ffmpeg-switch.sh               # build and install into portlibs
#   DESTDIR=/tmp/stage tools/build-ffmpeg-switch.sh   # stage the install instead
#
# THE PREFIX IS PART OF THE OUTPUT. `configure` records its whole command line
# in the libraries (avutil_configuration()), so a different --prefix gives
# different bytes. Keep it; use DESTDIR to install somewhere else.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$REPO/tools/pins.env"
: "${DEVKITPRO:=/opt/devkitpro}"
export PATH="$DEVKITPRO/devkitA64/bin:$PATH"
WORK="${FFMPEG_WORK:-$(mktemp -d)}"

tarball="$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz"
if [ ! -f "$tarball" ]; then
    curl -sSfL -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
fi
echo "$FFMPEG_SHA256  $tarball" | sha256sum -c -

rm -rf "$WORK/ffmpeg-$FFMPEG_VERSION"
tar -xJf "$tarball" -C "$WORK"
cd "$WORK/ffmpeg-$FFMPEG_VERSION"
patch -p1 -s < "$REPO/patches/ffmpeg/nvtegra-$FFMPEG_VERSION.patch"

# Recorded from the build that produced the working .nro - do not "tidy" it.
./configure --prefix=/opt/devkitpro/portlibs/switch --enable-gpl --disable-shared --enable-static \
    --cross-prefix=aarch64-none-elf- --enable-cross-compile --arch=aarch64 --cpu=cortex-a57 \
    --target-os=horizon --enable-pic \
    --extra-cflags='-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -I/opt/devkitpro/libnx/include -I/opt/devkitpro/portlibs/switch/include' \
    --extra-ldflags='-fPIE -L/opt/devkitpro/portlibs/switch/lib -L/opt/devkitpro/libnx/lib' \
    --disable-runtime-cpudetect --disable-programs --disable-debug --disable-doc --enable-asm \
    --enable-neon --disable-autodetect --disable-avdevice --disable-encoders --disable-muxers \
    --disable-demuxers --disable-filters --disable-bsfs --disable-devices --disable-network \
    --enable-swscale --enable-swresample --enable-nvtegra
make -j"$(nproc)"
make install ${DESTDIR:+DESTDIR="$DESTDIR"}
echo "FFmpeg $FFMPEG_VERSION + nvtegra installed${DESTDIR:+ under $DESTDIR}"
