# An autonomous bandwidth/quality test workflow

Tools for measuring throughput and quality gains autonomously, with no human touching the Borealis UI.

## Prerequisites

1. `halyard` (the GUI) must have been launched at least once and the OAuth device grant completed → `refresh_token` is saved under `/tmp/halyard/refresh_token`.
2. The `halyard-cli` binary must be built (`bench_runner.py` does it automatically).
3. A Shadow VM must be available on the account (otherwise exit_reason=2).

## The tools

### `halyard-cli` — the headless runner

It does the complete bootstrap (an OAuth refresh → TINAG → list the VMs → start → the native session) then samples `session_stats_t` at 1 Hz for N seconds. It dumps a JSON with aggregated stats (avg/peak/p50/p95 Kbps, frames, etc.) plus a second-by-second timeline.

```bash
cd build_linux
./halyard-cli --duration=60 --output=run.json --label=baseline --activity
```

Flags:
- `--duration=N`: seconds of sampling (60 by default)
- `--output=PATH`: the path of the metrics JSON (`-` = stdout)
- `--label=NAME`: a label in the JSON that identifies the run
- `--activity`: injects mouse moves every second to force non-trivial video content (without it: ~500 Kbps on an idle desktop; with it: ~5-15 Mbps)
- `--vm-id=ID`: an explicit VM (the first one listed by default)
- `--quiet`: silences the `[test]` logs (useful in CI)

Exit codes:
- `0` OK, the JSON was dumped
- `1` the bootstrap failed (oauth/tinag/vm)
- `2` no VMs available
- `3` webrtc_session_open failed
- `4` a freeze was detected (webrtc set abort_flag)
- `5` the user aborted (SIGINT)

### `bench_runner.py` — the A/B orchestrator

It runs N samples on the current code (the baseline), optionally applies a git patch and runs N more (the treatment), then prints a diff.

```bash
# The baseline alone - 3 runs of 60 s with activity
python3 tools/bench_runner.py --runs 3 --duration 60 --activity

# An A/B against a patch
python3 tools/bench_runner.py --runs 3 --duration 60 --activity \
    --treatment-patch /tmp/sdp_extended.patch \
    --treatment-label "sdp_high_profile"
```

Example output:
```
== baseline: 3 runs of 60s ==
  baseline (n=3/3):
    video avg :   1.40 Mbps  peak 6.54 Mbps  p50 0.61 Mbps  p95 6.54 Mbps
    frames    :    560
    audio avg :   0.00 Mbps
    bootstrap :   4877 ms

== treatment: 3 runs of 60s ==
  sdp_high_profile (n=3/3):
    video avg :   2.10 Mbps  peak 8.20 Mbps  p50 1.10 Mbps  p95 7.50 Mbps
    ...

  delta (treatment vs baseline):
    video_avg_kbps_mean      :    1400.0 →   2100.0  (+700.0, +50.0%)
    ...
```

Flags:
- `--runs N`: the number of runs per variant (2 by default)
- `--duration N`: seconds per run (60 by default)
- `--activity`: passes `--activity` through to test-cli
- `--treatment-patch PATH`: git-applies this patch before the treatment runs
- `--cooldown N`: a pause between runs (15 s by default) to avoid server rate limiting
- `--no-build`: skips the initial rebuild

## The Claude workflow (what it does unattended)

```
1. Apply edit on sdp.c / launcher.c / etc.
2. python3 tools/bench_runner.py --runs 3 --duration 90 --activity --label-baseline "before"
3. Read output table, decide if gain is significant
4. If gain insufficient → next hypothesis
5. If gain confirmed → save memory + commit
```

## Known limitations

- **No audio in the test**: no audio source is active on the VM by default. The `audio.avg_kbps` counter is always 0 — that is expected.
- **`--activity` is minimally simulated**: it injects mouse moves (alternating by 50 px). To stress it harder, launch a game or a video through PointerEnter + a click in the centre.
- **Cleanup is non-blocking**: the WebRTC thread's `pthread_join` has a 10 s timeout, after which `_Exit()` skips the cleanup. That is deliberate — the `wss_drainer` can hang on `wss_recv` (see webrtc.c:1961).
- **A transient bitrate pattern**: the first seconds after opening peak (~6 Mbps) then drop sharply to 200-600 Kbps with no activity. To measure the steady state, use `--duration=120` at minimum and exclude the first 5 seconds.
- **Telemetry SSL**: the endpoint `prod.log.frsbg01.shadow.tech:2443` rejects the certificate (probably a custom cert). Auto-disabled after 3 failures — not blocking.
