#!/usr/bin/env python3
"""bench_runner - an autonomous A/B test orchestrator for halyard-cli.

Usage typique :
    # Baseline = HEAD courant, treatment = patch en plus
    python3 tools/client/bench_runner.py --baseline-label "current" --runs 3 --duration 90 --activity

    # Compare baseline vs treatment (patch tampon)
    python3 tools/client/bench_runner.py --treatment-patch some.patch --runs 3 --duration 90

    # Single run, dump raw JSON
    python3 tools/client/bench_runner.py --runs 1 --duration 60 --output one_shot.json

Le script :
  1. Build halyard-cli (make depuis build_linux/)
  2. Runs N samples on the current code -> baseline
  3. Si --treatment-patch : git apply, rebuild, relance N runs → treatment
  4. Restore git state
  5. Compare baseline vs treatment, print table + final verdict

Prerequisite: the data directory's refresh_token (see core/services/config.h) must
exist (login already done through halyard). The test-cli reuses it
silencieusement.
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from statistics import mean, median
from typing import List, Optional

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build_linux"
TEST_BIN = BUILD_DIR / "halyard-cli"
RESULTS_DIR = Path("/tmp/shadow-bench")
RESULTS_DIR.mkdir(parents=True, exist_ok=True)


def run(cmd, **kw):
    """Runs a command, raises if exit != 0 unless check=False."""
    print(f"  $ {' '.join(str(c) for c in cmd)}")
    return subprocess.run(cmd, **kw)


def build():
    """Rebuild halyard-cli depuis build_linux/."""
    print("== build halyard-cli ==")
    r = run(["make", "-j", "-C", str(BUILD_DIR), "halyard-cli"],
            capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise RuntimeError("build failed")
    print("  build OK")


def single_run(label: str, duration: int, activity: bool, vm_id: Optional[str],
               mode: str = "native-stream") -> dict:
    """Runs halyard-cli once, returns the parsed JSON.

    Supported modes (= A3, 2026-05-18):
      native-stream — the native pipeline (schema video.{avg_kbps, fps} + nal.{bottom_pct})
      native-smoke  — bootstrap validation only (exit 0/2)
      health-check  — native-stream 30s + thresholds (exit 0/6)
    (A `webrtc` mode and its schema existed until the WebRTC path was deleted,
    2026-09-26.)
    """
    out_path = RESULTS_DIR / f"{label}_{int(time.time())}.json"
    args = [str(TEST_BIN),
            f"--duration={duration}",
            f"--output={out_path}",
            f"--label={label}",
            f"--mode={mode}",
            "--quiet"]
    if activity:
        args.append("--activity")
    if vm_id:
        args.append(f"--vm-id={vm_id}")
    print(f"  → run {label}, mode={mode}, duration={duration}s, activity={activity}")
    t0 = time.time()
    r = subprocess.run(args, timeout=duration + 120)
    dt = time.time() - t0
    print(f"    exit={r.returncode} elapsed={dt:.1f}s out={out_path}")
    if not out_path.exists():
        return {"error": "no_output", "exit_code": r.returncode, "_mode": mode}
    with open(out_path) as f:
        data = json.load(f)
    data["_elapsed_wall"] = dt
    data["_exit_code"] = r.returncode
    data["_path"] = str(out_path)
    data["_mode"] = mode
    return data


def aggregate(runs: List[dict]) -> dict:
    """Aggregates several runs into average stats (the native schema)."""
    if not runs:
        return {}
    valid = [r for r in runs if "video" in r and "avg_kbps" in r["video"]]
    if not valid:
        return {"n_runs": len(runs), "n_valid": 0, "errors": runs}
    mode = valid[0].get("_mode", "native-stream")
    out = {
        "n_runs": len(runs),
        "n_valid": len(valid),
        "mode": mode,
        "video_avg_kbps_mean": mean(r["video"]["avg_kbps"] for r in valid),
        "video_avg_kbps_med":  median(r["video"]["avg_kbps"] for r in valid),
        "frames_decoded_mean": mean(r["video"]["frames_decoded"] for r in valid),
        "bootstrap_ms_mean":   mean(r.get("bootstrap_ms", 0) for r in valid),
    }
    # native-stream / health-check schema
    out.update({
        "fps_mean":         mean(r["video"]["fps"]            for r in valid),
        "bottom_pct_mean":  mean(r["nal"]["bottom_pct"]       for r in valid),
        "decrypt_pct_mean": mean(r["crypto"]["decrypt_pct"]   for r in valid),
        "nal_top_mean":     mean(r["nal"]["top"]              for r in valid),
        "nal_bottom_mean":  mean(r["nal"]["bottom"]           for r in valid),
        "session_seconds_mean": mean(r.get("session_seconds", 0) for r in valid),
    })
    # health-check : count des PASS
    if mode == "health-check":
        out["health_pass_count"] = sum(1 for r in valid if r.get("health", {}).get("pass"))
        out["health_pass_rate"]  = out["health_pass_count"] / out["n_valid"]
    return out


def fmt_table(label: str, agg: dict) -> str:
    if not agg or "video_avg_kbps_mean" not in agg:
        return f"  {label}: NO VALID DATA ({agg})"
    mode = agg.get("mode", "native-stream")
    head = f"  {label} (n={agg['n_valid']}/{agg['n_runs']}, mode={mode}):"
    extra = ""
    if mode == "health-check":
        extra = (f"\n    HEALTH    : {agg['health_pass_count']}/{agg['n_valid']} PASS "
                 f"({agg['health_pass_rate']*100:.0f}%)")
    return (f"{head}\n"
            f"    video avg : {agg['video_avg_kbps_mean']/1000:6.2f} Mbps\n"
            f"    fps       : {agg['fps_mean']:>6.2f}\n"
            f"    frames    : {agg['frames_decoded_mean']:>6.0f}\n"
            f"    bottom %  : {agg['bottom_pct_mean']:>6.2f}\n"
            f"    decrypt % : {agg['decrypt_pct_mean']:>6.2f}\n"
            f"    NAL top/b : {agg['nal_top_mean']:>6.0f} / {agg['nal_bottom_mean']:>6.0f}\n"
            f"    session_s : {agg['session_seconds_mean']:>6.1f}\n"
            f"    bootstrap : {agg['bootstrap_ms_mean']:>6.0f} ms"
            f"{extra}")


def diff(baseline_agg: dict, treatment_agg: dict) -> str:
    if not baseline_agg or not treatment_agg:
        return "  (cannot diff — missing data)"
    keys = ["video_avg_kbps_mean", "fps_mean", "bottom_pct_mean",
            "decrypt_pct_mean", "frames_decoded_mean", "nal_bottom_mean"]
    out = ["  delta (treatment vs baseline):"]
    for k in keys:
        b = baseline_agg.get(k, 0)
        t = treatment_agg.get(k, 0)
        d = t - b
        pct = (d / b * 100) if b > 0 else 0
        sign = "+" if d > 0 else ""
        out.append(f"    {k:30}: {b:9.2f} → {t:9.2f}  ({sign}{d:+9.2f}, {sign}{pct:+6.1f}%)")
    return "\n".join(out)


def git_apply_patch(patch_path: Path):
    print(f"== applying patch {patch_path} ==")
    r = run(["git", "-C", str(ROOT), "apply", str(patch_path)],
            capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        raise RuntimeError("git apply failed")


def git_revert_patch(patch_path: Path):
    print(f"== reverting patch {patch_path} ==")
    subprocess.run(["git", "-C", str(ROOT), "apply", "-R", str(patch_path)])


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--runs", type=int, default=2,
                   help="the number of runs per variant (default 2)")
    p.add_argument("--duration", type=int, default=60,
                   help="sampling duration per run, in seconds (default 60)")
    p.add_argument("--activity", action="store_true",
                   help="inject mouse moves to force some video content")
    p.add_argument("--vm-id", default=None,
                   help="an explicit VM id (default = the first one listed)")
    p.add_argument("--baseline-label", default="baseline",
                   help="label for the run on the current code")
    p.add_argument("--treatment-patch", type=Path, default=None,
                   help="the path of a git patch to apply and test as the treatment")
    p.add_argument("--treatment-label", default="treatment")
    p.add_argument("--no-build", action="store_true",
                   help="skip le rebuild initial (use binary tel quel)")
    p.add_argument("--cooldown", type=int, default=15,
                   help="seconds between runs, to let the server clean up")
    # AF13 2026-09-10: the default was "webrtc", a mode main_test.c has rejected
    # since S112 ("unknown mode") - so the documented command, run
    # without --mode, produced NO measurement at all.
    p.add_argument("--mode", default="native-stream",
                   choices=["native-stream", "native-smoke", "health-check"],
                   help="(A3 2026-05-18) pipeline mode for halyard-cli")
    args = p.parse_args()

    if not TEST_BIN.exists() and not args.no_build:
        build()

    if not args.no_build:
        build()

    print(f"\n== baseline: {args.runs} runs of {args.duration}s (mode={args.mode}) ==")
    baseline_runs = []
    for i in range(args.runs):
        r = single_run(f"{args.baseline_label}_{i+1}",
                       args.duration, args.activity, args.vm_id, args.mode)
        baseline_runs.append(r)
        if i + 1 < args.runs:
            time.sleep(args.cooldown)

    treatment_runs = []
    if args.treatment_patch:
        try:
            git_apply_patch(args.treatment_patch)
            build()
            print(f"\n== treatment: {args.runs} runs of {args.duration}s (mode={args.mode}) ==")
            for i in range(args.runs):
                r = single_run(f"{args.treatment_label}_{i+1}",
                               args.duration, args.activity, args.vm_id, args.mode)
                treatment_runs.append(r)
                if i + 1 < args.runs:
                    time.sleep(args.cooldown)
        finally:
            git_revert_patch(args.treatment_patch)
            build()  # rebuild back to baseline
    else:
        print("\n(no --treatment-patch given, baseline only)")

    print("\n" + "=" * 70)
    print("RESULTS")
    print("=" * 70)

    base_agg = aggregate(baseline_runs)
    print(fmt_table(args.baseline_label, base_agg))

    if treatment_runs:
        treat_agg = aggregate(treatment_runs)
        print()
        print(fmt_table(args.treatment_label, treat_agg))
        print()
        print(diff(base_agg, treat_agg))

    print()
    print(f"Raw JSON files in {RESULTS_DIR}/")


if __name__ == "__main__":
    main()
