#!/usr/bin/env bash
# bundle-windows-dlls.sh - make a Windows build directory self-contained.
#
# === WIN2 2026-10-02 - WHY THIS EXISTS ======================================
#
# `halyard.exe` links six MSYS2 DLLs directly (avcodec, avutil, curl, jansson,
# opus, qrencode) and those pull in forty-odd more. They live in
# `/ucrt64/bin`, which is on PATH inside an MSYS2 shell and NOWHERE ELSE. So the
# binary runs when launched from that shell and fails with a wall of
# "DLL not found" boxes when launched from Explorer, from PowerShell, from a
# shortcut - that is, every way a person actually starts it.
#
# `docs/WINDOWS_BUILD.md` has warned about this since §3c-bis. A note in a
# document is not a fix: it only moves the cost onto whoever did not read it.
# Copying the closure next to the exe is the fix, and it is also what a release
# would have to do anyway.
#
# Usage:  tools/bundle-windows-dlls.sh [build_dir] [ucrt64_bin]
#
# Re-runnable: it overwrites only what has changed and says what it did.
set -euo pipefail

BUILD=${1:-build_windows}
UCRT=${2:-/c/msys64/ucrt64/bin}

EXE="$BUILD/halyard.exe"
[ -f "$EXE" ] || { echo "no $EXE - build it first" >&2; exit 1; }
[ -d "$UCRT" ] || { echo "no $UCRT - pass the ucrt64 bin directory as \$2" >&2; exit 1; }

OBJDUMP=$(command -v objdump || true)
[ -n "$OBJDUMP" ] || { echo "objdump not found (pacman -S mingw-w64-ucrt-x86_64-binutils)" >&2; exit 1; }

# The closure, walked breadth-first. A DLL that is NOT in $UCRT is a system one
# (kernel32, the api-ms-win-crt-* stubs, ...) and must NOT be copied: shipping a
# system DLL next to an executable is how one ends up overriding the host's own.
declare -A seen=()
queue=("$EXE")
copied=0 unchanged=0

while [ ${#queue[@]} -gt 0 ]; do
    cur=${queue[0]}
    queue=("${queue[@]:1}")
    # `|| true`: objdump fails on a file that is not a PE image, which is not
    # worth aborting the whole bundle for.
    deps=$("$OBJDUMP" -p "$cur" 2>/dev/null | sed -n 's/.*DLL Name: //p' || true)
    for d in $deps; do
        [ -n "${seen[$d]+x}" ] && continue
        seen[$d]=1
        src="$UCRT/$d"
        [ -f "$src" ] || continue          # a system DLL: leave it to Windows
        dst="$BUILD/$d"
        if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
            unchanged=$((unchanged + 1))
        else
            cp -f "$src" "$dst"
            copied=$((copied + 1))
        fi
        queue+=("$dst")
    done
done

echo "bundle: $copied copied, $unchanged already current, into $BUILD/"
echo "        $EXE can now be started from anywhere, PATH or not."
