#!/usr/bin/env bash
# Campaign 6 — check G55 on the desktop client, with a uinput gamepad.
#
# THE THESIS: the server tears the gamepad channel down (CHANNEL_DOWN canal=CONTROLLER)
# when we put a byte on it within the ~500 ms after the grant. Our
# announcement then goes out on an already-dead channel.
#
# An A/B with ONE variable: what precedes the announcement on :base+13.
#   AVANT  = SHADOW_UDP_REG_13=1 SHADOW_GAMEPAD_STRICT_ORDER=0
#            -> registre 25 o en clair + rapports d'entree avant l'annonce
#   AFTER  = the defaults -> the announcement is the channel's FIRST byte
#
# CRITERES, ECRITS AVANT :
#   P0  [G55] must name "a plug-in announcement" in 5 AFTER sessions out of 5.
#   P1  ZERO [S56] CHANNEL_DOWN canal=CONTROLLER line in AFTER, 5 out of 5.
#   P2  the BEFORE arm must produce some — otherwise the test compares nothing.
#   P3  (a bonus) some [G53] RAW ARRIVAL lines: the server finally answers us.
set -u
cd "$(dirname "$0")/../.."
RACINE=$PWD
BIN=$RACINE/build_linux/halyard-cli
OUT=$RACINE/captures/tests_autonomes_20260828

lancer() {
    local nom=$1 duree=$2; shift 2
    echo "  -> $nom (${duree}s) $*"
    "$RACINE/tools/test/virtual_gamepad" $((duree + 15)) > "$OUT/$nom.uinput.log" 2>&1 &
    local mpid=$!
    sleep 2
    # SHADOW_VIDEO_TCP=0: the :base+20 channel times out, 8 s × 4, at
    # INTERMITTENT intervals on this VM — 1 event in one arm of the first test, 10
    # in the other, hence a bootstrap finishing at 12 s on one side and 43 s on the other.
    # Since the MOMENT we speak on :base+13 is precisely the variable under
    # test, that noise made the comparison uninterpretable. So we cut the
    # cursor channel during the test: it has nothing to do with the gamepad.
    ( export SHADOW_GAMEPAD=1 SHADOW_VIDEO_TCP=0 "$@" 2>/dev/null || true
      timeout $((duree + 90)) "$BIN" --mode=native-stream --duration="$duree" --activity \
          --label="$nom" --output="$OUT/$nom.json" ) > "$OUT/$nom.out" 2>&1
    cp -f /tmp/halyard/halyard.log "$OUT/$nom.halyard.log" 2>/dev/null
    wait $mpid 2>/dev/null
    awk -v n="$nom" '
        /\[G55\] PREMIER octet/ { sub(/.*manette : /,""); premier=$0 }
        /\[S56\] CHANNEL_DOWN canal=CONTROLLER/ { down++ }
        /\[G53\] ARRIVEE BRUTE/ { brut++ }
        /\[G53\] bilan/ { bilan=$0; sub(/.*bilan/,"bilan",bilan) }
        /detache —/ { d=$0; sub(/.*detache/,"detache",d) }
        END {
          printf("     1er octet : %s\n", premier ? premier : "(non journalise)")
          printf("     CHANNEL_DOWN CONTROLLER : %d   arrivees brutes : %d\n", down+0, brut+0)
          if (bilan) printf("     %s\n", bilan)
          if (d) printf("     %s\n", d)
        }' "$OUT/$nom.halyard.log"
    sleep 8
}

echo "=== A/B G55 (2nd attempt, without vst) — 5 alternating pairs, a uinput gamepad, 70 s per session ==="
for i in 1 2 3 4 5; do
    lancer "g55b_avant_$i" 70 SHADOW_UDP_REG_13=1 SHADOW_GAMEPAD_STRICT_ORDER=0
    lancer "g55b_apres_$i" 70
done
echo "=== TERMINE ==="
