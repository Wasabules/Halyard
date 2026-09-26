#!/usr/bin/env bash
# Campaign 3 — G53: does the server really answer on :base+13?
#
# Campaign 1: 2479 gamepad messages emitted, ONE plug-in announcement (G52 fine),
# et ZERO reponse recue. Trois explications possibles, qu'on separe ici :
#   a) the server does not answer in our configuration;
#   b) it only answers after the UDP REGISTRATION packet, which we do not send
#      on that port (SHADOW_UDP_REG_13, 0 by default);
#   c) it answers but from a different source port, and our `connect()`ed socket
#      le filtre.
# The RAW ARRIVAL counter (G53b) decides between (c) and the others: it counts
# before any application-level filter. That leaves the A/B on the registration, for (b).
set -u
cd "$(dirname "$0")/../.."
RACINE=$PWD
BIN=$RACINE/build_linux/halyard-cli
OUT=$RACINE/captures/tests_autonomes_20260828

lancer() {
    local nom=$1 duree=$2; shift 2
    echo "  -> $nom (${duree}s) $*"
    "$RACINE/tools/test/virtual_gamepad" $((duree + 10)) > "$OUT/$nom.uinput.log" 2>&1 &
    local mpid=$!
    sleep 2
    ( export "$@" 2>/dev/null || true
      timeout $((duree + 90)) "$BIN" --mode=native-stream --duration="$duree" --activity \
          --label="$nom" --output="$OUT/$nom.json" ) > "$OUT/$nom.out" 2>&1
    cp -f /tmp/halyard/halyard.log "$OUT/$nom.halyard.log" 2>/dev/null
    wait $mpid 2>/dev/null
    awk -v n="$nom" '
        /ARRIVEE BRUTE/{brut++}
        /annonce de branchement envoyee/{ann++}
        /detache —/{bilan=$0}
        END{printf("     arrivees brutes sur :base+13 = %d | annonces = %d\n%s\n", brut+0, ann+0, bilan)}
    ' "$OUT/$nom.halyard.log"
    sleep 8
}

echo "=== A — sans enregistrement UDP (defaut actuel) ==="
lancer "g53_sans_reg" 60 SHADOW_GAMEPAD=1 SHADOW_UDP_REG_13=0
echo "=== B — WITH a UDP registration on :base+13 ==="
lancer "g53_avec_reg" 60 SHADOW_GAMEPAD=1 SHADOW_UDP_REG_13=1
echo "=== TERMINE ==="
