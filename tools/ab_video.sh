#!/usr/bin/env bash
# ab_video.sh - compares two configurations on the SAME content.
#
# Every artefact measurement made so far was taken on
# recordings of different content: an animated passage scores higher than a
# quiet run, and the comparison proved nothing. With an animated wallpaper
# looping on the VM, the content becomes constant and the A/B becomes
# concluant.
#
#   ab_video.sh 60                          # one reference measurement
#   ab_video.sh 60 SHADOW_EMIT_TRUNCATED=1  # reference PUIS variante, comparees
#
# Each run opens a window, connects on its own (the token is already there),
# records the stream then closes itself.
set -u
DUREE="${1:-60}"; shift || true
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build_linux"
BIN="$BUILD/halyard"
DATA="/tmp/halyard"
[ -x "$BIN" ] || { echo "binaire absent : $BIN"; exit 1; }

passage() {   # $1 = etiquette, $@ = variables d'environnement
    local nom="$1"; shift
    echo "[*] passage « $nom » — ${DUREE}s"
    rm -f "$DATA/stream.h264"
    # from the build directory: the resources (XML, fonts) are loaded by a
    # RELATIVE path, and the client stops dead if it is launched
    # d'ailleurs.
    ( cd "$BUILD" && env SHADOW_DUMP_H264=1 "$@" ./halyard ) \
        >/tmp/ab_$nom.log 2>&1 &
    local pid=$!
    # the stream takes ~15 s to start (VM + bootstrap)
    sleep $((DUREE + 20))
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    sleep 2
    if [ ! -s "$DATA/stream.h264" ]; then
        echo "    no stream recorded - the session never started"
        return 1
    fi
    cp "$DATA/stream.h264" "/tmp/ab_$nom.h264"
    echo "    $(stat -c%s "/tmp/ab_$nom.h264") octets"
    # the session's counters matter as much as the score
    grep -h "ctrl_session\] stats" "$DATA/halyard.log" 2>/dev/null | tail -1 \
        | grep -o "lost=[0-9]*/[0-9]*\|trunc=[0-9]*\|nack=[0-9]*\|abandoned=[0-9]*" \
        | tr '\n' ' '
    echo
    grep -hc "G13. decodeur cale" "$DATA/halyard.log" 2>/dev/null \
        | sed 's/^/    calages du decodeur : /'
    python3 "$REPO/tools/analyze_artifacts.py" "/tmp/ab_$nom.h264" |  head -7 | sed 's/^/    /'
}

passage reference || exit 1
if [ $# -gt 0 ]; then
    echo
    passage variante "$@"
    echo
    echo "=== comparaison ==="
    echo "reference : $(python3 "$REPO/tools/analyze_artifacts.py" /tmp/ab_reference.h264 | sed -n 2p)"
    echo "variante  : $(python3 "$REPO/tools/analyze_artifacts.py" /tmp/ab_variante.h264 | sed -n 2p)"
fi
