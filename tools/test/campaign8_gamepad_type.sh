#!/usr/bin/env bash
# Campaign 8 — sweeping byte 3 of the kPlug: which gamepad type are we announcing?
#
# THE HYPOTHESIS: byte 3 carries the type, and the official binary's contiguous pool
#   Google, Microsoft, Nintendo, Logitech, Default,
#   ControllerRight, ControllerLeft, Dualshock4, XboxOne, Xbox360
# donne, en ordre de declaration, Dualshock4=2 / XboxOne=3 / Xbox360=4.
# Our hardcoded `02` comes from a capture taken with a DualShock 4: consistent.
#
# WHAT THE MEASUREMENT CAN SETTLE WITHOUT SEEING WINDOWS: the server acknowledges every
# announcement with a kReply. The official binary carries `OnGamepadReplyError` next
# to `OnGamepadReplyUnplug` — so a refused value ought to change the reply
# (a different type, a different byte 11, or silence). A 0..9 sweep draws out the enum.
set -u
cd "$(dirname "$0")/../.."
RACINE=$PWD
BIN=$RACINE/build_linux/halyard-cli
OUT=$RACINE/captures/tests_autonomes_20260828

for t in 0 1 2 3 4 5 6 7 9; do
    nom="g56_type_$t"
    echo "  -> type=$t"
    # === CLEAR THE LOG BEFORE EVERY SESSION ===
    # Without this, a session that FAILS (e.g. HTTP 409: another Shadow client
    # already holds the session) leaves the PREVIOUS session's log in place, which we
    # then copy as if it were its own. Eight files identical to the md5,
    # and a whole enum read out of stale data. Silence is this repo's
    # failure mode; we remove it at the source.
    : > /tmp/halyard/halyard.log 2>/dev/null
    "$RACINE/tools/test/virtual_gamepad" 55 > "$OUT/$nom.uinput.log" 2>&1 &
    mpid=$!
    sleep 2
    ( export SHADOW_GAMEPAD=1 SHADOW_VIDEO_TCP=0 SHADOW_GAMEPAD_TYPE=$t
      timeout 130 "$BIN" --mode=native-stream --duration=40 --activity \
          --label="$nom" --output="$OUT/$nom.json" ) > "$OUT/$nom.out" 2>&1
    cp -f /tmp/halyard/halyard.log "$OUT/$nom.halyard.log" 2>/dev/null
    wait $mpid 2>/dev/null
    # A mounting check: without the [G55] line, the session never reached the
    # gamepad channel and its log means nothing.
    if ! awk '/\[G55\] PREMIER octet/{f=1} END{exit !f}' "$OUT/$nom.halyard.log" 2>/dev/null; then
        echo "     !! SESSION NON ABOUTIE — $(awk '/FAIL|ERROR|409/{print; exit}' "$OUT/$nom.out" 2>/dev/null)"
        sleep 10
        continue
    fi
    awk -v t="$t" '
        /annonce de branchement   corps=/ { sub(/.*corps= /,""); envoi=substr($0,1,14) }
        /manette RX/ { sub(/.*len=14 : /,""); rx[++n]=substr($0,1,14); typ[n]=$0 }
        /\[S56\] CHANNEL_DOWN canal=CONTROLLER/ { down++ }
        END {
          printf("     envoye  : %s\n", envoi)
          printf("     reponses: %d   CHANNEL_DOWN: %d\n", n+0, down+0)
          for (i=1; i<=n && i<=3; i++) printf("       <- %s\n", rx[i])
        }' "$OUT/$nom.halyard.log"
    sleep 6
done
echo "=== TERMINE ==="
