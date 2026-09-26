#!/usr/bin/env bash
# test-quality-params.sh - an A/B test: does the Shadow server honour the
# params bitrate/fps qu'on advertise dans channel announcement chan_idx=0 ?
#
# The method:
#   Run 1 : bitrate=5  Mbps, fps=30
#   Run 2 : bitrate=30 Mbps, fps=60
#   Run 3 : bitrate=80 Mbps, fps=120
#
# If the server honours it, we should see:
#   - the average bandwidth (= total_bytes/duration) ≈ the bitrate requested
#   - the measured fps (= frames_decoded/duration) ≈ the fps requested
#
# Sinon (server-side cap, profil VM, plan offre) → bandwidth/fps invariants.
#
# Usage : tools/test-quality-params.sh [DURATION_S]
#   Default : 30s par run

set -u

DURATION="${1:-30}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build_linux"
TEST_BIN="$BUILD_DIR/halyard-cli"

if [ -t 1 ]; then
    G='\033[0;32m'; R='\033[0;31m'; Y='\033[1;33m'; B='\033[0;36m'; N='\033[0m'
else
    G=''; R=''; Y=''; B=''; N=''
fi

if [ ! -x "$TEST_BIN" ]; then
    echo "$TEST_BIN missing — make halyard-cli first" >&2
    exit 20
fi

echo -e "${B}=== test-quality-params.sh (A/B/C bitrate+fps server-respect) ===${N}"
echo "  duration  = ${DURATION}s × 3 configs"
echo ""

# Config A : low (5 Mbps, 30 fps)
# Config B : mid (30 Mbps, 60 fps)
# Config C : ultra (80 Mbps, 120 fps)
declare -a LABELS=("low" "mid" "ultra")
declare -a BITRATES=("5" "30" "80")
declare -a FPS_REQS=("30" "60" "120")

results=()

for i in 0 1 2; do
    L="${LABELS[$i]}"
    BR="${BITRATES[$i]}"
    FR="${FPS_REQS[$i]}"
    OUT_JSON="/tmp/test-quality-$L.json"

    echo -e "${B}--- Run $L (bitrate=${BR} Mbps, fps=${FR}) ---${N}"
    rm -f /tmp/halyard/halyard.log

    set +e
    SHADOW_HWACCEL=0 \
    SHADOW_BITRATE_MBPS="$BR" \
    SHADOW_FPS="$FR" \
    timeout $((DURATION + 30)) "$TEST_BIN" \
        --duration="$DURATION" --output="$OUT_JSON" --mode="native-stream" --quiet \
        > /tmp/test-quality-$L.log 2>&1
    RC=$?
    set -e

    if [ ! -f "$OUT_JSON" ]; then
        echo -e "  ${R}NO JSON output rc=$RC${N}"
        continue
    fi

    FPS_REAL=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('fps',0))" 2>/dev/null || echo 0)
    KBPS_REAL=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('avg_kbps',0))" 2>/dev/null || echo 0)
    FRAMES=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('frames_decoded',0))" 2>/dev/null || echo 0)
    SEC=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('session_seconds',0))" 2>/dev/null || echo 0)
    PARAM_LINE=$(grep "\[Q1\] channel video params" /tmp/halyard/halyard.log 2>/dev/null | head -1)

    MBPS_REAL=$(python3 -c "print(round($KBPS_REAL/1000, 2))" 2>/dev/null || echo 0)
    echo "  PARAM line: $PARAM_LINE"
    echo "  Measured: fps_real=$FPS_REAL  Mbps_real=$MBPS_REAL  frames=$FRAMES  seconds=$SEC"
    echo ""

    results+=("$L|$BR|$FR|$MBPS_REAL|$FPS_REAL|$FRAMES|$SEC")
done

echo ""
echo -e "${B}=== SUMMARY ===${N}"
printf "  %-6s | %10s | %10s | %12s | %12s\n" "Config" "BR req" "FPS req" "Mbps measured" "fps measured"
echo "  ----------------------------------------------------------------"
for r in "${results[@]}"; do
    IFS='|' read -r L BR FR MBPS_REAL FPS_REAL FRAMES SEC <<< "$r"
    printf "  %-6s | %10s | %10s | %12s | %12s\n" "$L" "$BR" "$FR" "$MBPS_REAL" "$FPS_REAL"
done

echo ""
echo -e "${B}=== VERDICT ===${N}"
# Compare: if Mbps_real varies significantly between low (5) and ultra (80) → the server honours it
# Si varie de < 30% → server ignore (= cap probable)
LOW_MBPS=$(echo "${results[0]}" | cut -d'|' -f4)
ULTRA_MBPS=$(echo "${results[2]}" | cut -d'|' -f4)
LOW_FPS=$(echo "${results[0]}" | cut -d'|' -f5)
ULTRA_FPS=$(echo "${results[2]}" | cut -d'|' -f5)

BR_RATIO=$(python3 -c "print(round($ULTRA_MBPS/max($LOW_MBPS,0.1),2))" 2>/dev/null || echo 0)
FPS_RATIO=$(python3 -c "print(round($ULTRA_FPS/max($LOW_FPS,0.1),2))" 2>/dev/null || echo 0)

echo "  Bitrate ratio ultra/low = ${BR_RATIO}× (attendu: 16× pour respect parfait)"
echo "  FPS ratio ultra/low     = ${FPS_RATIO}× (attendu: 4×  pour respect parfait)"
echo ""

if [ "$(python3 -c "print(1 if $BR_RATIO > 2.0 else 0)")" = "1" ]; then
    echo -e "  ${G}✅ Bitrate: the server HONOURS it (a ratio above 2× was detected)${N}"
else
    echo -e "  ${R}❌ Bitrate : server IGNORE ou CAP (ratio ≤ 2× → plafond actif)${N}"
fi

if [ "$(python3 -c "print(1 if $FPS_RATIO > 1.5 else 0)")" = "1" ]; then
    echo -e "  ${G}✅ FPS: the server HONOURS it (a ratio above 1.5× was detected)${N}"
else
    echo -e "  ${R}❌ FPS : server IGNORE ou CAP (ratio ≤ 1.5× → plafond actif)${N}"
fi
