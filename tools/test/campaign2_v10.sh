#!/usr/bin/env bash
# Campaign 2 — an A/B of the retransmission (V10) under HEAVIER loss.
#
# Campaign 1 showed that SHADOW_UDP_RCVBUF alone is not enough on this link:
# 39 then 51 missing chunks out of ~25,000, i.e. 0.16 %, and ALWAYS a single
# chunk per picture (frames_miss1 == chunks_missing). At that level no picture
# is dropped and the A/B can show nothing.
#
# So we make the bursts bigger: the bitrate and frame rate requested at the ceiling,
# the receive buffer at its lowest. It is the same mechanism as N56.
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
d=json.load(open(sys.argv[1]))
v=d.get("video",{}); p=d.get("perte",{}); s=max(d.get("session_seconds",1),1)
print("     pps=%.0f kbps=%.0f fps=%.1f img=%d | manques=%s/%s (%.2f%%) miss1=%s "
      "retrans=%s queues=%s tard=%s orph=%s nack=%s trunc=%s"
      % (v.get("total_packets",0)/s, v.get("avg_kbps",0), v.get("fps",0),
         v.get("frames_decoded",0), p.get("chunks_missing",0), p.get("chunks_expected",0),
         100.0*p.get("chunks_missing",0)/max(p.get("chunks_expected",1),1),
         p.get("frames_miss1","?"), p.get("chunks_redundant","?"),
         p.get("chunks_tail_recovered","?"), p.get("chunks_late","?"),
         p.get("chunks_orphan","?"), p.get("nack_sent","?"),
         p.get("frames_dropped_trunc","?")))
PY
    sleep 8
}

echo "=== A/B V10 — 3+3 alternes, debit 50 Mbps / 120 im/s, rcvbuf 64 KB ==="
for i in 1 2 3; do
    lancer "v10b_avant_$i" 90 SHADOW_UDP_RCVBUF=65536 SHADOW_NACK_AT_GAP=0 SHADOW_NACK_TICK=1
    lancer "v10b_apres_$i" 90 SHADOW_UDP_RCVBUF=65536
done
echo "=== TEMOIN — retransmission entierement coupee ==="
lancer "v10b_sans_nack" 90 SHADOW_UDP_RCVBUF=65536 SHADOW_NACK=0
echo "=== TERMINE ==="
