#!/usr/bin/env bash
# auto-test.sh - a one-command post-patch wrapper for validating the stream fast.
#
# A4 2026-05-18 : combiner rebuild + run health-check + parse verdict + exit code.
#
# Usage :
#   tools/auto-test.sh                       # health-check 30s, default thresholds
#   tools/auto-test.sh 60                    # health-check 60s
#   tools/auto-test.sh 30 native-stream      # native-stream sans verdict (= juste mesure)
#   tools/auto-test.sh 30 health-check 25    # health-check 30s, threshold-fps=25
#
# Exit codes :
#   0  PASS (= health-check OK ou stream OK)
#   1  bootstrap fail (= login / VM / etc.)
#   2  no VMs / native stream fail
#   3  webrtc fail
#   5  user abort
#   6  health-check FAIL (= threshold non atteint)
#  20  build fail

set -eu

DURATION="${1:-30}"
MODE="${2:-health-check}"
THRESHOLD_FPS="${3:-20}"
THRESHOLD_DECRYPT="${4:-95}"
THRESHOLD_BOTTOM="${5:-30}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build_linux"
TEST_BIN="$BUILD_DIR/halyard-cli"
TS="$(date +%Y%m%d_%H%M%S)"
OUT_JSON="/tmp/auto-test-$TS.json"

# Couleurs si terminal interactif
if [ -t 1 ]; then
    R='\033[0;31m'; G='\033[0;32m'; Y='\033[1;33m'; B='\033[0;36m'; N='\033[0m'
else
    R=''; G=''; Y=''; B=''; N=''
fi

echo -e "${B}== auto-test.sh ==${N}"
echo "  mode      = $MODE"
echo "  duration  = ${DURATION}s"
if [ "$MODE" = "health-check" ]; then
    echo "  fps_min   = $THRESHOLD_FPS"
    echo "  decrypt_min = $THRESHOLD_DECRYPT%"
    echo "  bottom_min  = $THRESHOLD_BOTTOM%"
fi
echo "  output    = $OUT_JSON"
echo ""

# 1. Build
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${R}ERROR: build_linux/ missing, please cmake first${N}" >&2
    exit 20
fi
echo -e "${B}-- Build halyard-cli --${N}"
if ! make -C "$BUILD_DIR" -j"$(nproc)" halyard-cli > /tmp/auto-test-build.log 2>&1; then
    echo -e "${R}BUILD FAIL — see /tmp/auto-test-build.log${N}" >&2
    tail -20 /tmp/auto-test-build.log >&2
    exit 20
fi
echo -e "  ${G}OK${N}"

if [ ! -x "$TEST_BIN" ]; then
    echo -e "${R}ERROR: $TEST_BIN missing after build${N}" >&2
    exit 20
fi

# 2. Run
echo ""
echo -e "${B}-- Run halyard-cli --${N}"
ARGS=(--duration="$DURATION" --output="$OUT_JSON" --mode="$MODE" --quiet)
if [ "$MODE" = "health-check" ]; then
    ARGS+=(--threshold-fps="$THRESHOLD_FPS"
           --threshold-decrypt="$THRESHOLD_DECRYPT"
           --threshold-bottom="$THRESHOLD_BOTTOM")
fi
echo "  $ $TEST_BIN ${ARGS[*]}"
set +e
"$TEST_BIN" "${ARGS[@]}"
RC=$?
set -e
echo ""

# 3. Verdict
if [ ! -f "$OUT_JSON" ]; then
    echo -e "${R}NO OUTPUT JSON — exit=$RC${N}" >&2
    exit "$RC"
fi

case "$MODE" in
    health-check)
        PASS=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('health',{}).get('pass',False))")
        REASON=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('health',{}).get('reason','?'))")
        FPS=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('fps',0))")
        DECRYPT=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('crypto',{}).get('decrypt_pct',0))")
        BOTTOM=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('nal',{}).get('bottom_pct',0))")
        FRAMES=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('frames_decoded',0))")
        SEC=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('session_seconds',0))")
        echo -e "${B}== VERDICT ==${N}"
        echo "  session_seconds = $SEC"
        echo "  frames_decoded  = $FRAMES"
        echo "  fps             = $FPS   (min=$THRESHOLD_FPS)"
        echo "  decrypt %       = $DECRYPT (min=$THRESHOLD_DECRYPT%)"
        echo "  bottom NAL %    = $BOTTOM   (min=$THRESHOLD_BOTTOM%)"
        echo "  exit_code       = $RC"
        echo ""
        if [ "$PASS" = "True" ]; then
            echo -e "  ${G}✅ HEALTH CHECK PASSED${N}"
        else
            echo -e "  ${R}❌ HEALTH CHECK FAILED — reason=$REASON${N}"
        fi
        ;;
    native-stream|native-smoke)
        OK=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('bootstrap_ok',False))")
        FRAMES=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('frames_decoded',0))")
        SEC=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('session_seconds',0))")
        echo -e "${B}== RESULT ==${N}"
        echo "  bootstrap_ok    = $OK"
        echo "  session_seconds = $SEC"
        echo "  frames_decoded  = $FRAMES"
        echo "  exit_code       = $RC"
        ;;
    webrtc)
        STARTED=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('stream_started',False))")
        AVG=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('avg_kbps',0))")
        FRAMES=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('frames_decoded',0))")
        echo -e "${B}== RESULT ==${N}"
        echo "  stream_started  = $STARTED"
        echo "  video avg kbps  = $AVG"
        echo "  frames_decoded  = $FRAMES"
        echo "  exit_code       = $RC"
        ;;
esac
echo ""
echo "  JSON detail: $OUT_JSON"
echo "  Build log  : /tmp/auto-test-build.log"

exit "$RC"
