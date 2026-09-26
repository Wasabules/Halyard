#!/usr/bin/env bash
# linux-desktop-entry.sh - give a local Linux build its icon in the dock.
#
# The window already carries its icon (_NET_WM_ICON, from resources/icon/
# icon.png), but GNOME's dock - and most others - ignore it: they draw the
# icon of the .desktop entry whose StartupWMClass matches the window's
# WM_CLASS, and show a generic one when nothing matches. Borealis names the
# window, and so its WM_CLASS, after the title: "Halyard".
#
#   tools/linux-desktop-entry.sh              install, for the build in build_linux/
#   tools/linux-desktop-entry.sh <build-dir>  install, for another build directory
#   tools/linux-desktop-entry.sh --remove     undo
#
# Per user, nothing outside ~/.local/share. The entry runs the binary FROM its
# build directory, because a desktop build finds its resources relative to the
# working directory ("./resources/").
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
ENTRY="$DATA/applications/halyard.desktop"
ICON="$DATA/icons/hicolor/256x256/apps/halyard.png"

refresh() {
    command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database -q "$DATA/applications" 2>/dev/null || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 &&
        gtk-update-icon-cache -q -t "$DATA/icons/hicolor" 2>/dev/null || true
}

if [ "${1:-}" = "--remove" ]; then
    rm -f "$ENTRY" "$ICON"
    refresh
    echo "removed $ENTRY and $ICON" >&2
    exit 0
fi

BUILD="$(cd "${1:-$REPO/build_linux}" 2>/dev/null && pwd)" || {
    echo "no build directory: ${1:-$REPO/build_linux}" >&2
    exit 1
}
[ -x "$BUILD/halyard" ] || { echo "no halyard binary in $BUILD - build it first" >&2; exit 1; }
[ -d "$BUILD/resources" ] || { echo "no resources/ in $BUILD - the build copies them there" >&2; exit 1; }

mkdir -p "$(dirname "$ENTRY")" "$(dirname "$ICON")"
install -m 0644 "$REPO/resources/icon/icon.png" "$ICON"

# Exec and Path are quoted by the spec's own rules only when they need it;
# a build path with spaces is rare, but the quoting costs nothing.
cat > "$ENTRY" <<EOF
[Desktop Entry]
Type=Application
Name=Halyard
Comment=Stream a Shadow cloud PC
Exec="$BUILD/halyard"
Path=$BUILD
Icon=halyard
Terminal=false
Categories=Game;
StartupWMClass=Halyard
EOF
refresh
echo "installed $ENTRY -> $BUILD/halyard" >&2
