#!/usr/bin/env bash
# How much of the core already compiles for the PS Vita?
#
# WHY A SCRIPT AND NOT A NOTE. "Most of it ports" is the kind of claim that ages
# badly and that nobody can check. This compiles every file in `core/` with the
# real Vita toolchain and prints a number plus the exact reason for each miss,
# so the claim is reproducible and its progress is visible.
#
# It does NOT link and it does NOT run: `-fsyntax-only`. Compiling is not
# connecting, and a green line here says a file has no portability blocker --
# not that the Vita will stream.
#
#   tools/vita-survey.sh          # the count and the misses
#   tools/vita-survey.sh -v       # plus the full compiler output for each miss
#
# Needs $VITASDK (see docs/PSVITA_PORT.md, which also has the wolfSSL recipe).
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VITASDK="${VITASDK:-$HOME/vitasdk}"
CC="$VITASDK/bin/arm-vita-eabi-gcc"
V="$VITASDK/arm-vita-eabi"
WOLF="$REPO/third_party/wolfssl"

[ -x "$CC" ] || { echo "no vita toolchain at $CC - see docs/PSVITA_PORT.md"; exit 2; }
[ -f "$WOLF/build_vita/libwolfssl.a" ] || \
  echo "note: wolfSSL is not built for the Vita; 9 files will miss on that alone"

# NO_WRITEV is not optional and not cosmetic: without it wolfSSL's headers pull
# <sys/uio.h>, which the Vita does not have. The Switch needs exactly the same
# flag, and the project notes had already written that down.
INC=(-I"$REPO" -I"$REPO/clients/borealis/include"
     -I"$V/include" -I"$V/include/opus"
     -I"$WOLF" -I"$WOLF/build_vita" -DNO_WRITEV -DNO_WOLFSSL_DIR)

verbose=0; [ "${1:-}" = "-v" ] && verbose=1
ok=0; ko=0; log=$(mktemp); trap 'rm -f "$log"' EXIT

cd "$REPO"
for f in core/*/*.c; do
    if "$CC" -fsyntax-only -O2 "${INC[@]}" "$f" > "$log" 2>&1; then
        ok=$((ok + 1))
    else
        ko=$((ko + 1))
        reason=$(grep -oE 'fatal error: [^:]+' "$log" | head -1 | sed 's/fatal error: //')
        [ -z "$reason" ] && reason=$(grep -oE 'error: .{0,60}' "$log" | head -1)
        printf "  MISS  %-34s %s\n" "${f#core/}" "$reason"
        [ "$verbose" = 1 ] && sed 's/^/        /' "$log"
    fi
done

echo "  ------------------------------------------------------------"
printf "  %d of %d core files compile for arm-vita-eabi\n" "$ok" $((ok + ko))
echo "  (57 of 57 on 2026-09-13 - and the .vpk links; docs/PSVITA_PORT.md)"
