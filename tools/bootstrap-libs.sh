#!/usr/bin/env bash
# Fetches the vendored libraries and puts the local fixes back.
#
# WHY THIS EXISTS. `third_party/` is gitignored, so a
# fresh clone of this repository has NO libraries at all and cannot build a
# single target. Worse, the fixes we carry on top of them lived nowhere a
# machine could read: Borealis' seven were `.extract` SNIPPETS meant to be
# re-pasted by hand, and wolfSSL's were not written down at all.
# Two of the seven had already been lost that way -- `patches/borealis/apply.sh`
# reported them MISSING for months (K20, restored 2026-09-12).
#
# So: pinned commits, real patches, one command.
#
# It FETCHES and PATCHES. It does not compile, except jansson for the Vita
# (see the note at that step) - `tools/build-libs.sh <target>` builds the rest,
# with the options recovered from the trees that produced the shipped archives.
#
#   tools/bootstrap-libs.sh            # everything
#   tools/bootstrap-libs.sh borealis   # just one
#   tools/bootstrap-libs.sh --check    # verify what is there, change nothing
#
# The commits below are the ones the working tree was ACTUALLY built from on
# 2026-09-12, not the tags nearest to them. Borealis' had to be recovered by
# comparing file contents against upstream -- the copy carried no git metadata,
# which is the same reason its fixes went missing.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="$REPO/third_party"
PAT="$REPO/patches"

#   name            url                                                    commit
LIBS=(
  # 2026-09-26 - the pin read `5000a1e5`, a commit that does not exist upstream
  # (the real one is 5000a1e4..., a typo), so a fresh clone could not build at
  # all. It was not the right base either: comparing every file's git hash
  # against the 1310 upstream commits, the tree we ship matches 3ecf2de -
  # with its submodules - on all 2878 files except the nine carrying our local
  # fixes. `local-fixes.patch` was regenerated against it and proven to
  # reproduce those nine byte for byte. A FULL SHA now: a short one is exactly
  # how the typo went unnoticed.
  "borealis|https://github.com/xfangfang/borealis.git|3ecf2de10226392ecb071c470bea9758a24cd6b1|$PAT/borealis/local-fixes.patch"
  "wolfssl|https://github.com/wolfSSL/wolfssl.git|8541142a100ba2b0c45548cb0f503b31d475f036|$PAT/wolfssl/grease.patch"
  "libopus|https://github.com/xiph/opus.git|ddbe48383984d56acd9e1ab6a090c54ca6b735a6|"
  # 2026-09-26 - libjuice, libdatachannel and libsrtp left with the WebRTC
  # path, and ngtcp2 with the QUIC trial before it: nothing compiled them.
  # PS Vita only: the SDK packages jansson with -fPIC, and `vita-elf-create`
  # rejects the GOT-relative relocations that produces ("Invalid relocation type
  # 25"). Rebuilt here without it. Harmless on every other target, which take
  # jansson from pkg-config or portlibs and never look at this checkout.
  "jansson|https://github.com/akheron/jansson.git|v2.14|"
  # PS Vita only: libcurl rebuilt on Mbed TLS. The vitasdk's is built on
  # OpenSSL 1.0.2, whose licence the GPL does not allow, so the .vpk could not
  # be redistributed while it linked it. Same version as the SDK's (8.17.0, the
  # commit of tag curl-8_17_0), same options - only the TLS backend changes.
  # Built by tools/build-libs.sh vita.
  "curl|https://github.com/curl/curl.git|400fffa90f30c7a2dc762fa33009d24851bd2016|"
)
# These carry submodules of their own, and the build needs them. (None since
# 2026-09-26; the mechanism stays for the next one.)
RECURSIVE=""
# Borealis carries SDL and GLFW as submodules. Only GLFW is needed - the
# desktop build uses it, and `devlink` takes `stb_image_write.h` from its
# `deps/` on EVERY target, the consoles included. SDL is never built here and
# weighs more than everything else combined. Found 2026-09-26 by building from a
# fresh export: the Switch build failed on that header, because this list did
# not name Borealis at all - every local checkout had the submodules from an
# earlier, manual init, so no machine that had ever built noticed.
SUBMODULES_ONLY="borealis:library/lib/extern/glfw"

check_only=0
[ "${1:-}" = "--check" ] && { check_only=1; shift; }
want="${1:-}"
rc=0

for entry in "${LIBS[@]}"; do
    IFS='|' read -r name url commit patch <<< "$entry"
    [ -n "$want" ] && [ "$want" != "$name" ] && continue
    dir="$LIB/$name"

    if [ "$check_only" = 1 ]; then
        if [ ! -d "$dir" ]; then echo "  ABSENT   $name"; rc=1; continue; fi
        if [ -z "$patch" ]; then echo "  ok       $name"; continue; fi
        # `git apply --check --reverse` only means something in a real
        # repository. Borealis arrives as a plain copy AND sits inside this
        # repo's work tree, where git resolves the paths against the wrong
        # root -- so it is checked the way `patches/borealis/apply.sh` checks
        # it, by looking for the markers. Same question, an answer that holds.
        if [ -d "$dir/.git" ]; then
            if git -C "$dir" apply --check --reverse "$patch" 2>/dev/null; then
                echo "  ok       $name"
            else
                echo "  NO PATCH $name  (the local fixes are not in place)"; rc=1
            fi
        elif grep -rqs "LOCAL FIX halyard" "$dir"; then
            echo "  ok       $name  (markers found; see patches/borealis/apply.sh for the detail)"
        else
            echo "  NO PATCH $name  (no LOCAL FIX marker anywhere)"; rc=1
        fi
        continue
    fi

    if [ ! -d "$dir/.git" ]; then
        # === CLONE FIRST, SWAP AFTER =====================================
        #
        # This used to move the existing copy aside and THEN clone. On
        # 2026-09-14 the clone failed on a dropped connection and the checkout
        # was left with NO borealis at all - the build broken by the very
        # script meant to repair it. A step that destroys before it has
        # something to put in place is a step that fails badly instead of
        # failing safely.
        #
        # The copy is still moved aside rather than deleted: it may hold a fix
        # nobody wrote down, which is exactly how this repo lost two of them.
        echo "== $name: cloning"
        tmp="$dir.incoming-$$"
        rm -rf "$tmp"
        if ! git clone -q "$url" "$tmp"; then
            rm -rf "$tmp"
            echo "   clone FAILED - $name left exactly as it was" >&2
            exit 5
        fi
        [ -d "$dir" ] && mv "$dir" "$dir.before-bootstrap-$(date +%Y%m%d-%H%M%S)"
        mv "$tmp" "$dir"
    fi
    echo "== $name: checking out $commit"
    git -C "$dir" fetch -q --all --tags
    git -C "$dir" checkout -q --force "$commit"
    case " $RECURSIVE " in *" $name "*) git -C "$dir" submodule update -q --init --recursive ;; esac
    for spec in $SUBMODULES_ONLY; do
        [ "${spec%%:*}" = "$name" ] && git -C "$dir" submodule update -q --init --recursive -- "${spec#*:}"
    done

    if [ "$name" = jansson ] && [ -n "${VITASDK:-}" ] && [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
        # Twelve C files, compiled by hand rather than through jansson's own
        # CMake: it sets POSITION_INDEPENDENT_CODE on the target, which no
        # -D on the command line overrides. Cheaper to compile than to fight.
        echo "   building for the Vita, without PIC"
        ( cd "$dir"
          cmake -S . -B build_vita -G Ninja \
                -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
                -DJANSSON_BUILD_SHARED_LIBS=OFF -DJANSSON_EXAMPLES=OFF \
                -DJANSSON_BUILD_DOCS=OFF -DJANSSON_WITHOUT_TESTS=ON >/dev/null
          rm -rf build_vita/obj build_vita/lib
          mkdir -p build_vita/obj build_vita/lib
          for c in src/*.c; do
              "$VITASDK/bin/arm-vita-eabi-gcc" -O2 -fno-pic -include stdint.h \
                  -c "$c" -o "build_vita/obj/$(basename "${c%.c}").o" \
                  -Ibuild_vita/include -Ibuild_vita/private_include -Isrc
          done
          "$VITASDK/bin/arm-vita-eabi-ar" rcs build_vita/lib/libjansson.a build_vita/obj/*.o )
    fi

    if [ -n "$patch" ]; then
        if git -C "$dir" apply --check --reverse "$patch" 2>/dev/null; then
            echo "   local fixes already in place"
        else
            git -C "$dir" apply "$patch"
            echo "   local fixes applied ($(basename "$patch"))"
        fi
    fi
done

if [ "$check_only" = 1 ]; then
    echo
    [ $rc = 0 ] && echo "Every library is present with its local fixes." \
                || echo "Run tools/bootstrap-libs.sh to fix the above."
    exit $rc
fi

echo
echo "Done. wolfSSL still has to be BUILT out of band (with DTLS on Windows -"
echo "see docs/WINDOWS_BUILD.md); CMake does not drive these builds."
