#!/usr/bin/env bash

# NOTE (2026-09-14): the markers are matched against BOTH project names. The
# 2026-09-13 rename touched this script and the patch file but not the comments
# already sitting in the vendored tree, so every one of the nine checks reported
# MISSING while every fix was in fact present. A guard that cries wolf on all
# nine is worse than no guard: it gets ignored, and the day one really goes
# missing nobody looks. (That day was 2026-09-14, GXM-1.)
#
# NOTE (2026-09-12): this script CHECKS, by grepping for the markers. Since the
# same day there is also a real patch -- `local-fixes.patch`, made against the
# upstream commit 3ecf2de (see README.md - `5000a1e5` was a typo'd, wrong base) --
# and `tools/bootstrap-libs.sh` clones and applies it. Prefer that to re-pasting
# the `.extract` snippets by hand: two of the seven fixes below had been lost
# exactly that way, and stayed MISSING for months.
# Re-applies the local Borealis fixes (see README.md).
# Idempotent: a fix that is already present is reported, not duplicated.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRL="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/third_party/borealis/library/lib"
rc=0

verify() {   # file, pattern, description
    if [ ! -f "$1" ]; then echo "  ABSENT  $1"; return 1; fi
    if grep -Eq "$2" "$1"; then echo "  present $3"; return 0; fi
    echo "  MISSING $3  ->  $1"; return 1
}

echo "Local Borealis fixes:"
verify "$BRL/core/application.cpp"          "LOCAL FIX (halyard|shadow2switch).*S63" "S63 giveFocus(nullptr)"           || rc=1
verify "$BRL/core/application.cpp"          "videoContext->beginFrame\(\)"  "GXM-1 the frame is opened (PS Vita)"  || rc=1
verify "$BRL/views/scrolling_frame.cpp"     "LOCAL FIX (halyard|shadow2switch)"      "null-focus guard (vertical)"      || rc=1
verify "$BRL/views/h_scrolling_frame.cpp"   "LOCAL FIX (halyard|shadow2switch)"      "null-focus guard (horizontal)"    || rc=1
verify "$BRL/views/hint.cpp"                "LOCAL FIX (halyard|shadow2switch).*K20" "K20 keyboard labels in the hint bar" || rc=1
verify "$BRL/platforms/glfw/glfw_input.cpp" "LOCAL FIX (halyard|shadow2switch).*K20" "K20 keys X/Y/L/R/F1/F2/Q/P"       || rc=1
verify "$BRL/core/thread.cpp"               "LOCAL FIX (halyard|shadow2switch).*PSV2" "PSV2 task-loop stack on Vita"       || rc=1
verify "$BRL/platforms/psv/psv_input.cpp"   "LOCAL FIX (halyard|shadow2switch).*PSV3" "PSV3 stick axes centred on Vita"    || rc=1
verify "$BRL/platforms/glfw/glfw_input.cpp"   "(halyard|shadow2switch)_inject_controller" "INJ-1 input injection (desktop)"  || rc=1
verify "$BRL/platforms/switch/switch_input.cpp" "(halyard|shadow2switch)_inject_controller" "INJ-1 input injection (console)" || rc=1
verify "$BRL/platforms/psv/psv_video.cpp"   "SHADOW_GXM_MSAA"              "LAT-V1 MSAA level (PS Vita)"        || rc=1
verify "$BRL/../include/borealis/extern/nanovg/nanovg_gxm_utils.h" "SHADOW_GXM_PENDING" "LAT-V1 display queue depth (PS Vita)" || rc=1

if [ $rc -ne 0 ]; then
    echo
    echo "A fix is missing: see patches/borealis/README.md for the what and the why,"
    echo "and patches/borealis/*.extract for the exact code to put back."
fi
exit $rc
