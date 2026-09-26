#!/bin/bash
# test_loop.sh - build + push + (optionally) fetch the logs.
#
# Usage:
#   tools/test_loop.sh build       - build only
#   tools/test_loop.sh push        - push (the Switch must be on hbmenu)
#   tools/test_loop.sh logs        - fetch borealis.log + halyard.log into /tmp
#   tools/test_loop.sh             - build + push + (how to run, and get the logs)
#   tools/test_loop.sh full        - same as with no argument
#   tools/test_loop.sh diff a.json b.json - diff two captures


# The repository root, derived from this script's own location rather than
# hardcoded to one machine's home directory.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
set -e

BUILD_DIR="$REPO/build_switch"
NRO="$BUILD_DIR/halyard.nro"
MOUNT="/run/user/$(id -u)/gvfs/mtp:host=Nintendo_Nintendo_Switch_XTJ10221245951"
SD_DIR="$MOUNT/SD Card"
TARGET_NRO="$SD_DIR/switch/halyard.nro"
TOOLS="$(dirname "$0")"

cmd_build() {
    echo ">> build"
    cd "$BUILD_DIR"
    make -j4 halyard.nro 2>&1 | tail -5
    md5sum "$NRO"
}

cmd_push() {
    if [ ! -d "$SD_DIR" ]; then
        echo "x Switch SD not mounted. Plug the cable in + go back to hbmenu."
        echo "  Mount attendu: $MOUNT"
        exit 1
    fi
    echo ">> push to $TARGET_NRO"
    gio copy "$NRO" "$TARGET_NRO"
    md5sum "$NRO"
}

cmd_logs() {
    if [ ! -d "$SD_DIR" ]; then
        echo "x Switch SD not mounted. Plug the cable in + go back to hbmenu."
        exit 1
    fi
    echo ">> fetch borealis.log + halyard.log → /tmp/"
    gio copy "$SD_DIR/switch/halyard/borealis.log" /tmp/borealis.log 2>&1 || echo "  (no borealis.log)"
    gio copy "$SD_DIR/switch/halyard/halyard.log"   /tmp/halyard.log   2>&1 || echo "  (no halyard.log)"
    if [ -f /tmp/borealis.log ]; then
        echo "--- borealis tail ---"; tail -10 /tmp/borealis.log
    fi
    if [ -f /tmp/halyard.log ]; then
        echo "--- webrtc tail ---"; tail -10 /tmp/halyard.log
    fi
    echo
    echo ">> latest crash reports"
    ls -t "$SD_DIR/atmosphere/crash_reports/" 2>/dev/null | head -3
}

cmd_diff() {
    shift  # "diff"
    python3 "$TOOLS/capture_diff.py" "$@"
}

cmd_decode() {
    shift  # "decode"
    python3 "$TOOLS/fb_decode.py" "$@"
}

cmd_full() {
    cmd_build
    cmd_push
    echo
    echo ">> Start the session on the Switch, then:"
    echo "    $0 logs"
}

case "${1:-full}" in
    build) cmd_build ;;
    push)  cmd_push ;;
    logs)  cmd_logs ;;
    diff)  cmd_diff "$@" ;;
    decode) cmd_decode "$@" ;;
    full|"") cmd_full ;;
    *) echo "Unknown: $1"; exit 1 ;;
esac
