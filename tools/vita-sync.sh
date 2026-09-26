#!/usr/bin/env bash
# vita-sync.sh - put a fresh build on the Vita's card and hand the card back.
#
# WHY THE DEFAULT IS `eboot`, NOT THE VPK. The .vpk is an INSTALLER: dropping it
# in `ux0:data/downloads/` still costs a VitaShell install, a confirmation, an
# overwrite prompt and a rebuild of the bubble - for 9.4 MB. But the title is
# already installed, and what actually changes between two builds is one file:
# `ux0:app/<TITLE_ID>/eboot.bin`, 5.5 MB, which is exactly `halyard.self`
# under another name. Replacing it is the whole update. `resources/` only moves
# when the resources do, which is rare and is what `--full` is for.
#
# The VPK path is kept for the cases where it IS the right answer: a first
# install, a machine that has never seen the title, or a broken install.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TITLE_ID="${VITA_TITLE_ID:-HLYD00001}"
BUILD="${VITA_BUILD_DIR:-$(cd "$(dirname "$0")/.." && pwd)/build_psv}"

usage() {
    cat <<USAGE
usage: $0 [eboot|vpk|full|companion] [--keep]

  eboot   (default) replace ux0:app/$TITLE_ID/eboot.bin - no VitaShell needed,
          just relaunch the bubble
  vpk     drop halyard.vpk in ux0:data/downloads/ - for a first install
          or a repair; still needs VitaShell to install it
  full    eboot + resources/, when the resources changed

  companion
          install the vitacompanion plugin, so later round trips go over Wi-Fi
          with tools/vita-push.sh and need no cable at all

  --keep  do not unmount afterwards
USAGE
    exit 2
}

# No argument prints the usage and does NOTHING. It used to default to `eboot`,
# which meant that asking what the options were copied a file and UNMOUNTED the
# card as a side effect - a destructive answer to a read-only question.
[ $# -ge 1 ] || usage
MODE="$1"; [ "${MODE#-}" != "$MODE" ] && MODE=eboot
KEEP=0
for a in "$@"; do [ "$a" = "--keep" ] && KEEP=1; done
case "$MODE" in eboot|vpk|full|companion) ;; *) usage ;; esac

# The mount point is discovered, never assumed: the label is the card's, and a
# hard-coded /media path is wrong the first time someone else plugs it in.
MNT=""
for m in $(findmnt -rno TARGET -S /dev/sdb 2>/dev/null) \
         /media/"$USER"/*/ ; do
    [ -d "$m/app/$TITLE_ID" ] && { MNT="${m%/}"; break; }
done
if [ -z "$MNT" ]; then
    echo "no card carrying app/$TITLE_ID is mounted - plug the Vita in USB mode" >&2
    exit 3
fi
DEV="$(findmnt -no SOURCE "$MNT")"
echo "card: $MNT  ($DEV)"

case "$MODE" in
  eboot|full)
    SRC="$BUILD/halyard.self"
    [ -f "$SRC" ] || { echo "missing $SRC - build halyard.self first" >&2; exit 4; }
    DST="$MNT/app/$TITLE_ID/eboot.bin"
    if cmp -s "$SRC" "$DST"; then
        echo "eboot.bin already identical - nothing to send"
    else
        echo "eboot.bin <- $(du -h "$SRC" | cut -f1)"
        cp "$SRC" "$DST"
    fi
    if [ "$MODE" = full ]; then
        # THE SOURCE IS THE TREE, NOT THE BUILD. `CMakeLists.txt` packs the
        # .vpk straight from `${CMAKE_SOURCE_DIR}/resources` (the
        # `PSV_ASSETS_FILES` line): unlike the Switch target, which stages a
        # copy under the build directory, the Vita one never creates
        # `build_psv/resources`. This read that path for an unknown time, so
        # `full` could only ever have failed on `cp` - the resources have never
        # gone out this way. Found 2026-09-14, same class as the i18n net that
        # was walking a directory removed by the tree lift.
        echo "resources/ <- $REPO_ROOT/resources"
        cp -r "$REPO_ROOT/resources/." "$MNT/app/$TITLE_ID/resources/"
    fi
    ;;
  companion)
    SRCDIR="$(cd "$(dirname "$0")/.." && pwd)/third_party/vitacompanion"
    for m in vitacompanion.suprx vitacompanion_kernel.skprx; do
        [ -f "$SRCDIR/$m" ] || { echo "missing $SRCDIR/$m" >&2; exit 4; }
    done
    mkdir -p "$MNT/tai"
    cp "$SRCDIR"/vitacompanion*.s?prx "$MNT/tai/"
    echo "modules -> ux0:tai/"

    # WHERE taiHEN READS ITS CONFIG IS NOT GUESSED. It can be ur0:tai/ (the
    # internal memory, which USB does NOT expose) or ux0:tai/ (this card). So we
    # look: if a config.txt is on the card we can edit it here, and if it is not
    # we say plainly that the last step needs VitaShell rather than writing a
    # file taiHEN will never read and calling it done.
    CFG="$MNT/tai/config.txt"
    LINES_K="ux0:tai/vitacompanion_kernel.skprx"
    LINES_U="ux0:tai/vitacompanion.suprx"
    if [ -f "$CFG" ]; then
        if grep -q vitacompanion "$CFG"; then
            echo "config.txt already names vitacompanion - left alone"
        else
            cp "$CFG" "$CFG.bak"
            awk -v k="$LINES_K" -v u="$LINES_U" '
                { print }
                /^\*KERNEL[[:space:]]*$/ { print k }
                /^\*main[[:space:]]*$/   { print u }
            ' "$CFG.bak" > "$CFG"
            echo "config.txt updated (backup: config.txt.bak)"
        fi
        echo
        echo "REBOOT the console, then: tools/vita-push.sh ip <A.B.C.D> && tools/vita-push.sh ping"
    else
        cat <<MANUAL

The modules are on the card, but taiHEN's config.txt is NOT here - it lives on
ur0:, the internal memory, which USB does not expose. One manual step, once:

  in VitaShell, open  ur0:tai/config.txt  (SELECT = FTP, or edit on device)
  add under *KERNEL :  $LINES_K
  add under *main   :  $LINES_U
  then reboot the console

After that: tools/vita-push.sh ip <A.B.C.D> && tools/vita-push.sh ping
MANUAL
    fi
    ;;

  vpk)
    SRC="$BUILD/halyard.vpk"
    [ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 4; }
    mkdir -p "$MNT/data/downloads"
    echo "downloads/halyard.vpk <- $(du -h "$SRC" | cut -f1)"
    cp "$SRC" "$MNT/data/downloads/halyard.vpk"
    ;;
esac

# `sync` before unmounting, and unmount for real. A card pulled while the FAT
# is still dirty is how an install ends up half-written, and the console then
# reports a corrupt application rather than a copy that never finished.
sync
if [ "$KEEP" = 1 ]; then
    echo "left mounted (--keep)"
else
    if udisksctl unmount -b "$DEV" >/dev/null 2>&1; then
        echo "unmounted - the card can be pulled"
    else
        umount "$MNT" 2>/dev/null && echo "unmounted - the card can be pulled" \
            || echo "could NOT unmount: close anything reading $MNT and retry" >&2
    fi
fi
