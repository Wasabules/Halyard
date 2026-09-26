#!/usr/bin/env bash
# test-suite.sh — halyard's automated v1 validation
#
# Runs every non-visual test (= headless + log checks). For the tests
# visuels GUI, voir TESTS_PLAN.md sections 1.1, 2.x, 3.x, 4.x.
#
# Usage: tools/test-suite.sh [quick|full]
#   quick = juste smoke test (= ~90s)
#   full  = + checks token obfuscation + log redaction + cursor dump (~5 min)

set -u

MODE="${1:-quick}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build_linux"
TEST_CLI="$BUILD_DIR/halyard-cli"

if [ -t 1 ]; then
    G='\033[0;32m'; R='\033[0;31m'; Y='\033[1;33m'; B='\033[0;36m'; N='\033[0m'
else
    G=''; R=''; Y=''; B=''; N=''
fi

PASS=0
FAIL=0
WARN=0

check() {
    local name="$1"; local cond="$2"
    if [ "$cond" = "ok" ]; then
        echo -e "  ${G}✅ PASS${N} $name"
        PASS=$((PASS+1))
    elif [ "$cond" = "warn" ]; then
        echo -e "  ${Y}⚠️  WARN${N} $name"
        WARN=$((WARN+1))
    else
        echo -e "  ${R}❌ FAIL${N} $name"
        FAIL=$((FAIL+1))
    fi
}

echo -e "${B}=== test-suite.sh v1 ($MODE) ===${N}"
echo ""

# === 1. Build verification ===
echo -e "${B}--- 1. Build verification ---${N}"
if [ ! -d "$BUILD_DIR" ]; then
    check "build_linux dir exists" fail
    echo "Run: cmake .. -DPLATFORM_DESKTOP=ON from build_linux"
    exit 20
fi
check "build_linux exists" ok
cd "$BUILD_DIR"
if make -j$(nproc) halyard-cli halyard > /tmp/build.log 2>&1; then
    check "make halyard-cli + halyard" ok
else
    check "make halyard-cli + halyard" fail
    tail -10 /tmp/build.log
    exit 20
fi

# === 2. Smoke test native stream ===
echo ""
echo -e "${B}--- 2. Smoke test 30s native-stream ---${N}"
cd "$REPO_ROOT"
if timeout 100 ./tools/auto-test.sh 30 native-stream > /tmp/test-smoke.log 2>&1; then
    check "auto-test exit 0" ok
else
    check "auto-test exit 0" fail
    tail -10 /tmp/test-smoke.log
fi

LATEST=$(ls -t /tmp/auto-test-*.json 2>/dev/null | head -1)
if [ -z "$LATEST" ]; then
    check "JSON output found" fail
else
    check "JSON output found" ok
    BOOTSTRAP=$(python3 -c "import json; d=json.load(open('$LATEST')); print(d.get('bootstrap_ok', False))")
    FPS=$(python3 -c "import json; d=json.load(open('$LATEST')); print(d['video']['fps'])")
    BOTTOM=$(python3 -c "import json; d=json.load(open('$LATEST')); print(d['nal']['bottom_pct'])")
    DECRYPT=$(python3 -c "import json; d=json.load(open('$LATEST')); print(d['crypto']['decrypt_pct'])")
    FRAMES=$(python3 -c "import json; d=json.load(open('$LATEST')); print(d['video']['frames_decoded'])")
    echo "  Stats: bootstrap=$BOOTSTRAP fps=$FPS bottom=$BOTTOM% decrypt=$DECRYPT% frames=$FRAMES"
    if [ "$BOOTSTRAP" = "True" ]; then check "bootstrap_ok" ok; else check "bootstrap_ok" fail; fi
    if python3 -c "import sys; sys.exit(0 if $FPS >= 20 else 1)"; then check "fps >= 20" ok; else check "fps >= 20" warn; fi
    if python3 -c "import sys; sys.exit(0 if $BOTTOM >= 45 else 1)"; then check "bottom >= 45%" ok; else check "bottom >= 45%" warn; fi
    if python3 -c "import sys; sys.exit(0 if $DECRYPT >= 99 else 1)"; then check "decrypt >= 99%" ok; else check "decrypt >= 99%" warn; fi
fi

# === 3. Native channels active ===
echo ""
echo -e "${B}--- 3. Native channels logs ---${N}"
WLOG="/tmp/halyard/halyard.log"
if [ ! -f "$WLOG" ]; then
    check "halyard.log exists" fail
else
    check "halyard.log exists" ok
    # VST (= V16 + RE11)
    if grep -q "vst: TLS handshake OK" "$WLOG"; then check "VST :base+20 handshake OK" ok; else check "VST :base+20 handshake OK" fail; fi
    if grep -q "vst: 3x 0x64 trigger sent" "$WLOG"; then check "VST trigger 0x64 sent" ok; else check "VST trigger 0x64 sent" fail; fi
    if grep -q "vst: heartbeat 0x70 sent" "$WLOG"; then check "VST heartbeat 0x70" ok; else check "VST heartbeat 0x70" warn; fi
    # Input (= I1 + RE5)
    if grep -q "\[input-tcp\] TLS handshake OK" "$WLOG"; then check "Input :base+14 handshake OK" ok; else check "Input :base+14 handshake OK" fail; fi
    if grep -q "\[input-tcp\] Connect 96B sent" "$WLOG"; then check "Input Connect 96B sent" ok; else check "Input Connect 96B sent" fail; fi
    # Cursor (= CUR1)
    if grep -q "\[cursor\] init mode=" "$WLOG"; then check "Cursor state init" ok; else check "Cursor state init" fail; fi
    if grep -q "\[cursor\] bitmap update" "$WLOG"; then check "Cursor bitmap received" ok; else check "Cursor bitmap received" warn; fi
    if grep -q "\[cursor\] pos update" "$WLOG"; then check "Cursor position tracking" ok; else check "Cursor position tracking" warn; fi
fi

# === 4. Security: token obfuscation ===
echo ""
echo -e "${B}--- 4. Security: token obfuscation (UX3 B1) ---${N}"
TOKEN_FILE="/tmp/halyard/refresh_token"
if [ ! -f "$TOKEN_FILE" ]; then
    check "refresh_token file exists" warn
else
    MAGIC=$(head -c 1 "$TOKEN_FILE" | xxd -p)
    if [ "$MAGIC" = "52" ]; then
        check "Token magic byte 0x52 (= v2 obfuscated)" ok
    else
        check "Token v1 plaintext (legacy)" warn
        echo "    -> On the next save, it will be auto-migrated to v2"
    fi
    # Test back-compat read : load + check non-empty
    SIZE=$(stat -c%s "$TOKEN_FILE")
    if [ "$SIZE" -gt 50 ]; then check "Token size reasonable (>50B)" ok; else check "Token size suspect" warn; fi
fi

# === 5. Log redaction (UX7 B11) ===
echo ""
echo -e "${B}--- 5. Log redaction (UX7 B11) ---${N}"
if [ -f "$WLOG" ]; then
    # Anti-leak check. Use < redirect (= force single file even if WLOG had glob).
    JWT_LEAK=$(grep -c "Bearer ey[A-Za-z0-9._-]" < "$WLOG" 2>/dev/null || echo 0)
    JWT_REDACTED=$(grep -c "Bearer \[JWT-REDACTED\]" < "$WLOG" 2>/dev/null || echo 0)
    REFRESH_LEAK=$(grep -cE "refresh_token=[A-Za-z0-9.]" < "$WLOG" 2>/dev/null || echo 0)
    REFRESH_REDACTED=$(grep -c "refresh_token=\[REFRESH-REDACTED\]" < "$WLOG" 2>/dev/null || echo 0)

    # Force a single int (= `grep -c` on an empty match can return "0\n" → strip it)
    JWT_LEAK=$(printf "%d" "$JWT_LEAK" 2>/dev/null || echo 0)
    JWT_REDACTED=$(printf "%d" "$JWT_REDACTED" 2>/dev/null || echo 0)
    REFRESH_LEAK=$(printf "%d" "$REFRESH_LEAK" 2>/dev/null || echo 0)
    REFRESH_REDACTED=$(printf "%d" "$REFRESH_REDACTED" 2>/dev/null || echo 0)

    if [ "$JWT_LEAK" -eq 0 ]; then
        check "No JWT plaintext leak" ok
    else
        check "JWT leak detected: $JWT_LEAK occurrences" fail
    fi
    if [ "$REFRESH_LEAK" -eq 0 ]; then
        check "No refresh_token plaintext leak" ok
    else
        check "refresh_token leak: $REFRESH_LEAK occurrences" fail
    fi
    echo "    → Redacted JWT: $JWT_REDACTED, Redacted refresh: $REFRESH_REDACTED"
fi

# === 6. Quality settings test (Q1 env vars) ===
if [ "$MODE" = "full" ]; then
    echo ""
    echo -e "${B}--- 6. Quality params (Q1: bitrate=80, fps=60) ---${N}"
    sleep 10
    SHADOW_BITRATE_MBPS=80 SHADOW_FPS=60 timeout 60 ./tools/auto-test.sh 20 native-stream > /tmp/test-q1.log 2>&1
    if grep -q "channel video params: 1920x1080 @ 60.0 fps @ 80.0 Mbps" "$WLOG"; then
        check "Q1 bitrate=80 + fps=60 applied" ok
    else
        check "Q1 params not applied as expected" warn
    fi
fi

# === 7. Cursor dump (= CUR1 phase 2) ===
if [ "$MODE" = "full" ]; then
    echo ""
    echo -e "${B}--- 7. Cursor dump CUR1 ---${N}"
    sleep 10
    rm -f /tmp/cursor_*.bin
    SHADOW_DUMP_CURSOR=1 timeout 50 ./tools/auto-test.sh 15 native-stream > /tmp/test-cursor.log 2>&1
    DUMPED=$(ls /tmp/cursor_*.bin 2>/dev/null | wc -l)
    if [ "$DUMPED" -gt 0 ]; then
        check "Cursor frames dumped ($DUMPED files)" ok
        echo "    → /tmp/cursor_*.bin for offline format analysis"
    else
        check "No cursor frames dumped" warn
    fi
fi

# === Summary ===
echo ""
echo -e "${B}=== SUMMARY ===${N}"
echo -e "  ${G}PASS${N}: $PASS"
echo -e "  ${Y}WARN${N}: $WARN"
echo -e "  ${R}FAIL${N}: $FAIL"
TOTAL=$((PASS + WARN + FAIL))
if [ "$TOTAL" -eq 0 ]; then exit 1; fi
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "${G}✅ The headless tests pass — now move on to the visual tests, TESTS_PLAN.md sections 1-4${N}"
    exit 0
else
    echo -e "${R}❌ $FAIL test(s) failed — read the logs before the visual tests${N}"
    exit 1
fi
