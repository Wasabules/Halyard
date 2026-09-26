#!/usr/bin/env bash
# campaign_latency.sh - repeated measurements of the native path, with an A/B.
#
# === WHY THIS TOOL EXISTS (S92, 2026-08-29) ===
#
# `bench_runner.py` already compares two states of the repo by applying a patch
# and removing it. It answers "does this CODE change improve anything". That is
# not the question here: the hunt for latency is run by flipping ENVIRONMENT
# VARIABLES - the repo has ~120 of them, put there precisely so an experiment
# needs no recompilation.
#
# So this tool does the other half: the same binary, two sets of toggles,
# several alternating samples.
#
# === WHY ALTERNATE RATHER THAN GROUP ===
#
# A session's variance is ENORMOUS (`project_iter_2026-05-06_bandwidth_caps`):
# the server's bitrate, the VM's load, network congestion - everything moves from
# one minute to the next. Three "before" sessions followed by three "after" ones
# therefore measure the passage of time as much as the change. So we alternate:
#   before, after, before, after, ...
# and we compare MEDIANS, not means - one degraded session drags a
# mean around without saying anything representative.
#
# THREE SAMPLES MINIMUM per arm. It is the repo's rule, and it comes from a whole
# campaign concluded backwards on a single run.
#
# === THE TOKEN ===
#
# `/tmp` gets cleaned, and the auth token with it. `tools/` keeps a copy in
# `.token/` (outside git): it is restored automatically rather than asking for an
# interactive login in the middle of a campaign.
#
# Usage:
#   tools/campaign_latency.sh 3 30                          # 3 x 30 s, defaults
#   tools/campaign_latency.sh 3 30 "SHADOW_X=1"             # 3 A/B pairs
#   tools/campaign_latency.sh 3 30 "SHADOW_X=1 SHADOW_Y=0" campaign-name
set -uo pipefail
cd "$(dirname "$0")/.."

SAMPLES="${1:-3}"
DURATION="${2:-30}"
TREATMENT="${3:-}"
NAME="${4:-latence_$(date +%Y%m%d_%H%M%S)}"

BIN=build_linux/halyard-cli
DATA=/tmp/halyard
OUT_DIR="captures/campagnes/$NAME"

[ -x "$BIN" ] || { echo "binary missing: $BIN - build halyard-cli"; exit 1; }
mkdir -p "$OUT_DIR"

# --- The token, restored without asking -------------------------------------
mkdir -p "$DATA"
for f in refresh_token device.uuid; do
    if [ ! -s "$DATA/$f" ] && [ -s ".token/$f" ]; then
        cp -p ".token/$f" "$DATA/$f"
        echo "  token restored from .token/$f"
    fi
done
if [ ! -s "$DATA/refresh_token" ]; then
    echo "NO TOKEN. Run the desktop client once and log in:"
    echo "  cd build_linux && ./halyard"
    exit 1
fi

# --- One session -------------------------------------------------------------
# Returns one line of measurements. The log is kept: it is what will carry the
# per-stage times once the instrumentation is in place, and throwing it away now
# would force the campaign to be run again.
session() {
    local arm=$1 idx=$2 env_extra=$3
    local base="$OUT_DIR/${arm}_${idx}"

    rm -f "$DATA/halyard.log"
    # shellcheck disable=SC2086
    env $env_extra timeout $((DURATION + 90)) "$BIN" --mode=native-stream \
        --duration "$DURATION" > "$base.json" 2>&1
    [ -f "$DATA/halyard.log" ] && cp "$DATA/halyard.log" "$base.log"

    python3 - "$base.json" "$base.log" "$arm" "$idx" <<'PY'
import json, re, sys, statistics
json_path, log_path, arm, idx = sys.argv[1:5]

# The stream holds several JSON objects; the LAST one carries the summary. Taking
# the first would give the bootstrap's measurements, not the session's.
t = open(json_path, encoding='utf-8', errors='replace').read()
dec, objs, i = json.JSONDecoder(), [], 0
while i < len(t):
    j = t.find('{', i)
    if j < 0: break
    try:
        o, end = dec.raw_decode(t[j:]); objs.append(o); i = j + end
    except Exception: i = j + 1

if not objs:
    print('  %-10s #%s  FAILED - no measurement' % (arm, idx)); sys.exit(0)
d = objs[-1]
v = d.get('video', {}); p = d.get('loss', {}); n = d.get('nal', {})

# The per-stage times will come from the log (S92 instrumentation). Until they
# are there the column stays EMPTY rather than reading zero: an absent
# measurement and a zero measurement must not be confused.
# === WHAT IS ACTUALLY BEING MEASURED ===
#
# The first version of this harness only read fps, bitrate and loss. On the first
# real A/B (L6, polling against sleeping), all three returned "indistinguishable"
# while the change cut 33 % of the reassembly time. That is logical, and
# it had to be noticed: bitrate and frame rate measure THROUGHPUT, not LATENCY.
# A stream can arrive at the same rate while being systematically
# late.
#
# So we read L5's stages, which are the repo's only latency measurement. Each
# stage returns the MEDIAN of its p50 and p99 across the session's windows: the
# p99 matters as much as the p50 - a good mean latency with a 200 ms tail FEELS
# bad, and the mean alone hides it.
stages = {}
try:
    txt = open(log_path, encoding='utf-8', errors='replace').read()
    raw = {}
    for m in re.finditer(r'\[L5\]\s+(\S+)\s+n=\d+\s+avg=[\d.]+\s+p50=([\d.]+)'
                         r'\s+p90=[\d.]+\s+p99=([\d.]+)', txt):
        name = m.group(1)
        raw.setdefault(name, {'p50': [], 'p99': []})
        raw[name]['p50'].append(float(m.group(2)))
        raw[name]['p99'].append(float(m.group(3)))
    # `v` already carries the session's video stats: reusing it here as
    # a loop variable OVERWROTE it, and the fps / bitrate / frames columns
    # then showed 0.00 for every arm. Campaign L10 ran
    # that way - ten minutes of measurements whose summary announced "0 frame
    # decoded" while the JSON carried 2507. A counter that is DISPLAYED is not
    # not a counter that is READ.
    for name, st in raw.items():
        stages[name + '.p50'] = statistics.median(st['p50'])
        stages[name + '.p99'] = statistics.median(st['p99'])
except Exception:
    pass

line = ('  %-10s #%s  fps=%5.2f  bitrate=%7.1f kb/s  frames=%4d  lost=%d/%d  '
         'orph=%d  trunc=%d  idr=%d  bootstrap=%d ms'
         % (arm, idx, v.get('fps', 0), v.get('avg_kbps', 0),
            v.get('frames_decoded', 0), p.get('chunks_missing', 0),
            p.get('chunks_expected', 0), p.get('chunks_orphan', 0),
            p.get('frames_dropped_trunc', 0), n.get('idr_top', 0),
            d.get('bootstrap_ms', 0)))
if stages:
    line += '  |  ' + ' '.join('%s=%.1fms' % (k, x) for k, x in stages.items())
print(line)

with open(json_path + .measure', 'w') as f:
    json.dump({'arm': arm, 'idx': idx, 'fps': v.get('fps', 0),
               'kbps': v.get('avg_kbps', 0), 'frames': v.get('frames_decoded', 0),
               'missing': p.get('chunks_missing', 0),
               'expected': p.get('chunks_expected', 0),
               'tronq': p.get('frames_dropped_trunc', 0),
               'bootstrap': d.get('bootstrap_ms', 0), 'stages': stages}, f)
PY
}

echo "=== campaign \"$NAME\" - $SAMPLES sample(s) of ${DURATION}s ==="
[ -n "$TREATMENT" ] && echo "    treatment: $TREATMENT" || echo "    (baseline only)"
echo

for i in $(seq 1 "$SAMPLES"); do
    session baseline "$i" ""
    if [ -n "$TREATMENT" ]; then
        session treatment "$i" "$TREATMENT"
    fi
    # The server holds the slot for a few seconds after a disconnection; without
    # this pause the next session returns HTTP 409 and the sample is lost.
    sleep 8
done

echo
python3 - "$OUT_DIR" <<'PY'
import glob, json, os, statistics, sys
out_dir = sys.argv[1]
per_arm = {}
for f in glob.glob(os.path.join(out_dir, '*.json.measure')):
    m = json.load(open(f))
    per_arm.setdefault(m['arm'], []).append(m)

if not per_arm:
    print('no usable measurement'); sys.exit(0)

print('=== BILAN (medianes) ===')
print('%-12s %6s %5s %9s %8s %9s %10s' % ('arm', 'n', 'fps', 'kb/s', 'trunc', 'loss %', 'bootstrap'))
med = {}
for arm in sorted(per_arm):
    e = per_arm[arm]
    def m(key): return statistics.median([x[key] for x in e])
    loss = statistics.median([100.0 * x['missing'] / max(x['expected'], 1) for x in e])
    med[arm] = {'fps': m('fps'), 'kbps': m('kbps'), 'tronq': m('tronq'),
                 'loss': loss, 'bootstrap': m('bootstrap')}
    print('%-12s %6d %5.1f %9.0f %8.0f %9.3f %9.0fms'
          % (arm, len(e), med[arm]['fps'], med[arm]['kbps'],
             med[arm]['tronq'], loss, med[arm]['bootstrap']))

if 'baseline' in med and 'treatment' in med:
    a, b = med['baseline'], med['treatment']
    print()
    print('=== VERDICT ===')
    for key, sign in (('fps', +1), ('kbps', +1), ('tronq', -1), ('loss', -1),
                      ('bootstrap', -1)):
        if a[key] == 0:
            continue
        d = 100.0 * (b[key] - a[key]) / a[key]
        # The 3 % threshold is not caution: a session's variance is well above
        # it, and a smaller gap cannot be told from
        # noise over three samples.
        avis = 'MIEUX' if d * sign > 3 else ('PIRE' if d * sign < -3 else 'indistinct')
        print('  %-10s %+7.1f %%   %s' % (key, d, avis))
    stages = sorted({k for a in ('baseline', 'treatment') for k in
                     (per_arm[a][0].get('stages') or {})})
    if stages:
        print()
        print('=== LATENCY STAGES (medians, ms) - this is where it is decided ===')
        print('%-22s %10s %10s %9s' % ('stage', 'baseline', 'treatment', 'delta'))
        for e in stages:
            va = [x['stages'][e] for x in per_arm['baseline'] if e in (x['stages'] or {})]
            vb = [x['stages'][e] for x in per_arm['treatment'] if e in (x['stages'] or {})]
            if not va or not vb:
                continue
            ma, mb = statistics.median(va), statistics.median(vb)
            d = 100.0 * (mb - ma) / ma if ma else 0.0
            print('%-22s %10.2f %10.2f %+8.1f %%' % (e, ma, mb, d))

    print()
    print('  Reminder: three samples only distinguish a CLEAR gap. A')
    print('  "indistinguishable" does not mean "no effect" - it means')
    print('  "not measurable this way".')
PY
echo
echo "journaux et mesures : $OUT_DIR"
