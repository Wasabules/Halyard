#!/usr/bin/env bash
# Campaign 9 — does the server grant TCP for the video on this account?
#
# K15 identified the field (StreamingProtocol.f3 = NetworkType, TCP=1) and
# `SHADOW_VIDEO_NET_TCP=1` emits it. We do NOT know how to receive STFP, so the
# session will have no picture — that is not the point. The point is the server's
# REPLY: its grant carries the transport granted and the port (f3 and f5).
#
# CRITERE ECRIT AVANT :
#   - a video grant WITHOUT f3      -> the server DOWNGRADED to UDP  (a fallback)
#   - a video grant WITH `18 01`    -> the server GRANTS TCP         (worth porting)
#   - no grant / a failed bootstrap -> the request is refused outright
set -u
cd "$(dirname "$0")/../.."
RACINE=$PWD
BIN=$RACINE/build_linux/halyard-cli
OUT=$RACINE/captures/tests_autonomes_20260828

lancer() {
    local nom=$1; shift
    echo "  -> $nom  $*"
    : > /tmp/halyard/halyard.log 2>/dev/null
    ( export SHADOW_VIDEO_TCP=0 "$@" 2>/dev/null || true
      timeout 120 "$BIN" --mode=native-stream --duration=35 --activity \
          --label="$nom" --output="$OUT/$nom.json" ) > "$OUT/$nom.out" 2>&1
    cp -f /tmp/halyard/halyard.log "$OUT/$nom.halyard.log" 2>/dev/null
    awk '/\[K12\] accord/{
            sub(/.*hex=/,""); n++
            printf("     accord #%d : %s\n", n, substr($0,1,96))
         }
         /bootstrap|M11|NATIVE result/{r=$0}
         END{ if(!n) print "     (aucun accord lu)"; }' "$OUT/$nom.halyard.log"
    awk '/NATIVE result/{print "     "substr($0,10,120)}' "$OUT/$nom.out"
    sleep 8
}

echo "=== TEMOIN — demande UDP (defaut) ==="
lancer "tcp_temoin_udp"
echo "=== ESSAI — demande TCP (StreamingProtocol.f3 = 1) ==="
lancer "tcp_demande_tcp" SHADOW_VIDEO_NET_TCP=1
echo "=== TERMINE ==="
