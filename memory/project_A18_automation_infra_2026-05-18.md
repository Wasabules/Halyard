---
name: project-A18-automation-infra-2026-05-18
description: "V18 automation infrastructure, 2026-05-18 — A1/A2/A3/A4 delivered. shadow-test-cli supports 4 modes (webrtc/native-smoke/native-stream/health-check) with a unified JSON schema. tools/auto-test.sh = one command to build+test+verdict. bench_runner.py --mode for an A/B on the native path. The health check validated live: fps=32.5, decrypt=100%, bottom=49.77% → PASS, exit 0."
metadata:
  node_type: memory
  type: project
---

# Automation infrastructure — 1-commande post-patch validation

## What was delivered

### A1 — JSON dump natif (`main_test.c`)
The `native-stream` mode now dumps structured JSON:
```json
{
  "label", "mode", "duration_requested_s", "bootstrap_ms", "bootstrap_ok",
  "stream_started", "exit_reason", "session_seconds",
  "video": {"avg_kbps", "total_packets", "total_bytes", "frames_decoded",
            "frames_displayed", "fps", "resolution"},
  "crypto": {"decrypt_ok", "decrypt_fail", "decrypt_pct"},
  "nal": {"top", "bottom", "idr_top", "idr_bottom", "bottom_pct"},
  "reasm": {"parity_skip", "abandoned"}
}
```
+ `port_base` computed from `conn.port + 7000` (= the pre-A18 bug: main_test.c passed port_base=0 and the bootstrap failed immediately).

### A2 — Mode `health-check` avec thresholds
Nouveau mode `--mode=health-check` :
- Duration defaults to 30 s (= short, ideal for CI)
- Validates :
  - bootstrap_ok=true
  - fps ≥ `--threshold-fps=20`
  - decrypt_pct ≥ `--threshold-decrypt=95`
  - bottom_pct ≥ `--threshold-bottom=30`
- Exit code 0 = PASS, 6 = FAIL
- JSON enrichi avec `health.{pass, reason, threshold_*}` field

Associated fix: `signal(SIGALRM, on_sigint)` so that `alarm()` sets g_abort instead of the default kill.

### A3 — `bench_runner.py --mode`
- Nouveau arg `--mode=webrtc|native-stream|native-smoke|health-check`
- `aggregate()` route schema-dependent (webrtc vs native)
- `fmt_table()` displays the metrics relevant to each mode (fps + bottom_pct for native)
- `diff()` compare keys pertinents par mode
- Support `health-check` : count `health_pass_count/n_valid` dans agg

Usage A/B natif :
```bash
python3 tools/bench_runner.py --mode=native-stream --runs 3 --duration 60 \
    --treatment-patch fix-X.patch
```

### A4 — `tools/auto-test.sh`
1-commande wrapper build + run + parse + verdict :
```bash
tools/auto-test.sh                       # health-check 30s defaults
tools/auto-test.sh 60                    # 60s
tools/auto-test.sh 30 native-stream      # mesure sans verdict
tools/auto-test.sh 30 health-check 25 98 40  # custom thresholds
```
Exit codes : 0=PASS, 1=bootstrap, 2=native fail, 3=webrtc fail, 5=abort, 6=health FAIL, 20=build fail.

## Validation live 2026-05-18 ~01:36

A real post-A4 run:
```
== VERDICT ==
  session_seconds = 28
  frames_decoded  = 910
  fps             = 32.5   (min=20)
  decrypt %       = 100.0  (min=95%)
  bottom NAL %    = 49.77  (min=30%)
  exit_code       = 0
  ✅ HEALTH CHECK PASSED
```

The V14 stack (PARITY_RAW + IFR + gE) confirmed OK on the Linux desktop. Bottom 49.77% = the 50% target ratio (= V11's top/bottom parity).

## Comment l'utiliser maintenant

### Post-patch quick smoke test (= ~90s total)
```bash
cd $REPO
tools/auto-test.sh                       # build + 30s test + verdict
```

### A/B testing d'un patch (= bench complet ~5-10 min)
```bash
cd 05-shadow-client-borealis
python3 tools/bench_runner.py \
    --mode=native-stream --runs 3 --duration 60 \
    --treatment-patch /tmp/my-experiment.patch
```

### Just measure native baseline
```bash
tools/auto-test.sh 60 native-stream
cat /tmp/auto-test-*.json   # raw stats
```

### Custom thresholds (= si V16 push bottom_pct > 60% par exemple)
```bash
tools/auto-test.sh 30 health-check 25 98 50   # threshold-fps=25, decrypt=98%, bottom=50%
```

## Limitations restantes

1. **Switch automation**: not integrated yet. tools/switch-sync.sh exists but is not coupled to auto-test. To get `auto-test --switch` you need an FTP push + a remote run + a log fetch + parsing → a large workflow (cf. the audit, §3.6).
2. **VST monitoring**: the `vst:` logs are not in the JSON. To validate that the VST channel works, you have to grep webrtc.log by hand. To be added in a future iteration if needed (= VST stats in ctrl_session_glue_stats).
3. **Visual frame validation**: SHADOW_FRAME_DUMP exists but is not exposed through auto-test. To diff PPMs before/after a patch, an additional wrapper is needed.
4. **Multi-VM parallel A/B** : single account = single VM concurrent. ROI faible.

## Cross-refs

- [[project-V18-cleanup-baseline-2026-05-18]] — the pre-A18 cleanup (= abandoned toggles removed)
- [[project-V16-vst-plaintext-FOUND-2026-05-18]] — VST byte-exact (= prochain candidate test A/B)
- `tools/auto-test.sh` (= NEW)
- `05-shadow-client-borealis/tools/bench_runner.py` (= extended with --mode)
- `05-shadow-client-borealis/demo/src/main_test.c` (= extended with native JSON + a health check)
- `05-shadow-client-borealis/demo/src/streaming/ctrl_session_glue.{h,c}` (= NAL stats exposed)

## Status

✅ DONE. The automation infrastructure is ready. `tools/auto-test.sh` = "validate after any patch" in one command.
