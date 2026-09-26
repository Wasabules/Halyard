#!/usr/bin/env bash
# test-N56-holes.sh - iterates on the "first_hole_idx=0" bug to measure the
# effect of the frame_id tracking fix on incomplete NALs / holes in native mode.
#
# Key metrics measured per run:
#   - frames_decoded
#   - nal_top / nal_bottom (= ratio top/bottom)
#   - incompl (= a flush with chunks still missing)
#   - reasm_abandoned (= frames dropped for a SoF mismatch)
#   - HOLES first_hole_idx=0 count (= chunk 0 is systematically missing)
#   - HOLES first_hole_idx > 0 count (= real UDP loss, mid or late)
#
# Usage: tools/test-N56-holes.sh [N_RUNS] [DURATION_S]
#   Default: 3 runs x 30 s
#
# Output: aggregated into /tmp/test-N56-aggregate.txt

set -u

N_RUNS="${1:-3}"
DURATION="${2:-30}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build_linux"
TEST_BIN="$BUILD_DIR/halyard-cli"
WLOG="/tmp/halyard/halyard.log"
AGGREGATE="/tmp/test-N56-aggregate.txt"

# Couleurs
if [ -t 1 ]; then
    R='\033[0;31m'; G='\033[0;32m'; Y='\033[1;33m'; B='\033[0;36m'; N='\033[0m'
else
    R=''; G=''; Y=''; B=''; N=''
fi

if [ ! -x "$TEST_BIN" ]; then
    echo -e "${R}ERROR: $TEST_BIN missing — run make halyard-cli first${N}" >&2
    exit 20
fi

echo -e "${B}=== test-N56-holes.sh ===${N}"
echo "  N runs    = $N_RUNS"
echo "  duration  = ${DURATION}s"
echo "  test bin  = $TEST_BIN"
echo "  log file  = $WLOG"
echo ""

: > "$AGGREGATE"

# Aggregate accumulators
TOTAL_FPS=0
TOTAL_FRAMES=0
TOTAL_TOP=0
TOTAL_BOTTOM=0
TOTAL_INCOMPL=0
TOTAL_ABAND=0
TOTAL_HOLES_AT_0=0
TOTAL_HOLES_GT_0=0
TOTAL_HOLES=0
TOTAL_DECRYPT_PCT=0

for i in $(seq 1 "$N_RUNS"); do
    echo -e "${B}--- Run $i/$N_RUNS (${DURATION}s) ---${N}"
    OUT_JSON="/tmp/test-N56-run$i.json"
    # Reset halyard.log before the run, for clean stats
    rm -f "$WLOG"
    mkdir -p "$(dirname "$WLOG")"

    # Run halyard-cli in native-stream mode
    set +e
    SHADOW_HWACCEL=0 timeout $((DURATION + 30)) "$TEST_BIN" \
        --duration="$DURATION" --output="$OUT_JSON" --mode="native-stream" --quiet \
        > /tmp/test-N56-run$i.log 2>&1
    RC=$?
    set -e

    if [ ! -f "$OUT_JSON" ]; then
        echo -e "  ${R}NO JSON output, rc=$RC, skipping${N}"
        continue
    fi

    # Extract stats from JSON
    FPS=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('fps',0))" 2>/dev/null || echo 0)
    FRAMES=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('video',{}).get('frames_decoded',0))" 2>/dev/null || echo 0)
    TOP=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('nal',{}).get('top',0))" 2>/dev/null || echo 0)
    BOTTOM=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('nal',{}).get('bottom',0))" 2>/dev/null || echo 0)
    ABAND=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('reasm',{}).get('abandoned',0))" 2>/dev/null || echo 0)
    DECRYPT=$(python3 -c "import json; d=json.load(open('$OUT_JSON')); print(d.get('crypto',{}).get('decrypt_pct',0))" 2>/dev/null || echo 0)

    # Extract HOLES from halyard.log
    HOLES_TOTAL=$(grep -c "^HOLES " "$WLOG" 2>/dev/null || echo 0)
    HOLES_AT_0=$(grep "^HOLES.*first_hole_idx=0\b" "$WLOG" 2>/dev/null | wc -l)
    HOLES_GT_0=$(grep "^HOLES" "$WLOG" 2>/dev/null | grep -vE "first_hole_idx=0\b" | wc -l)

    # Extract incompl from latest stats line
    INCOMPL=$(grep "^\[ctrl_session\] stats" "$WLOG" 2>/dev/null | tail -1 | grep -oE "incompl=[0-9]+" | grep -oE "[0-9]+" || echo 0)

    # Clean up the integers (= grep wc -l can return "0\n0" on an empty match)
    HOLES_TOTAL=$(printf "%d" "${HOLES_TOTAL:-0}" 2>/dev/null || echo 0)
    HOLES_AT_0=$(printf "%d" "${HOLES_AT_0:-0}" 2>/dev/null || echo 0)
    HOLES_GT_0=$(printf "%d" "${HOLES_GT_0:-0}" 2>/dev/null || echo 0)
    INCOMPL=$(printf "%d" "${INCOMPL:-0}" 2>/dev/null || echo 0)

    HOLES_AT_0_PCT=$(python3 -c "print(round(100*$HOLES_AT_0/max($HOLES_TOTAL,1), 1))" 2>/dev/null || echo 0)
    BOTTOM_PCT=$(python3 -c "print(round(100*$BOTTOM/max($TOP+$BOTTOM,1), 1))" 2>/dev/null || echo 0)

    echo "  fps=$FPS  frames=$FRAMES  top=$TOP  bottom=$BOTTOM (${BOTTOM_PCT}%)  decrypt=${DECRYPT}%"
    echo "  HOLES total=$HOLES_TOTAL  at_idx=0: $HOLES_AT_0 (${HOLES_AT_0_PCT}%)  at_idx>0: $HOLES_GT_0"
    echo "  incompl=$INCOMPL  reasm_abandoned=$ABAND"

    # Save raw stats line + samples
    {
        echo "=== Run $i (rc=$RC) ==="
        echo "fps=$FPS frames=$FRAMES top=$TOP bottom=$BOTTOM bottom_pct=$BOTTOM_PCT"
        echo "decrypt_pct=$DECRYPT"
        echo "holes_total=$HOLES_TOTAL holes_at_0=$HOLES_AT_0 holes_gt_0=$HOLES_GT_0 holes_at_0_pct=$HOLES_AT_0_PCT"
        echo "incompl=$INCOMPL abandoned=$ABAND"
        echo "--- Sample HOLES lines (first 10) ---"
        grep "^HOLES " "$WLOG" 2>/dev/null | head -10
        echo ""
    } >> "$AGGREGATE"

    TOTAL_FPS=$(python3 -c "print($TOTAL_FPS + $FPS)")
    TOTAL_FRAMES=$((TOTAL_FRAMES + FRAMES))
    TOTAL_TOP=$((TOTAL_TOP + TOP))
    TOTAL_BOTTOM=$((TOTAL_BOTTOM + BOTTOM))
    TOTAL_INCOMPL=$((TOTAL_INCOMPL + INCOMPL))
    TOTAL_ABAND=$((TOTAL_ABAND + ABAND))
    TOTAL_HOLES_AT_0=$((TOTAL_HOLES_AT_0 + HOLES_AT_0))
    TOTAL_HOLES_GT_0=$((TOTAL_HOLES_GT_0 + HOLES_GT_0))
    TOTAL_HOLES=$((TOTAL_HOLES + HOLES_TOTAL))
    TOTAL_DECRYPT_PCT=$(python3 -c "print($TOTAL_DECRYPT_PCT + $DECRYPT)")
    echo ""
done

echo ""
echo -e "${B}=== AGGREGATE (${N_RUNS} runs) ===${N}"
AVG_FPS=$(python3 -c "print(round($TOTAL_FPS / max($N_RUNS,1), 1))")
AVG_FRAMES=$((TOTAL_FRAMES / N_RUNS))
AVG_INCOMPL=$((TOTAL_INCOMPL / N_RUNS))
AVG_ABAND=$((TOTAL_ABAND / N_RUNS))
HOLES_AT_0_PCT_AVG=$(python3 -c "print(round(100*$TOTAL_HOLES_AT_0/max($TOTAL_HOLES,1), 1))")
BOTTOM_PCT_AVG=$(python3 -c "print(round(100*$TOTAL_BOTTOM/max($TOTAL_TOP+$TOTAL_BOTTOM,1), 1))")
AVG_DECRYPT=$(python3 -c "print(round($TOTAL_DECRYPT_PCT / max($N_RUNS,1), 1))")

echo "  avg fps           = $AVG_FPS"
echo "  avg frames        = $AVG_FRAMES"
echo "  total top         = $TOTAL_TOP"
echo "  total bottom      = $TOTAL_BOTTOM (${BOTTOM_PCT_AVG}%)"
echo "  avg decrypt       = ${AVG_DECRYPT}%"
echo "  HOLES total       = $TOTAL_HOLES"
echo "  HOLES at_idx=0    = $TOTAL_HOLES_AT_0 (${HOLES_AT_0_PCT_AVG}%)  ← TARGET: bring it below 5%"
echo "  HOLES at_idx>0    = $TOTAL_HOLES_GT_0  (= vrai UDP loss mid/late)"
echo "  avg incompl       = $AVG_INCOMPL"
echo "  avg abandoned     = $AVG_ABAND  (= frames dropped on a frame_id switch)"
echo ""
echo "  log d'aggregation : $AGGREGATE"

# Verdict
if [ "$TOTAL_HOLES" -eq 0 ]; then
    echo -e "  ${G}✅ No hole detected — the NAL is always complete${N}"
    exit 0
fi
if [ "$(python3 -c "print(1 if $HOLES_AT_0_PCT_AVG < 5 else 0)")" = "1" ]; then
    echo -e "  ${G}✅ first_hole_idx=0 below 5% — the N56 fix worked${N}"
    exit 0
elif [ "$(python3 -c "print(1 if $HOLES_AT_0_PCT_AVG < 30 else 0)")" = "1" ]; then
    echo -e "  ${Y}⚠️  first_hole_idx=0 is reduced but still above 5% — a partial fix${N}"
    exit 1
else
    echo -e "  ${R}❌ first_hole_idx=0 toujours >30% — fix insuffisant${N}"
    exit 2
fi
