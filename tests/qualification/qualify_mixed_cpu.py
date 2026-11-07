#!/usr/bin/env python3
"""Opt-in, no-device CPU rehearsal using the saved 32-track mixed graph."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIELDS = re.compile(r"(\w+)=([^\s]+)")


def parse_result(
    output: str,
    frames: int,
    blocks: int,
    warmup: int,
    repetitions: int,
    processed_clips: int = 0,
    native_synths: bool = False,
) -> list[dict]:
    lines = output.splitlines()
    workload = [line for line in lines if line.startswith("workload=")]
    if len(workload) != 1:
        raise RuntimeError("missing or duplicate workload record")
    observed = dict(FIELDS.findall(workload[0]))
    expected = {
        "workload": "native-synth-32-routed" if native_synths else "managed-wav-32-routed",
        "rate": "48000",
        "frames": str(frames),
        "source_tracks": "30" if native_synths else "32",
        "group_buses": "2",
        "return_buses": "2",
        "vst3_inserts": "1",
        "vst3_instruments": "0" if native_synths else "1",
        "automated_sends": "1",
        "native_instruments": "2" if native_synths else "0",
        "sends": "32" if native_synths else "33",
        "processed_clips": str(processed_clips),
        "repitch_clips": str(processed_clips // 2),
        "pitched_clips": str(processed_clips // 2),
        "warmup_blocks": str(warmup),
        "measured_blocks": str(blocks),
        "repetitions": str(repetitions),
        "pace_multiplier": "1",
        "master": "eq+compressor+reverb+limiter",
    }
    for key, value in expected.items():
        if observed.get(key) != value:
            raise RuntimeError(f"wrong workload {key}: {observed.get(key)}")
    if not any(line.startswith("vst3_probe ") for line in lines):
        raise RuntimeError("effect audio oracle is absent")
    if not native_synths and not any(line.startswith("vst3_instrument_probe ") for line in lines):
        raise RuntimeError("instrument audio oracle is absent")
    if not any(line.startswith("send_automation_probe ") for line in lines):
        raise RuntimeError("automation audio oracle is absent")
    period_us = frames / 48_000 * 1_000_000
    results = []
    for line in lines:
        if not line.startswith("run="):
            continue
        fields = dict(FIELDS.findall(line))
        try:
            run = int(fields["run"])
            timings = {
                key: float(fields[key])
                for key in ("mean_us", "p50_us", "p95_us", "p99_us", "max_us")
            }
            process_cpu_us = float(fields["process_cpu_us"])
            wall_us = float(fields["wall_us"])
            counters = {
                key: int(fields[key])
                for key in (
                    "over_period",
                    "callback_allocations",
                    "render_failures",
                    "missing_frames",
                    "worker_failed",
                    "audible_blocks",
                    "vst3_fault",
                    "processed_clips",
                    "reduced_quality_clips",
                    "quality_step_downs",
                    "synth_voice_blocks",
                )
            }
        except (KeyError, ValueError) as error:
            raise RuntimeError("incomplete run evidence") from error
        if (
            run != len(results) + 1
            or any(
                value != 0
                for key, value in counters.items()
                if key
                not in ("audible_blocks", "synth_voice_blocks", "over_period", "processed_clips")
            )
            or (counters["processed_clips"] != processed_clips)
            or not (0 < counters["audible_blocks"] <= blocks + warmup)
            or not (0 <= counters["over_period"] <= blocks)
            or (native_synths and counters["synth_voice_blocks"] == 0)
            or (not native_synths and counters["synth_voice_blocks"] != 0)
        ):
            raise RuntimeError(f"run {run} failed a callback or audible-output invariant")
        if (
            not all(map(math.isfinite, timings.values()))
            or not 0 < timings["mean_us"] <= timings["max_us"]
        ):
            raise RuntimeError(f"run {run} has invalid timing")
        if (
            not math.isfinite(process_cpu_us)
            or not math.isfinite(wall_us)
            or not (process_cpu_us > 0 and wall_us > 0)
        ):
            raise RuntimeError(f"run {run} has invalid process CPU evidence")
        if not 0 < timings["p50_us"] <= timings["p95_us"] <= timings["p99_us"] <= timings["max_us"]:
            raise RuntimeError(f"run {run} has unordered percentiles")
        results.append(
            {
                "run": run,
                **timings,
                **counters,
                "process_cpu_us": process_cpu_us,
                "wall_us": wall_us,
                "process_cpu_percent": round(100 * process_cpu_us / wall_us, 2),
                "period_us": round(period_us, 3),
                "p99_period_percent": round(100 * timings["p99_us"] / period_us, 2),
                "worst_period_percent": round(100 * timings["max_us"] / period_us, 2),
            }
        )
    if len(results) != repetitions:
        raise RuntimeError(f"expected {repetitions} runs, found {len(results)}")
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--blocks", type=int, default=2_000)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--processed-clips", type=int, choices=(0, 4), default=0)
    parser.add_argument(
        "--native-synths",
        action="store_true",
        help="add routed Prism and Ember tracks with MIDI notes",
    )
    args = parser.parse_args()
    if args.repetitions < 1 or args.blocks < 1 or args.warmup < 1:
        parser.error("repetitions, blocks and warmup must be positive")
    if (args.blocks + args.warmup) * 128 > 48_000 * 8:
        parser.error("rehearsal exceeds the managed source duration")
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error("build the opt-in scanner-enabled Release rehearsal target first")
    scratch = args.output.resolve().parent / "scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(scratch))
    results = {}
    for frames in (64, 128):
        command = [
            str(executable),
            "--frames",
            str(frames),
            "--blocks",
            str(args.blocks),
            "--warmup",
            str(args.warmup),
            "--repetitions",
            str(args.repetitions),
            "--vst3-insert",
            "1",
        ]
        if not args.native_synths:
            command.extend(("--vst3-instrument", "1"))
        if args.processed_clips:
            command.extend(("--processed-clips", str(args.processed_clips)))
        if args.native_synths:
            command.extend(("--native-synths", "1"))
        completed = subprocess.run(
            command,
            cwd=REPO,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=120,
            check=False,
        )
        if completed.returncode:
            raise RuntimeError(f"{frames}-frame rehearsal failed:\n{completed.stdout}")
        results[str(frames)] = parse_result(
            completed.stdout,
            frames,
            args.blocks,
            args.warmup,
            args.repetitions,
            args.processed_clips,
            args.native_synths,
        )
        for run in results[str(frames)]:
            print(
                f"frames={frames} run={run['run']} p99_period={run['p99_period_percent']}% "
                f"worst_period={run['worst_period_percent']}% ",
                flush=True,
            )
    evidence = {
        "kind": "fake-callback mixed-project development rehearsal; not device qualification",
        "host": platform.platform(),
        "processor": platform.processor(),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "sample_rate": 48_000,
        "blocks_per_run": args.blocks,
        "warmup_blocks_per_run": args.warmup,
        "workload": (
            "30 or 32 managed WAV tracks plus Prism/Ember or a synthetic VST3 instrument, "
            "totaling 32 or 33 source tracks; 2 groups, 2 returns, sends, automation, "
            "native master EQ/compressor/reverb/limiter, synthetic VST3 effect"
        ),
        "native_synths": ["prism", "ember"] if args.native_synths else [],
        "processed_clips": args.processed_clips,
        "processed_fixture": "2 Re-Pitch marker maps and 2 Complex clips pitched +2/-3 semitones"
        if args.processed_clips
        else None,
        "callback_allocation_probe": (
            "scalar C++ new on callback thread only; excludes other allocators and worker threads"
        ),
        "synth_audio_oracle": (
            "both routed native synth runtimes report active voices while mixed output is "
            "finite/audible; standalone synth/export parity tests carry sample-level oracles"
        )
        if args.native_synths
        else None,
        "process_cpu_measurement": (
            "std::clock() over each paced run; all process threads, including source "
            "workers, not preparation or prebuffer"
        ),
        "physical_device": False,
        "backend_xruns": None,
        "over_period_is_backend_xrun": False,
        "real_third_party_plugins": False,
        "runs": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    pending = args.output.with_suffix(args.output.suffix + ".pending")
    pending.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    pending.replace(args.output)
    print(f"evidence={args.output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"mixed CPU rehearsal failed: {error}", file=sys.stderr)
        sys.exit(1)
