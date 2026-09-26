#!/usr/bin/env bash
# showcase-screenshots.sh - the README's and the website's screenshots, from the
# desktop build in demo mode (DEMO-1: fictional account, machines and numbers,
# no request leaves - nothing personal can end up in a picture).
#
#   tools/showcase-screenshots.sh [output-dir]      default: docs/screenshots
#
# Needs a desktop build in build_linux/ and a visible desktop: the app draws in
# a real window, and devlink can only read back a window that is composited
# (tools/client/DEVLINK.md, "the trap that looks like a dead channel"). Leave
# the window alone while it runs, about two minutes.
#
# The stream shots show clients/borealis/branding/demo_stream.jpg when it
# exists, the app's own background otherwise.
#
# What it touches, and puts back: /tmp/halyard/settings.txt, logsink.txt and
# devlink_autorises.txt (the desktop data directory). A devlink listener on
# port 9999 is reused if one runs, started and stopped otherwise.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mkdir -p "${1:-$REPO/docs/screenshots}" && cd "${1:-$REPO/docs/screenshots}" && pwd)"
BIN="$REPO/build_linux/halyard"
DATA=/tmp/halyard
PORT=9999
WORK="$(mktemp -d)"
[ -x "$BIN" ] || { echo "no $BIN - build the desktop client first" >&2; exit 1; }

PICTURE="$REPO/clients/borealis/branding/demo_stream.jpg"
[ -f "$PICTURE" ] || PICTURE=""

dl() { timeout 150 python3 "$REPO/tools/client/devlink.py" --port "$PORT" "$@"; }
screen() { dl state 2>/dev/null | sed -n 's/^screen=\([^ ]*\).*/\1/p'; }
wait_screen() {   # wait_screen <name> [seconds]
    local deadline=$((SECONDS + ${2:-60}))
    until [ "$(screen)" = "$1" ]; do
        [ $SECONDS -lt $deadline ] || { echo "timed out waiting for screen $1" >&2; return 1; }
        sleep 0.5
    done
}
shot() { dl shot "$WORK/$1.png" >/dev/null 2>&1 && echo "  $1" >&2; }
press() { dl "$@" >/dev/null 2>&1 || { echo "failed: $*" >&2; return 1; }; }

# --- the desktop data directory, saved and restored ------------------------
mkdir -p "$DATA" "$WORK/saved"
for f in settings.txt logsink.txt devlink_autorises.txt; do
    [ -e "$DATA/$f" ] && cp -p "$DATA/$f" "$WORK/saved/$f"
done
LISTENER=""
APP=""
cleanup() {
    [ -n "$APP" ] && kill "$APP" 2>/dev/null || true
    [ -n "$LISTENER" ] && kill "$LISTENER" 2>/dev/null || true
    for f in settings.txt logsink.txt devlink_autorises.txt; do
        rm -f "$DATA/$f"
        [ -e "$WORK/saved/$f" ] && cp -p "$WORK/saved/$f" "$DATA/$f"
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

printf '127.0.0.1:%s\n' "$PORT" > "$DATA/logsink.txt"
printf '127.0.0.1:%s\n' "$PORT" > "$DATA/devlink_autorises.txt"
if ! ss -ltn 2>/dev/null | grep -qE ":$PORT\b"; then
    python3 "$REPO/tools/client/devlink.py" --port "$PORT" listen >/dev/null 2>&1 &
    LISTENER=$!
    sleep 1
fi

# settings: English, the performance panel with three sections (perf, network,
# audio - hud_sections is one bit per section in StreamView::setupHud order).
settings() { printf 'language=en-US\n%s' "${1:-}" > "$DATA/settings.txt"; }

run() {   # run <extra env...> - one app instance per scene
    (cd "$REPO/build_linux" && exec env SHADOW_DEMO=1 SHADOW_UI_KEY_LABELS=0 \
        SHADOW_DEVLINK_REDUCTION=1 SHADOW_DEVLINK_LIGNES=256 \
        ${PICTURE:+SHADOW_DEMO_PICTURE="$PICTURE"} "$@" "$BIN" >/dev/null 2>&1) &
    APP=$!
}
stop() { kill "$APP" 2>/dev/null || true; wait "$APP" 2>/dev/null || true; APP=""; sleep 1; }

echo "scene 1: sign-in" >&2
settings
run SHADOW_DEMO_PAIRING=1
sleep 6
shot signin
stop

echo "scene 2: machines, settings, connecting, stream" >&2
settings $'show_perf_stats=1\nhud_sections=69\n'
run
wait_screen liste-vm
sleep 1.5
shot machines
press btn y; wait_screen reglages; sleep 1
shot settings-video
press btn a; wait_screen qualite; sleep 0.5
press nav down; sleep 0.3; press nav down; sleep 1
shot quality
press btn b; wait_screen reglages; sleep 0.5
press btn r; sleep 0.5
press nav down; sleep 0.3; press nav down; sleep 0.3
press btn a; sleep 1.2
shot equaliser
press btn b; wait_screen reglages; sleep 0.5
press btn r; sleep 1
shot settings-controls
press btn b; wait_screen liste-vm; sleep 1
press btn a; sleep 1.8
shot connecting
wait_screen flux
sleep 12
shot stream
stop

echo "scene 3: the pause menu" >&2
settings
run SHADOW_DEMO_MENU_AT_S=2
wait_screen liste-vm
sleep 1
press btn a
wait_screen menu-pause 60
sleep 1
shot pause-menu
stop

echo "converting to WebP in $OUT" >&2
python3 - "$WORK" "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for p in sorted(glob.glob(os.path.join(src, "*.png"))):
    name = os.path.splitext(os.path.basename(p))[0] + ".webp"
    Image.open(p).convert("RGB").save(os.path.join(dst, name), "WEBP", quality=88, method=6)
    print("  %-26s %4d KB" % (name, os.path.getsize(os.path.join(dst, name)) // 1024))
EOF
