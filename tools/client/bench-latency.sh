#!/usr/bin/env bash
# bench-latency.sh - the cost of DECODING and UPLOADING, offline.
#
# No session, no token, no console: everything is measured on the Annex-B
# streams already recorded (SHADOW_DUMP_H264=1).
#
#   bench_decode  - configures libavcodec EXACTLY like media/h264_decoder.c
#                   (software path), re-splits the stream into access units with
#                   streaming/vid_wire.c's G40 rule, then measures:
#                     * has_b_frames before/after open2 and at the first picture
#                     * how many units are fed BEFORE the first picture comes out
#                     * the time per unit (send_packet + receive_frame) and its
#                       dispersion (p50/p90/p95/p99/max)
#                     * how many pictures come out per unit fed in
#                   Second argument: 1 sets AV_CODEC_FLAG_LOW_DELAY.
#
#   bench_push    — reproduit StreamView::pushYuvFrame (copie du plan Y +
#                   scalar U/V interleaving) and the NV12 path.
#
# Counter-case: if `bench_decode` reports anything other than "1 picture per unit
# from the first one", the decoder is HOLDING pictures back and LOW_DELAY /
# has_b_frames become latency leads again. This measurement is what ruled them out.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/.."
data="${1:-$src/build_linux/halyard-data}"
out="${TMPDIR:-/tmp}/bench-latence"
mkdir -p "$out"

gcc -O2 -o "$out/bench_decode" "$here/bench_decode.c" \
    "$src/core/protocol/vid_wire.c" \
    -I"$src/streaming" \
    $(pkg-config --cflags --libs libavcodec libavutil) -lm
g++ -O2 -o "$out/bench_push" "$here/bench_push.cpp"

for f in stream_baseline stream_trim28; do
    [ -f "$data/$f.h264" ] || continue
    for ld in 0 1; do
        echo "########## $f  LOW_DELAY=$ld"
        "$out/bench_decode" "$data/$f.h264" "$ld" 2>/dev/null
        echo
    done
done
echo "########## upload into the display queue"
"$out/bench_push" 1920 1080 300
"$out/bench_push" 1280 720 300
