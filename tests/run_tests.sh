#!/usr/bin/env bash
# Runs the offline tests of the protocol logic.
# No console, no VM, no network: they check PURE functions, the ones whose every
# bug has cost an RE campaign (G4, G40, G43) or could hang the control thread
# (the protobuf bounds).
#
# To add a module: put it in PURE_MODULES when it is pure (no global state, no
# I/O, no getenv) and write a test_<module>.c next to it.
set -euo pipefail
cd "$(dirname "$0")"

PURE_MODULES=(
    ../core/protocol/vid_wire.c    # the video wire's semantics (G4/G21, G40, G43)
    ../core/protocol/proto.c       # protobuf: our messages + the server replies
    ../core/protocol/msgframe.c    # framing by size prefix (the input channel)
    ../core/protocol/gamepad_wire.c # the gamepad channel's rumble (G57)
    ../core/protocol/clip_wire.c   # the clipboard channel's framing (CLIP)
)
# sufp.c needs a journal_uncategorised; only its own test provides the stub, so it stays
# out of the shared list.

# A watchdog per binary: a test that does not return signals an infinite loop -
# exactly the defect test_proto.c watches for.
CFLAGS=(-Wall -Wextra -Werror -O1)

# === WIN3 2026-10-02 - THIS SCRIPT COULD NOT RUN ON WINDOWS AT ALL ==========
#
# `mktemp -d` below, and then every `cc.exe` it invokes, need a temporary
# directory. MSYS2's bash CLEARS TMP, TEMP and TMPDIR on entry - it does not
# merely translate them - so the native toolchain falls back on the Windows
# default and gets `C:\WINDOWS\`, which is not writable:
#
#     Cannot create temporary file in C:\WINDOWS\: Permission denied
#
# and the script died on its first line, before a single test ran. Exporting
# TMP from the calling shell does not help, for the same reason.
# `tools/build-libs.sh` already pays this (`win_fix_tmp`, documented in
# docs/WINDOWS_BUILD.md §3c); the test runner had never been run from Windows,
# so it had never paid it.
#
# A WINDOWS path, not a POSIX one: `cc.exe` is a native binary and does not
# understand `/tmp`. Conditional on cygpath existing, so Linux and CI are
# untouched - and a Windows checkout with no cygpath is told why rather than
# failing on line 25 with a message about a directory nobody asked for.
case "${OSTYPE:-}${MSYSTEM:-}" in
  *msys*|*MINGW*|*UCRT*|*cygwin*)
    case "${TMP:-}" in
      [A-Za-z]:[/\\]*) ;;                       # already usable: leave it
      *)
        if command -v cygpath >/dev/null 2>&1; then
          TMP="$(cygpath -w /tmp 2>/dev/null)" || TMP=""
          if [ -n "$TMP" ]; then
            TEMP="$TMP"; TMPDIR="$TMP"
            export TMP TEMP TMPDIR
          else
            echo "windows: cannot derive a usable TMP (cygpath failed)" >&2
            exit 4
          fi
        else
          echo "windows: cygpath missing, so TMP cannot be made usable;" >&2
          echo "          set TMP to a Windows-style writable path by hand" >&2
          exit 4
        fi
        ;;
    esac
    ;;
esac

out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
rc=0

# === WIN4 2026-10-02 - A SUITE THAT WILL NOT COMPILE MUST NOT STOP THE REST ==
#
# `gcc` ran bare under `set -e`, so the FIRST suite that failed to compile took
# the whole script down and every suite after it was never run. On Windows that
# is `test_sufp`, which uses `%zu` in a journal call: correct for the targets
# that matter (journal.c rewrites the format at run time - see
# docs/WINDOWS_BUILD.md §3c-ter), and refused at compile time by MinGW's
# -Wformat, which does not know `%z`. CLAUDE.md already says several suites do
# not compile on MinGW; what it could not say is that one of them hid the other
# fifty.
#
# A compile failure is now a FAILURE OF THAT SUITE - named, with its compiler
# output kept - and the run continues. `rc` still ends non-zero, so CI is as
# strict as before; the difference is that a Windows checkout now gets 53
# verdicts instead of one abort.
compile_fail() {                 # compile_fail <name> <logfile>
    echo "  (DID NOT COMPILE: $1)"
    sed 's/^/    /' "$2" | head -12
    rc=1
}

run() {                          # run <name> <source> [sources/flags...]
    local name=$1; shift
    local bin="$out/$name"
    local log="$out/$name.cc"
    if ! gcc "${CFLAGS[@]}" -o "$bin" "$@" >"$log" 2>&1; then
        compile_fail "$name" "$log"
        return 0
    fi
    if ! timeout 30 "$bin"; then echo "  (failure or hang: $name)"; rc=1; fi
}

# `rate_meter.hpp` is a class: it needs a C++ compiler. Only one suite needs it,
# hence a variant rather than changing the whole file.
run_cpp() {                      # run_cpp <name> <source> [sources/flags...]
    local name=$1; shift
    local bin="$out/$name"
    local log="$out/$name.cc"
    if ! g++ "${CFLAGS[@]}" -o "$bin" "$@" >"$log" 2>&1; then
        compile_fail "$name" "$log"      # WIN4: see run() above
        return 0
    fi
    if ! timeout 30 "$bin"; then echo "  (failure or hang: $name)"; rc=1; fi
}

# -- Suites with no dependency at all: always run ----------------------------
run test_vid_wire test_vid_wire.c "${PURE_MODULES[@]}"
run test_proto    test_proto.c    "${PURE_MODULES[@]}"
# === WIN9 2026-10-02 - `%z` IS RIGHT HERE AND MinGW REFUSES IT ANYWAY ========
#
# `sufp.c` logs `(>%zu)` through the journal, where `strip_z_modifier` rewrites
# the format before the single `vsnprintf` - documented in
# docs/WINDOWS_BUILD.md §3c-ter, and `tools/check-z-formats.py` guards the case
# that IS a hazard, a `%z` in a direct printf. MinGW's -Wformat knows none of
# that and rejects the literal, so this suite did not compile on Windows.
#
# The format is not changed: it is correct for the three targets that ship, and
# editing production code to satisfy a warning on a platform that only builds
# for testing is the wrong direction. The flag is relaxed HERE, for THIS suite,
# on Windows only - Linux and CI keep -Wformat at full strength, which is where
# a real format defect would be caught.
# Named once: `vid_reasm.c` logs `%zu` through the journal for the same reason
# and is refused by the same warning.
ZFMT_FLAGS=()
case "${OSTYPE:-}${MSYSTEM:-}" in
  *msys*|*MINGW*|*UCRT*|*cygwin*) ZFMT_FLAGS=(-Wno-format -Wno-format-extra-args) ;;
esac
run test_sufp     test_sufp.c     ../core/protocol/sufp.c "${ZFMT_FLAGS[@]}"
run test_msgframe test_msgframe.c "${PURE_MODULES[@]}"
run test_idr_policy test_idr_policy.c   # header-only: no source to link
run test_freeze_stat test_freeze_stat.c # header-only: the G44 detector (HO-2)
run test_ovfl       test_ovfl.c         # header-only: kernel drop accounting (ING-1)
run test_audio_dedup test_audio_dedup.c # likewise
run test_audio_route  test_audio_route.c  # header-only: which :base+30 plaintexts are audio, when FLAC is recognised (DEC-1)
run test_aud_reasm    test_aud_reasm.c    # header-only: split audio frames are session state (ING-A2)
run test_audio_loss   test_audio_loss.c   # header-only: audio frames lost in both copies, and the panel's grade (AUD-DEDUP-3, AUD-INS-1)
run test_audio_gap    test_audio_gap.c -lm # header-only, -lm for the test's generator: output underruns, two-sided and confirmed (OUT-1)
run test_cursor_wire test_cursor_wire.c ../core/protocol/cursor_wire.c
# CLIP - the clipboard channel :base+14. clip_wire.c is in PURE_MODULES above;
# clip_chan.c holds the reassembly state and reads the two SHADOW_CLIP_*
# toggles, so it is named here rather than added to the shared list.
run test_clip_wire   test_clip_wire.c "${PURE_MODULES[@]}" ../core/protocol/clip_chan.c
run test_gamepad_wire test_gamepad_wire.c "${PURE_MODULES[@]}"
run test_ctrl_inv     test_ctrl_inv.c      # header-only: which Request field a ctrl message carries (SRV7)
run test_ft_path      test_ft_path.c       # header-only: remote paths we refuse to send, since the VM confines none (FT1)
run test_jwt          test_jwt.c           # header-only: the JWT `instance` field, and the buffer S49 once overflowed (LIB2)
# QT1 - the Qt client's stride-aware plane copy. Pure arithmetic on purpose, so
# it needs no Qt, no GPU and no window; the shear it prevents looks like a
# decoder fault.
run_cpp test_qt_planes test_qt_planes.cpp
run test_ft_uri       test_ft_uri.c        # header-only: the SFTP URI a file manager opens - base64 escaping, IPv6 brackets (FT4)
run test_clip_dir     test_clip_dir.c      # header-only: which way the clipboard may travel, and the clamp (CLIP6)
run test_hid_lock     test_hid_lock.c ../core/protocol/proto.c  # the Caps/Num/Scroll Lock message on :base+11 (HID1)
run test_vid_uplink   test_vid_uplink.c ../core/protocol/gamepad_wire.c  # the gE timestamp, the IFR counter, the axis int16, the channel map (SRV1/3/5/6)
run test_audio_gain   test_audio_gain.c    # header-only
run test_kbd_scancode test_kbd_scancode.c  # likewise
run test_ui_nav       test_ui_nav.c        # likewise
run test_ui_anim      test_ui_anim.c  -lm  # header-only, but powf/isnan
run test_ui_utf8      test_ui_utf8.c       # likewise
run test_ui_gestures  test_ui_gestures.c   # likewise
# K21 - the footer's button-to-key table. It must agree with the K20 patch in
# Borealis' own hint bar: the two bars share a screen.
run test_key_label    test_key_label.c     # header-only
# HOLD-1 - the hold that closes the three in-app testers. Its counter-case is an
# #ifdef that excluded the whole measurement off console: no warning, no failure.
run test_hold_exit    test_hold_exit.c     # header-only
run test_devcmd       test_devcmd.c        # likewise
run test_inject       test_inject.c   -lm  # INJ-1: the timing of synthetic input
run test_authz        test_authz.c         # AUTH-1: who may drive this console
run test_errors       test_errors.c ../core/services/errors.c
# WIN4 2026-10-02 - `win_env.c` supplies setenv/unsetenv, which the Windows CRT
# does not have. It is empty outside _WIN32, so naming it costs Linux nothing
# and is what lets this suite build on Windows at all.
run test_env_override test_env_override.c ../core/services/env_override.c ../core/services/win_env.c
run test_applock      test_applock.c      ../core/services/applock.c
run test_atomic_file  test_atomic_file.c  ../core/services/atomic_file.c  # AF4
run test_lock_grid    test_lock_grid.c           # header-only
run test_ann_reply    test_ann_reply.c    ../core/protocol/ann_reply.c ../core/protocol/proto.c
run test_path_probe   test_path_probe.c          # header-only
run test_rtt          test_rtt.c                 # header-only
# AUD-INS-3 - the metrics panel's snapshot: one writer thread per field group.
# Its stress half runs two real writer threads and a reader for about two
# seconds; -pthread takes winpthreads on MinGW.
run test_stats        test_stats.c   ../core/services/stats.c -pthread
run test_wav          test_wav.c     ../clients/borealis/ui/wav.c
run test_eq           test_eq.c      ../core/protocol/eq.c -lm
# S86 - splitting a log line into columns. It reads a format ANOTHER file writes
# (journal.c): this test is the only tie between the two, nothing in the compiler
# relates them.
run test_journal_line test_journal_line.c   # header-only

# The PS Vita paths: pure arithmetic, no system call. Everything the port gained
# on 2026-09-13 had only the console as a witness, and three of the defects found
# that day were arithmetic - checkable here in a few microseconds rather than in
# a deployment and a session.
run test_vita_paths test_vita_paths.c   # header-only
run test_bitrate      test_bitrate.c        # header-only: the one bitrate ladder (B1)
run test_bitrate_ctl  test_bitrate_ctl.c    # header-only: G19 and the user's cap (CFG-1)
run test_log_mask     test_log_mask.c       # header-only: secrets in the log, start and end only (SEC2)
run test_log_redact   test_log_redact.c     # header-only: the journal's LAST line of defence, by shape (2026-09-13)
# DNS1 - the session's single lookup of the VM name. It calls the real
# getaddrinfo, on NUMERIC hosts only: no DNS, no network. Run twice: the second
# run sets SHADOW_RESOLVE_ONCE=0 and asserts the previous behaviour (one real
# lookup per call) - the counter-case. Winsock on MinGW wants -lws2_32.
NETLIBS=()
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) NETLIBS=(-lws2_32) ;; esac
run test_session_host test_session_host.c ../core/protocol/session_host.c \
    ../core/services/sockets_compat.c -pthread ${NETLIBS[@]+"${NETLIBS[@]}"}
if ! SHADOW_RESOLVE_ONCE=0 timeout 30 "$out/test_session_host"; then
    echo "  (failure or hang: test_session_host, SHADOW_RESOLVE_ONCE=0)"; rc=1
fi
run test_pad_mouse    test_pad_mouse.c      # header-only: the Joy-Cons as a mouse (PM1)
run test_rear_touch    test_rear_touch.c     # header-only: the Vita's rear pad as the four buttons it lacks
# L5 - the latency measuring instrument. A WRONG instrument is worse than no
# instrument: it produces numbers people believe, and its three possible errors
# are all silent (an optimistic percentile, a reading that loses samples, a
# server clock offset taken for latency). The test includes the .c to reach the
# bucketing, which IS the logic.
run test_latency      test_latency.c ../core/services/win_env.c  # the log stub is provided by the test; win_env for setenv on Windows (WIN7)
# screen.hpp pulls in nanovg.h for NVGcolor; its geometric part stays pure C.
# The language files do not compile: a missing key returns the key itself, which
# runs fine and only shows on screen, in the language one does not use oneself.
# Hence a separate check.
# `cd` has already moved into tests/: `dirname "$0"` would be relative to the
# STARTING directory there and would give ./tests/tests/. The bare path is the
# only correct one here.
if ! python3 verify_i18n.py; then rc=1; fi

# The vitasdk's newlib knows no %z, and an unknown modifier does not consume its
# argument - so the NEXT conversion reads the wrong va_list slot. That is a
# crash the compiler cannot see (%zu is valid C99) and that only one of the two
# consoles suffers, which is exactly the kind of defect no build catches. The
# guard is partial by design (same-line calls only, see its docstring); running
# it here is what makes it a guard rather than a script nobody invokes.
if ! python3 ../tools/check-z-formats.py; then rc=1; fi

# LIB1 2026-10-02 - `core/` is built as the `halyard-core` library and is meant
# to be reusable by a Qt or Tauri client. That it depends on no client was true
# and UNVERIFIED: nothing would have failed if a core file had included
# `clients/`. Now something does.
if ! python3 ../tools/check-core-independence.py; then rc=1; fi

run_cpp test_rate_meter test_rate_meter.cpp
# AUDC-1 / OUT-2 - the UI sounds hand the console's single audio output to the
# stream. The REAL ui/sfx.cpp, built with -D__SWITCH__ against a mock of libnx's
# one IAudioOut (tests/mock/switch.h), next to a stub that transcribes
# media/audio.c's audout create/feed/destroy; real time, about ten seconds. Run
# twice: the second run sets SHADOW_SFX_HANDOFF=0 and asserts the defect itself
# (the cut chime, the silent reconnect, UI sounds opening audout under a live
# stream) - the counter-case. wav.c is compiled as the C it is.
run_cpp test_sfx_handoff test_sfx_handoff.cpp ../clients/borealis/ui/sfx.cpp \
    -x c ../clients/borealis/ui/wav.c -x none -D__SWITCH__ -Imock -pthread
if ! SHADOW_SFX_HANDOFF=0 timeout 30 "$out/test_sfx_handoff"; then
    echo "  (failure or hang: test_sfx_handoff, SHADOW_SFX_HANDOFF=0)"; rc=1
fi
run test_ui_screen    test_ui_screen.c  -lm \
    -I../third_party/borealis/library/include/borealis/extern/nanovg

# -- The server reply parsers ------------------------------------------------
# ctrl_msgs.c includes the wolfSSL HEADERS (not the library: the three
# randomness functions are stubbed in the test). `options.h` is generated by
# wolfssl's out-of-CMake build; without it we skip cleanly, rather than making
# the whole suite untestable on a freshly cloned repo.
WOLF=../third_party/wolfssl

# === WIN8 2026-10-02 - THE GATE NAMED ONE BUILD DIRECTORY, SO THREE SUITES
# ===               NEVER RAN ANYWHERE BUT LINUX
#
# The three suites below need wolfSSL's generated `options.h`, and one of them
# the compiled library. The gate tested `build_linux/` by name - correct on the
# reference platform and nowhere else - so on a Windows checkout, where
# `tools/build-libs.sh windows` has produced `build_windows/` with the SAME
# layout, they printed SKIPPED. Three of the most valuable suites in the file:
# the byte-exact control messages, the real ChaCha20-Poly1305 against RFC 8439,
# and video reassembly.
#
# SKIPPED is not a pass - CLAUDE.md says so in as many words - and these were
# skipped on every Windows run since the Windows build existed.
#
# `build_linux` is still PREFERRED, because it is the reference and because a
# measurement is quoted against it. The directory actually used is printed, so
# a result is never ambiguous about what it was linked against.
WOLFB=""
for cand in build_linux build_windows; do
    if [ -f "$WOLF/$cand/wolfssl/options.h" ]; then WOLFB="$cand"; break; fi
done
if [ -n "$WOLFB" ]; then
    echo "-- wolfSSL suites built against $WOLF/$WOLFB"
fi
if [ -n "$WOLFB" ]; then
    run test_ctrl_msgs test_ctrl_msgs.c \
        ../core/protocol/ctrl_msgs.c ../core/protocol/proto.c \
        -I "$WOLF" -I "$WOLF/$WOLFB"
    # The encryption, on the other hand, needs the compiled LIBRARY: it is the
    # real chacha20-poly1305 we check against the RFC 8439 vector, not a stub.
    if [ -f "$WOLF/$WOLFB/libwolfssl.a" ]; then
        run test_encryption test_encryption.c \
            ../core/protocol/encryption.c \
            -I "$WOLF" -I "$WOLF/$WOLFB" \
            "$WOLF/$WOLFB/libwolfssl.a" -lm -lpthread
    else
        echo "== chacha20-poly1305: SKIPPED ==  ($WOLF/$WOLFB/libwolfssl.a is missing)"
    fi
else
    echo "== server reply parsers: SKIPPED =="
    echo "  (no $WOLF/build_linux or build_windows with wolfssl/options.h - run tools/build-libs.sh)"
fi

# -- The real video reassembly (REASM-1) --------------------------------------
# vid_reasm.c itself, fed synthetic pictures: G43's three single-chunk cases, a
# clean stream that must come out byte-identical, a subchannel reuse, three
# stale buffers. It needs the wolfSSL HEADERS only, because ctrl_audio_dtls.h
# includes them: the test stubs the decryption as the identity and links no
# library - hence the same gate as above. -pthread: MinGW takes clock_gettime
# from winpthreads; harmless on Linux.
# Run TWICE. The second run sets the toggle to 0, which must restore the
# previous behaviour of both opening paths exactly - so that run asserts the
# defect itself (out-of-order emission, a loss in no counter): the counter-case.
if [ -n "$WOLFB" ]; then
    run test_vid_reasm test_vid_reasm.c \
        ../core/protocol/vid_reasm.c ../core/protocol/vid_wire.c \
        -I "$WOLF" -I "$WOLF/$WOLFB" -pthread "${ZFMT_FLAGS[@]}"
    # WIN9: the counter-case only means something if the binary exists. It did
    # not on Windows, and the run then reported a SECOND failure for the same
    # compile error - two lines pointing at one cause.
    if [ -x "$out/test_vid_reasm" ]; then
        if ! SHADOW_FLUSH_PREV_INCOMPLETE=0 timeout 30 "$out/test_vid_reasm"; then
            echo "  (failure or hang: test_vid_reasm, SHADOW_FLUSH_PREV_INCOMPLETE=0)"; rc=1
        fi
    fi
else
    echo "== video reassembly (REASM-1): SKIPPED ==  (same missing header)"
fi

exit $rc
