#!/usr/bin/env bash
# Campaign 4 — the V10 A/B REDONE, with a revert toggle that really does revert
# a l'ancien comportement.
#
# Campaign 2 was invalid: `SHADOW_NACK_TICK=1` did not put the drain back
# on the 50 ms heartbeat, it REMOVED it (the block had been moved, not
# duplicated). So the three reference sessions sent zero NACKs against
# 130 missing chunks — we were comparing "V10" against "no retransmission at
# all", not against "the retransmission we had before".
set -u
cd "$(dirname "$0")/../.."
RACINE=$PWD
BIN=$RACINE/build_linux/halyard-cli
OUT=$RACINE/captures/tests_autonomes_20260828

lancer() {
    local nom=$1 duree=$2; shift 2
    echo "  -> $nom (${duree}s) $*"
    ( export "$@" 2>/dev/null || true
      timeout $((duree + 90)) "$BIN" --mode=native-stream --duration="$duree" --activity \
          --bitrate=50 --fps=120 \
          --label="$nom" --output="$OUT/$nom.json" ) > "$OUT/$nom.out" 2>&1
    cp -f /tmp/halyard/halyard.log "$OUT/$nom.halyard.log" 2>/dev/null
    python3 - "$OUT/$nom.json" <<'PY' 2>/dev/null || echo "     (no JSON)"
import json,sys
d=json.load(open(sys.argv[1])); v=d.get("video",{}); p=d.get("perte",{})
s=max(d.get("session_seconds",1),1)
print("     pps=%.0f kbps=%.0f fps=%.1f img=%d | manques=%s/%s (%.2f%%) "
      "nack=%s retrans=%s queues=%s tard=%s trunc=%s"
      % (v.get("total_packets",0)/s, v.get("avg_kbps",0), v.get("fps",0),
         v.get("frames_decoded",0), p.get("chunks_missing",0), p.get("chunks_expected",0),
         100.0*p.get("chunks_missing",0)/max(p.get("chunks_expected",1),1),
         p.get("nack_sent","?"), p.get("chunks_redundant","?"),
         p.get("chunks_tail_recovered","?"), p.get("chunks_late","?"),
         p.get("frames_dropped_trunc","?")))
PY
    sleep 8
}
echo "=== A/B V10 corrige — 3+3 alternes ==="
for i in 1 2 3; do
    lancer "v10c_avant_$i" 90 SHADOW_UDP_RCVBUF=65536 SHADOW_NACK_AT_GAP=0 SHADOW_NACK_TICK=1
    lancer "v10c_apres_$i" 90 SHADOW_UDP_RCVBUF=65536
done
echo "=== TERMINE ==="
