#!/usr/bin/env bash
# find_divergence.sh - finds the EXACT picture where the live decoding parts
# company with the offline one, on the same session. The stream is identical (the
# same bytes fed); if the hashes diverge, it is the live decoding/display that
# corrupts.
#
# Start it, reproduce the artefacts (animated wallpaper) for at least 60-90 s, close the window.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"; BUILD="$REPO/build_linux"
DATA="/tmp/halyard"; OUT="/tmp/diverge"
rm -rf "$OUT"; mkdir -p "$OUT"
rm -f "$DATA/stream.h264" "$BUILD/halyard-data/stream.h264" "$DATA/halyard.log" 2>/dev/null
echo ">>> Watch the animated wallpaper with artefacts, let it run 60-90 s, close the window."
( cd "$BUILD" && SHADOW_DUMP_H264=1 SHADOW_FRAME_HASH=1 ./halyard ) >"$OUT/run.log" 2>&1
for c in "$DATA/stream.h264" "$BUILD/halyard-data/stream.h264"; do [ -s "$c" ] && cp "$c" "$OUT/stream.h264" && break; done
cp "$DATA/halyard.log" "$OUT/halyard.log" 2>/dev/null
echo "flux capture : $(stat -c%s "$OUT/stream.h264" 2>/dev/null) o"
echo "empreintes live : $(grep -c '\[HASH\]' "$OUT/halyard.log" 2>/dev/null)"
echo "[*] offline decoding of the same stream (CUDA)..."
SHADOW_HWACCEL=1 /tmp/decode_offline "$OUT/stream.h264" "$OUT/offline.csv" 2>"$OUT/dec.log"
tail -2 "$OUT/dec.log"
echo "[*] comparaison des empreintes..."
python3 "$REPO/tools/compare_hash.py" "$OUT/halyard.log" "$OUT/offline.csv"
