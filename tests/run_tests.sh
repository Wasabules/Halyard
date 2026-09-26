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
)
# sufp.c needs a journal_uncategorised; only its own test provides the stub, so it stays
# out of the shared list.

# A watchdog per binary: a test that does not return signals an infinite loop -
# exactly the defect test_proto.c watches for.
CFLAGS=(-Wall -Wextra -Werror -O1)
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
rc=0

run() {                          # run <name> <source> [sources/flags...]
    local name=$1; shift
    local bin="$out/$name"
    gcc "${CFLAGS[@]}" -o "$bin" "$@"
    if ! timeout 30 "$bin"; then echo "  (failure or hang: $name)"; rc=1; fi
}

# `rate_meter.hpp` is a class: it needs a C++ compiler. Only one suite needs it,
# hence a variant rather than changing the whole file.
run_cpp() {                      # run_cpp <name> <source> [sources/flags...]
    local name=$1; shift
    local bin="$out/$name"
    g++ "${CFLAGS[@]}" -o "$bin" "$@"
    if ! timeout 30 "$bin"; then echo "  (failure or hang: $name)"; rc=1; fi
}

# -- Suites with no dependency at all: always run ----------------------------
run test_vid_wire test_vid_wire.c "${PURE_MODULES[@]}"
run test_proto    test_proto.c    "${PURE_MODULES[@]}"
run test_sufp     test_sufp.c     ../core/protocol/sufp.c
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
run test_gamepad_wire test_gamepad_wire.c "${PURE_MODULES[@]}"
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
run test_env_override test_env_override.c ../core/services/env_override.c
run test_applock      test_applock.c      ../core/services/applock.c
run test_atomic_file  test_atomic_file.c  ../core/services/atomic_file.c  # AF4
run test_lock_grid    test_lock_grid.c           # header-only
run test_ann_reply    test_ann_reply.c    ../core/protocol/ann_reply.c ../core/protocol/proto.c
run test_path_probe   test_path_probe.c          # header-only
run test_rtt          test_rtt.c                 # header-only
# AUD-INS-3 - the metrics panel's snapshot: one writer thread per field group.
# Its stress half runs two real writer threads and a reader for about two
# seconds; -pthread takes winpthreads on MinGW.
run test_stats        test_stats.c   ../core/common/stats.c -pthread
run test_wav          test_wav.c     ../clients/borealis/ui/wav.c
run test_eq           test_eq.c      ../core/protocol/eq.c -lm
# S86 - splitting a log line into columns. It reads a format ANOTHER file writes
# (journal.c): this test is the only tie between the two, nothing in the compiler
# relates them.
run test_journal_line test_journal_line.c   # header-only

# Les chemins PS Vita : arithmetique pure, aucun appel systeme. Tout ce que le
# port a gagne le 2026-09-13 n'avait que la console pour temoin, et trois des
# defauts trouves ce jour-la etaient de l'arithmetique -- verifiable ici en
# quelques microsecondes plutot qu'en un deploiement et une session.
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
run test_latency      test_latency.c        # the log stub is provided by the test
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
if [ -f "$WOLF/build_linux/wolfssl/options.h" ]; then
    run test_ctrl_msgs test_ctrl_msgs.c \
        ../core/protocol/ctrl_msgs.c ../core/protocol/proto.c \
        -I "$WOLF" -I "$WOLF/build_linux"
    # The encryption, on the other hand, needs the compiled LIBRARY: it is the
    # real chacha20-poly1305 we check against the RFC 8439 vector, not a stub.
    if [ -f "$WOLF/build_linux/libwolfssl.a" ]; then
        run test_encryption test_encryption.c \
            ../core/protocol/encryption.c \
            -I "$WOLF" -I "$WOLF/build_linux" \
            "$WOLF/build_linux/libwolfssl.a" -lm -lpthread
    else
        echo "== chacha20-poly1305: SKIPPED ==  ($WOLF/build_linux/libwolfssl.a is missing)"
    fi
else
    echo "== server reply parsers: SKIPPED =="
    echo "  ($WOLF/build_linux/wolfssl/options.h is missing - build wolfssl for Linux)"
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
if [ -f "$WOLF/build_linux/wolfssl/options.h" ]; then
    run test_vid_reasm test_vid_reasm.c \
        ../core/protocol/vid_reasm.c ../core/protocol/vid_wire.c \
        -I "$WOLF" -I "$WOLF/build_linux" -pthread
    if ! SHADOW_FLUSH_PREV_INCOMPLETE=0 timeout 30 "$out/test_vid_reasm"; then
        echo "  (failure or hang: test_vid_reasm, SHADOW_FLUSH_PREV_INCOMPLETE=0)"; rc=1
    fi
else
    echo "== video reassembly (REASM-1): SKIPPED ==  (same missing header)"
fi

exit $rc
