#!/usr/bin/env bash
# An autonomous campaign, 2026-08-28 — it validates V9, V10 and G52/G53 on the desktop client.
#
# Trois questions, trois protocoles :
#   A. V10 (retransmission) — an A/B under INDUCED loss. A clean link never
#      triggers a NACK: with no loss, the A/B measures nothing. The lever is
#      SHADOW_UDP_RCVBUF (N56), calibre au prealable.
#   B. G52/G53 (the gamepad) — impossible without a gamepad: we create one through uinput.
#   C. V9 — the short-packet counter on :base+10, read in every session.
set -u
cd "$(dirname "$0")/../.."
RACINE=$PWD
BIN=$RACINE/build_linux/halyard-cli
OUT=$RACINE/captures/tests_autonomes_20260828
mkdir -p "$OUT"

lancer() {   # lancer <etiquette> <duree> [VAR=VAL ...]
    local nom=$1 duree=$2; shift 2
    echo "  -> $nom (${duree}s) $*"
    ( export "$@" 2>/dev/null || true
      timeout $((duree + 90)) "$BIN" --mode=native-stream --duration="$duree" --activity \
          --label="$nom" --output="$OUT/$nom.json" ) > "$OUT/$nom.out" 2>&1
    cp -f /tmp/halyard/halyard.log "$OUT/$nom.halyard.log" 2>/dev/null
    python3 - "$OUT/$nom.json" <<'PY' 2>/dev/null || echo "     (no JSON)"
import json,sys
d=json.load(open(sys.argv[1]))
v=d.get("video",{}); p=d.get("perte",{})
print("     pps=%.0f kbps=%.0f fps=%.1f img=%d | manques=%s/%s orph=%s nack=%s trunc=%s courts=%s"
      % (v.get("total_packets",0)/max(d.get("session_seconds",1),1), v.get("avg_kbps",0),
         v.get("fps",0), v.get("frames_decoded",0),
         p.get("chunks_missing","?"), p.get("chunks_expected","?"),
         p.get("chunks_orphan","?"), p.get("nack_sent","?"),
         p.get("frames_dropped_trunc","?"), p.get("paquets_courts_base10","?")))
PY
    sleep 8   # let the server release the previous session
}

echo "=== STEP 0 — calibrating the loss (the N56 lever) ==="
CAL=""
for rb in 262144 131072 65536; do
    lancer "cal_rcvbuf_$rb" 45 SHADOW_UDP_RCVBUF=$rb
    m=$(python3 -c "import json;print(json.load(open('$OUT/cal_rcvbuf_$rb.json')).get('perte',{}).get('chunks_missing',0))" 2>/dev/null || echo 0)
    echo "     manques=$m"
    if [ "${m:-0}" -ge 200 ]; then CAL=$rb; break; fi
done
if [ -z "$CAL" ]; then
    echo "  !! no loss induced even at 64 KB — the V10 A/B can measure nothing."
    echo "     We carry on to the gamepad anyway."
else
    echo "=== ETAPE 1 — A/B V10 sous SHADOW_UDP_RCVBUF=$CAL (3+3, alternes) ==="
    for i in 1 2 3; do
        lancer "v10_avant_$i" 90 SHADOW_UDP_RCVBUF=$CAL SHADOW_NACK_AT_GAP=0 SHADOW_NACK_TICK=1
        lancer "v10_apres_$i" 90 SHADOW_UDP_RCVBUF=$CAL
    done
fi

echo "=== ETAPE 2 — G52/G53 manette (uinput) ==="
"$RACINE/tools/test/virtual_gamepad" 80 > "$OUT/manette_uinput.log" 2>&1 &
MPID=$!
sleep 2
lancer "g52_g53_manette" 60 SHADOW_GAMEPAD=1
wait $MPID 2>/dev/null

echo "=== STEP 3 — a control with no gamepad (the detector must stay silent) ==="
lancer "g52_temoin_sans_manette" 40 SHADOW_GAMEPAD=1

echo "=== TERMINE ==="
