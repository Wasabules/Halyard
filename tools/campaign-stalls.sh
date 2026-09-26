#!/usr/bin/env bash
# campagne-coupures - hunts the video stalls (§3.34) with the CLI client, on desktop.
#
# What this measures, and why the CLI is enough: the [D4] detector and the S41c
# key-frame request live in `streaming/ctrl_session.c`, which `halyard-cli`
# uses as much as the GUI does. The CLI's display path is a stub, so NOTHING
# visual is concluded here - but a stall of the UDP stream shows up exactly the
# same way: a packet counter that stops advancing.
#
# THE SOUND IS MUTED (SHADOW_VOLUME=0 zeroes every sample and beats the setting):
# a campaign can run back to back without being unbearable to sit beside.
#
# What to look for in the output:
#   [D4] *** VIDEO SILENT ***   le silence commence
#   [D4] video RESTORED …     and above all its new end of line:
#        "estampille SUFP +N sur M ms" - an N on the scale of the stall means the
#        encoder kept running and the datagrams were lost on the way; an N near
#        zero means the server produced nothing. That is the open question this
#        campaign exists to settle.
#   [S41c] image cle reclamee   the request the client did not send before
#
# Usage :
#   tools/campaign-stalls.sh              # 6 manches de 5 min
#   tools/campaign-stalls.sh 12 600       # 12 manches de 10 min
set -euo pipefail
cd "$(dirname "$0")/.."

MANCHES="${1:-6}"
DUREE="${2:-300}"
BIN=build_linux/halyard-cli
OUT="/tmp/campagne-coupures-$(date +%Y%m%d_%H%M%S)"

[ -x "$BIN" ] || { echo "Binaire absent : $BIN"; echo "  cd build_linux && make halyard-cli"; exit 1; }
if [ ! -s /tmp/halyard/refresh_token ]; then
    echo "No token in /tmp/halyard/refresh_token."
    echo "One login through the interface is needed, just once:"
    echo "  build_linux/halyard"
    echo "(the one gesture that cannot be automated - OAuth is interactive.)"
    exit 1
fi

mkdir -p "$OUT"
echo "Campaign: $MANCHES runs of ${DUREE}s - sound MUTED - output in $OUT"
echo

for i in $(seq 1 "$MANCHES"); do
    log="$OUT/manche_$i.log"
    printf "  manche %d/%d ... " "$i" "$MANCHES"
    # SHADOW_VOLUME=0: real silence. SHADOW_LATENCE=1: the per-stage report.
    SHADOW_VOLUME=0 SHADOW_LATENCE=1 \
        "$BIN" --duration="$DUREE" > "$log" 2>&1 || true
    n=$(grep -ac "VIDEO SILENT" "$log" || true)
    printf "%s coupure(s)\n" "$n"
done

echo
echo "=== COUPURES ==="
grep -ah "VIDEO SILENT\|video RESTORED\|\[S41c\]" "$OUT"/*.log || echo "  no stall across the whole campaign"
echo
echo "=== WHAT THE TIMESTAMP DELTA SAYS ==="
grep -ah "video RESTORED" "$OUT"/*.log \
  | grep -oE "estampille SUFP \+[0-9]+ sur [0-9]+ ms \([^)]*\)" | sort | uniq -c || true
echo
echo "journaux : $OUT"
