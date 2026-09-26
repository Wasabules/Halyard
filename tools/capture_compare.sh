#!/usr/bin/env bash
# capture_compare.sh - one run that dumps the H.264 stream AND the decoder's
# output pictures, at the same point, to compare live against offline on the SAME
# session. Reproduce the artefacts (animated wallpaper), let it run >40 s, close
# the window.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"; BUILD="$REPO/build_linux"
DATA="/tmp/halyard"; OUT="/tmp/compare"
rm -rf "$OUT"; mkdir -p "$OUT"; rm -f "$DATA"/frame_*.ppm "$DATA/stream.h264" "$BUILD/halyard-data/stream.h264" 2>/dev/null
echo ">>> Watch the animated wallpaper with artefacts, let it run 40s+, close the window."
( cd "$BUILD" && SHADOW_DUMP_H264=1 SHADOW_FRAME_DUMP=1 ./halyard ) >"$OUT/log.txt" 2>&1
for c in "$DATA/stream.h264" "$BUILD/halyard-data/stream.h264"; do [ -s "$c" ] && cp "$c" "$OUT/stream.h264" && break; done
cp "$DATA"/frame_*.ppm "$OUT/" 2>/dev/null
echo "flux: $(stat -c%s "$OUT/stream.h264" 2>/dev/null) o   images live dumpees: $(ls "$OUT"/frame_*.ppm 2>/dev/null | wc -l)"
ls "$OUT"/frame_*.ppm 2>/dev/null | sed 's/^/  /'
echo "Capture prete dans $OUT."
