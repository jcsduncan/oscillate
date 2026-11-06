#!/usr/bin/env python3
"""Repeat the existing mixed-project 32-track rehearsal on disposable sinks.

This is a development rehearsal. Its PipeWire adapter has no attributable
per-stream xrun count, and a null sink cannot qualify a physical device.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RESULT = re.compile(r"^live_result (.+)$", re.MULTILINE)
MONITOR = re.compile(
    r"^monitor_frames=(\d+) monitor_peak=([\d.]+) "
    r"active_20ms_windows=(\d+) disposable_sink_cleaned=true$",
    re.MULTILINE,
)


def parse_run(output: str, frames: int, blocks: int, warmup: int) -> dict:
    match = RESULT.search(output)
    monitor = MONITOR.search(output)
    if match is None or monitor is None:
        raise RuntimeError("the existing live harness omitted result or cleaned-monitor evidence")
    fields = dict(re.findall(r"(\w+)=([^\s]+)", match.group(1)))
    required = {
        "requested_frames": frames,
        "observed_frames": frames,
        "client_rate": 48_000,
        "graph_rate": 48_000,
        "target_frames": (blocks + warmup) * frames,
        "vst3_insert": 1,
        "vst3_instrument": 1,
        "render_failures": 0,
        "missing_frames": 0,
        "worker_failed": 0,
        "vst3_fault": 0,
        "oversized_blocks": 0,
        "invalid_buffers": 0,
        "callback_over_period_blocks": 0,
    }
    for key, expected in required.items():
        if int(fields.get(key, -1)) != expected:
            raise RuntimeError(f"unexpected {key}: {fields.get(key)} (expected {expected})")
    if int(fields.get("completed_frames", -1)) < required["target_frames"]:
        raise RuntimeError("transport stopped before target frames")
    if int(fields.get("measured_callback_blocks", 0)) < blocks:
        raise RuntimeError("insufficient measured callback blocks")
    if "xruns" not in fields:
        raise RuntimeError("backend xrun availability was not reported")
    if fields["xruns"] != "unavailable" and int(fields["xruns"]) != 0:
        raise RuntimeError("backend reported an xrun")
    return {
        "frames": frames,
        "callback_blocks": int(fields["measured_callback_blocks"]),
        "over_period_blocks": int(fields["callback_over_period_blocks"]),
        "max_callback_duration_ns": int(fields["max_callback_duration_ns"]),
        "source_missing_frames": int(fields["missing_frames"]),
        "render_failures": int(fields["render_failures"]),
        "plugin_faults": int(fields["vst3_fault"]),
        "backend_xruns": None if fields.get("xruns") == "unavailable" else int(fields["xruns"]),
        "monitor_frames": int(monitor.group(1)),
        "monitor_peak": float(monitor.group(2)),
        "monitor_active_20ms_windows": int(monitor.group(3)),
        "null_sink_cleaned": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--executable",
        type=Path,
        default=REPO / "build/g7-mixed/oscillate_streaming_32track_rehearsal",
    )
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--blocks", type=int, default=2_000)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.repetitions < 1 or args.blocks < 1 or args.warmup < 1:
        parser.error("repetitions, blocks and warmup must be positive")
    if not args.executable.is_file():
        parser.error("build the opt-in Release qualification target first")
    scratch = (REPO / "build/g7-mixed/scratch").resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(scratch))
    runs = []
    for frames in (64, 128):
        for repetition in range(1, args.repetitions + 1):
            command = [
                sys.executable,
                str(REPO / "tests/qualification/qualify_live_streaming_32track.py"),
                "--executable",
                str(args.executable.resolve()),
                "--frames",
                str(frames),
                "--blocks",
                str(args.blocks),
                "--warmup",
                str(args.warmup),
                "--vst3-insert",
                "--vst3-instrument",
            ]
            start = time.monotonic()
            completed = subprocess.run(
                command,
                cwd=REPO,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=90,
                check=False,
            )
            if completed.returncode:
                raise RuntimeError(
                    f"{frames}-frame repetition {repetition} failed:\n{completed.stdout}"
                )
            run = parse_run(completed.stdout, frames, args.blocks, args.warmup)
            run["repetition"] = repetition
            run["elapsed_seconds"] = round(time.monotonic() - start, 3)
            runs.append(run)
            print(
                f"frames={frames} repetition={repetition} "
                f"callback_max_ns={run['max_callback_duration_ns']} "
                f"over_period={run['over_period_blocks']} "
                f"backend_xruns={run['backend_xruns']}",
                flush=True,
            )
    evidence = {
        "kind": "virtual-device development rehearsal; not G7 release qualification",
        "workload": (
            "32 managed audio tracks; one synthetic VST3 effect; one synthetic VST3 MIDI "
            "instrument; two groups; two returns; 33 sends; native master "
            "EQ/compressor/reverb/limiter; send automation"
        ),
        "sample_rate": 48_000,
        "blocks_per_run": args.blocks,
        "warmup_blocks_per_run": args.warmup,
        "backend_xrun_evidence": "unavailable"
        if any(run["backend_xruns"] is None for run in runs)
        else "reported",
        "physical_device": False,
        "real_third_party_plugins": False,
        "runs": runs,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".pending")
    temporary.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    temporary.replace(args.output)
    print(f"evidence={args.output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"mixed virtual rehearsal failed: {error}", file=sys.stderr)
        sys.exit(1)
