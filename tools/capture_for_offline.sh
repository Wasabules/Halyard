#!/usr/bin/env bash
# capture_for_offline.sh - records everything needed to diagnose the
# artefacts HORS LIGNE, sans nouvelle session live.
#
# Start it, THEN reproduce the artefacts on screen (watch a video, move the
# pause it) for ~40 s, THEN close the Shadow window.
#
# Produit /tmp/offline/ :
#   stream.h264   the exact stream fed to the decoder (the heart of the diagnosis)
#   diag.log      journal complet (reassemblage, IDR, erreurs libavcodec, stats)
#   manifest.txt  resume (duree, images, pertes, erreurs)
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build_linux"
DATA="/tmp/halyard"
OUT="/tmp/offline"
[ -x "$BUILD/halyard" ] || { echo "binaire absent — compiler d'abord"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
rm -f "$DATA/stream.h264" "$BUILD/halyard-data/stream.h264" 2>/dev/null

echo "==============================================================="
echo "  Get going, WATCH A VIDEO with artefacts, put it in"
echo "  PAUSE for a few seconds, then CLOSE the Shadow window."
echo "  (Ctrl+C here does NOTHING - close the client's window.)"
echo "==============================================================="
( cd "$BUILD" && SHADOW_DUMP_H264=1 SHADOW_DIAG_REASM=1 \
    ./halyard ) 2>&1 | tee "$OUT/diag.log"

# the dump can land in the data dir OR in build/halyard-data
for c in "$DATA/stream.h264" "$BUILD/halyard-data/stream.h264"; do
    [ -s "$c" ] && cp "$c" "$OUT/stream.h264" && break
done

{
    echo "=== manifest capture $(date) ==="
    if [ -s "$OUT/stream.h264" ]; then
        echo "flux H.264 : $(stat -c%s "$OUT/stream.h264") octets"
    else
        echo "NO H.264 stream recorded - the session produced no video"
    fi
    echo "--- derniere ligne de stats ---"
    grep "ctrl_session\] stats" "$OUT/diag.log" | tail -1
    echo "--- recuperations G18 (derive) ---"
    grep -c "G18. derive detectee" "$OUT/diag.log" 2>/dev/null | sed 's/^/IDR sur derive : /'
    echo "--- images incompletes [EMIT] ---"
    grep -c "EMIT.*INCOMPLETE" "$OUT/diag.log" 2>/dev/null | sed 's/^/total : /'
} | tee "$OUT/manifest.txt"
echo
echo "The capture is ready in $OUT — I can now finish offline."
