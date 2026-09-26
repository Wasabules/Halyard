#!/usr/bin/env bash
# test-fps-activity.sh - measures Shadow's fps UNDER ACTIVITY (= curl, a picture
# that changes) against IDLE (= halyard-cli, a static desktop). Confirms that
# the low fps at idle is VBR encoder frame-skip, not a client cap.
#
# Usage : tools/test-fps-activity.sh [DURATION_S]

set -u
DURATION="${1:-30}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BIN="$REPO_ROOT/build_linux/halyard-cli"

if [ -t 1 ]; then
    G='\033[0;32m'; R='\033[0;31m'; Y='\033[1;33m'; B='\033[0;36m'; N='\033[0m'
else
    G=''; R=''; Y=''; B=''; N=''
fi

echo -e "${B}== test-fps-activity.sh : 2 modes test ==${N}"
echo "  duration  = ${DURATION}s × 2 modes"
echo ""

# Mode IDLE : test-cli headless, desktop Shadow inactif
echo -e "${B}--- IDLE : halyard-cli, desktop Shadow sans interaction ---${N}"
rm -f /tmp/halyard/halyard.log
SHADOW_HWACCEL=0 SHADOW_FPS=120 SHADOW_BITRATE_MBPS=80 \
    timeout $((DURATION + 30)) "$TEST_BIN" \
    --duration="$DURATION" --mode=native-stream --quiet --output=/tmp/test-idle.json \
    > /tmp/test-idle.log 2>&1 || true

python3 << EOF
import json
d = json.load(open('/tmp/test-idle.json'))
sec = max(d['session_seconds'], 1)
print(f"  fps={d['video']['fps']}  Mbps={d['video']['total_bytes']*8/1e6/sec:.2f}")
print(f"  nal_top/s={d['nal']['top']/sec:.1f}  decoded={d['video']['frames_decoded']}")
EOF

echo ""
echo -e "${Y}--- ACTIVITY : USAGE = lance GUI manuellement et fais bouger image ---${N}"
echo "  1. In another terminal:"
echo "     cd build_linux"
echo "     SHADOW_HWACCEL=0 SHADOW_FPS=120 ./halyard"
echo "  2. Create movement on the Shadow desktop (= drag a window, open a video)"
echo "  3. After ${DURATION}s of activity, close it and look at:"
echo "     grep '\[ctrl_session\] stats' /tmp/halyard/halyard.log | tail -3"
echo ""
echo "If the fps rises significantly (= above 50/s) under activity = a VBR encoder is confirmed"
echo "If it stays around 32 = a client-side cap to dig into (GLFW vsync, the pacing queue)"
