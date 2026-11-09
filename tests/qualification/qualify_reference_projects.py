#!/usr/bin/env python3
"""Plays the light, heavy, pad and drums reference projects and records their cost.

Runs oscillate_reference_project for each reference and buffer size, through
the paced fake callback or, with --target-node, an explicitly named PipeWire
sink. Records callback time (p50/p99/max), whole-process CPU and, with
--trace, a per-device table from the performance trace (ADR-0061). Writes
results.json and results.md into --output.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

FIELDS = re.compile(r"(\w+)=(\S+)")
AUDIO_THREADS = ("audio callback", "audio worker")
# Spans that frame the work rather than doing it; reported apart from devices.
FRAME_SPANS = ("audio.render", "graph.node", "graph.source", "pool.wait_for_helpers")
REFERENCES = ("light", "heavy", "pad", "drums")
BUFFER_SIZES = (64, 128)
SAMPLE_RATE = 48_000
PERCENT = 100
KIB_PER_MIB = 1024
MEDIAN = 50
TAIL = 99
RUN_TIMEOUT_FACTOR = 4
RUN_TIMEOUT_MARGIN_SECONDS = 300
DEFAULT_SECONDS = 20


class RunFailed(RuntimeError):
    """A reference run that exited with an error or broke a correctness check."""


def fields(output: str, prefix: str) -> dict[str, str]:
    lines = [line for line in output.splitlines() if line.startswith(prefix + " ")]
    if len(lines) != 1:
        raise RunFailed(f"expected one {prefix!r} line, found {len(lines)}")
    return dict(FIELDS.findall(lines[0]))


def numeric(values: dict[str, str]) -> dict:
    out = {}
    for key, value in values.items():
        try:
            out[key] = int(value)
        except ValueError:
            try:
                out[key] = float(value)
            except ValueError:
                out[key] = value
    return out


def percentile(values: list[float], numerator: int) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, -(-len(ordered) * numerator // PERCENT) - 1)
    return ordered[min(index, len(ordered) - 1)]


def trace_tables(trace: dict) -> dict:
    """Per-device cost and, when the trace has device callbacks, their timing."""
    threads = {
        event["tid"]: event["args"]["name"]
        for event in trace["traceEvents"]
        if event.get("ph") == "M"
    }
    spans = [
        event
        for event in trace["traceEvents"]
        if event.get("ph") == "X" and threads.get(event["tid"]) in AUDIO_THREADS
    ]
    if not spans:
        return {"devices": [], "callbacks": None, "window_us": 0}
    window_us = max(span["ts"] + span["dur"] for span in spans) - min(span["ts"] for span in spans)
    by_kind: dict[str, list[dict]] = collections.defaultdict(list)
    for span in spans:
        by_kind[span["args"]["kind"]].append(span)
    devices = []
    for kind, found in by_kind.items():
        if kind in FRAME_SPANS:
            continue
        durations = [span["dur"] for span in found]
        devices.append(
            {
                "device": kind,
                "instances": len({span["args"]["n"] for span in found}),
                "core_percent": round(PERCENT * sum(durations) / window_us, 3) if window_us else 0,
                "mean_us": round(sum(durations) / len(durations), 3),
                "p99_us": round(percentile(durations, TAIL), 3),
                "max_us": round(max(durations), 3),
            }
        )
    devices.sort(key=lambda row: -row["core_percent"])
    renders = [span["dur"] for span in by_kind.get("audio.render", [])]
    callbacks = (
        {
            "blocks": len(renders),
            "p50_us": round(percentile(renders, MEDIAN), 3),
            "p99_us": round(percentile(renders, TAIL), 3),
            "max_us": round(max(renders), 3),
        }
        if renders
        else None
    )
    xruns = sum(
        1
        for event in trace["traceEvents"]
        if event.get("ph") == "i" and event["name"] in ("audio.xrun", "audio.xrun_recovery")
    )
    return {
        "devices": devices,
        "callbacks": callbacks,
        "xrun_events": xruns,
        "window_us": window_us,
    }


def summarise(output: str, trace: dict | None) -> dict:
    workload = numeric(fields(output, "workload"))
    result = numeric(fields(output, "result"))
    summary = {"workload": workload, "result": result}
    if trace is not None:
        summary["trace"] = trace_tables(trace)
        callbacks = summary["trace"]["callbacks"]
        if result["mode"] == "live" and callbacks:
            result.update(
                p50_us=callbacks["p50_us"], p99_us=callbacks["p99_us"], max_us=callbacks["max_us"]
            )
    if "p99_us" in result:
        result["p99_period_percent"] = round(PERCENT * result["p99_us"] / result["period_us"], 2)
    result["max_period_percent"] = round(PERCENT * result["max_us"] / result["period_us"], 2)
    return summary


def sink_serial(node_name: str) -> int:
    """The object serial of the named ALSA sink, never a default device."""
    nodes = json.loads(subprocess.run(["pw-dump"], check=True, capture_output=True).stdout)
    for node in nodes:
        props = node.get("info", {}).get("props", {})
        if (
            node.get("type") == "PipeWire:Interface:Node"
            and props.get("node.name") == node_name
            and props.get("media.class") == "Audio/Sink"
        ):
            return int(props["object.serial"])
    raise RunFailed(f"no Audio/Sink named {node_name!r}")


def run_one(args, reference: str, frames: int, repetition: int, serial: int | None) -> dict:
    name = f"{reference}-{frames}-{repetition}"
    scratch = args.output / "scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    command = [
        str(args.executable),
        "--reference",
        reference,
        "--frames",
        str(frames),
        "--seconds",
        str(args.seconds),
        "--scratch",
        str(scratch),
    ]
    trace_path = args.output / f"trace-{name}.json"
    if args.trace or serial is not None:
        command += ["--trace", str(trace_path)]
    if serial is not None:
        command += ["--pipewire-target", str(serial)]
    if args.workers is not None:
        command += ["--workers", str(args.workers)]
    if reference == "heavy":
        for flag, value in (
            ("--vst3-effect", args.vst3_effect),
            ("--vst3-instrument", args.vst3_instrument),
        ):
            if value:
                command += [flag, str(value)]
    if reference == "drums" and args.stick_kit:
        command += ["--stick-kit", str(args.stick_kit)]
    completed = subprocess.run(
        command,
        env=dict(os.environ, TMPDIR=str(scratch)),
        capture_output=True,
        text=True,
        timeout=args.seconds * RUN_TIMEOUT_FACTOR + RUN_TIMEOUT_MARGIN_SECONDS,
        check=False,
    )
    (args.output / f"log-{name}.txt").write_text(completed.stdout + completed.stderr)
    if completed.returncode:
        raise RunFailed(f"{name}: {completed.stderr.strip() or completed.stdout.strip()}")
    trace = json.loads(trace_path.read_text()) if trace_path.is_file() else None
    summary = summarise(completed.stdout, trace)
    if trace_path.is_file() and not args.keep_traces:
        trace_path.unlink()
    return {"reference": reference, "frames": frames, "repetition": repetition, **summary}


def markdown(evidence: dict) -> str:
    lines = [
        f"Host: {evidence['host']}, {evidence['cores']} logical CPUs. "
        f"Mode: {evidence['mode']}. {evidence['seconds']} s measured per run.",
        "",
        "| Reference | Frames | Run | Helpers | Callback p50 µs | p99 µs | max µs "
        "| p99 of period | Over period | Process CPU % | CPU p99 % (100 ms) | Peak RSS MiB |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for run in evidence["runs"]:
        result = run["result"]
        lines.append(
            f"| {run['reference']} | {run['frames']} | {run['repetition']} | {result['workers']} "
            f"| {result.get('p50_us', '')} | {result.get('p99_us', '')} | {result['max_us']} "
            f"| {result.get('p99_period_percent', '')}% | {result['over_period']} "
            f"| {result['process_cpu_percent']} | {result['cpu_window_p99']} "
            f"| {round(result['max_rss_kib'] / KIB_PER_MIB, 1)} |"
        )
    for run in evidence["runs"]:
        devices = run.get("trace", {}).get("devices")
        if not devices:
            continue
        lines += [
            "",
            f"Devices, {run['reference']} at {run['frames']} frames (run {run['repetition']}):",
            "",
            "| Device | Instances | % of one core | Mean µs | p99 µs | Max µs |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
        lines += [
            f"| {row['device']} | {row['instances']} | {row['core_percent']} | {row['mean_us']} "
            f"| {row['p99_us']} | {row['max_us']} |"
            for row in devices
        ]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--references", nargs="+", choices=REFERENCES, default=list(REFERENCES))
    parser.add_argument(
        "--stick-kit",
        type=Path,
        help="an installed Stick kit folder the drums reference plays (default: synthetic)",
    )
    parser.add_argument(
        "--frames", nargs="+", type=int, choices=BUFFER_SIZES, default=list(BUFFER_SIZES)
    )
    parser.add_argument("--seconds", type=int, default=DEFAULT_SECONDS)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--trace", action="store_true", help="record a per-device table")
    parser.add_argument("--keep-traces", action="store_true")
    parser.add_argument("--target-node", help="play on this PipeWire Audio/Sink node name")
    parser.add_argument("--vst3-effect", type=Path, help="heavy: a VST3 effect bundle")
    parser.add_argument("--vst3-instrument", type=Path, help="heavy: a VST3 instrument bundle")
    parser.add_argument(
        "--workers", type=int, help="audio helpers (default: the live playback default)"
    )
    args = parser.parse_args()
    args.executable = args.executable.resolve()
    if not args.executable.is_file():
        parser.error("build oscillate_reference_project first")
    if args.seconds < 1 or args.repetitions < 1:
        parser.error("seconds and repetitions must be positive")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    serial = sink_serial(args.target_node) if args.target_node else None
    runs = []
    for reference in args.references:
        for frames in args.frames:
            for repetition in range(1, args.repetitions + 1):
                run = run_one(args, reference, frames, repetition, serial)
                print(
                    f"{reference} {frames} #{repetition}: p99 {run['result'].get('p99_us')} µs, "
                    f"process CPU {run['result']['process_cpu_percent']}%",
                    flush=True,
                )
                runs.append(run)
    evidence = {
        "host": platform.platform(),
        "processor": platform.processor(),
        "cores": os.cpu_count(),
        "mode": f"live on {args.target_node}" if serial is not None else "paced fake callback",
        "seconds": args.seconds,
        "sample_rate": SAMPLE_RATE,
        "executable_sha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
        "runs": runs,
    }
    (args.output / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    (args.output / "results.md").write_text(markdown(evidence))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RunFailed, subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
        print(f"reference projects: {error}", file=sys.stderr)
        sys.exit(1)
