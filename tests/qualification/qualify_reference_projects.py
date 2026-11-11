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
import bisect
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
FRAME_SPANS = (
    "audio.render",
    "graph.node",
    "graph.source",
    "graph.wavefront",
    "pool.wait_for_helpers",
)
# The spans that render the graph's nodes and sources, on any audio thread.
NODE_SPANS = ("graph.node", "graph.source")
# The fake callback marks its first measured block; earlier ones are warm-up.
MEASURED_MARK = "qualification.measured"
REFERENCES = ("light", "heavy", "pad", "drums")
BUFFER_SIZES = (64, 128, 256, 512, 1024)
DEFAULT_BUFFER_SIZES = (64, 128)
MASTERS = ("default", "heavy")
# The master node's label in the trace (routing's project route plan).
MASTER_NODE = "Master"
MASTER_NODE_KIND = "graph.node"
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
    # Warm-up blocks, before the fake callback's mark, are left out.
    marks = [
        event["ts"]
        for event in trace["traceEvents"]
        if event.get("ph") == "i" and event.get("name") == MEASURED_MARK
    ]
    measured_from = min(marks) if marks else float("-inf")
    master = master_table(by_kind, measured_from)
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
    parts = [
        span["args"]["n"]
        for span in by_kind.get("graph.wavefront", [])
        if span["ts"] >= measured_from
    ]
    measured_renders = [
        span for span in by_kind.get("audio.render", []) if span["ts"] >= measured_from
    ]
    return {
        "devices": devices,
        "master": master,
        # Blocks rendered whole have no wavefront span: they count as one part.
        "mean_parts": round(
            (sum(parts) + len(measured_renders) - len(parts)) / len(measured_renders), 2
        )
        if measured_renders
        else 1,
        "callbacks": callbacks,
        "xrun_events": xruns,
        "window_us": window_us,
    }


def master_table(by_kind: dict[str, list[dict]], measured_from: float) -> dict | None:
    """The master's time in each block, and the part of it nothing else overlaps.

    A block is one audio.render span. The master may run in several parts a
    block (ADR-0127), so its time is the sum of its spans in the block; its
    exposed time is how long it runs after every other node and source of the
    block has finished, the serial tail that adds to the callback.
    """
    renders = sorted(
        (span["ts"], span["ts"] + span["dur"])
        for span in by_kind.get("audio.render", [])
        if span["ts"] >= measured_from
    )
    if not renders:
        return None
    starts = [start for start, _ in renders]
    master_us = [0.0] * len(renders)
    master_end = [None] * len(renders)
    others_end = [None] * len(renders)
    for kind in NODE_SPANS:
        for span in by_kind.get(kind, []):
            block = bisect.bisect_right(starts, span["ts"]) - 1
            if block < 0 or span["ts"] > renders[block][1]:
                continue
            end = span["ts"] + span["dur"]
            if kind == MASTER_NODE_KIND and span["name"] == MASTER_NODE:
                master_us[block] += span["dur"]
                master_end[block] = max(end, master_end[block] or end)
            else:
                others_end[block] = max(end, others_end[block] or end)
    blocks = [index for index, end in enumerate(master_end) if end is not None]
    if not blocks:
        return None
    totals = [master_us[index] for index in blocks]
    exposed = [
        max(0.0, master_end[index] - (others_end[index] or renders[index][0])) for index in blocks
    ]
    return {
        "blocks": len(blocks),
        "mean_us": round(sum(totals) / len(totals), 3),
        "p99_us": round(percentile(totals, TAIL), 3),
        "max_us": round(max(totals), 3),
        "exposed_mean_us": round(sum(exposed) / len(exposed), 3),
        "exposed_p99_us": round(percentile(exposed, TAIL), 3),
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
    master = summary.get("trace", {}).get("master")
    if master:
        result["master_mean_us"] = master["mean_us"]
        result["master_p99_us"] = master["p99_us"]
        result["master_period_percent"] = round(
            PERCENT * master["mean_us"] / result["period_us"], 2
        )
        result["mean_parts"] = summary["trace"]["mean_parts"]
        result["master_exposed_us"] = master["exposed_mean_us"]
        result["master_exposed_p99_us"] = master["exposed_p99_us"]
        result["master_exposed_percent"] = round(
            PERCENT * master["exposed_mean_us"] / result["period_us"], 2
        )
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
        "--master",
        args.master,
    ]
    trace_path = args.output / f"trace-{name}.json"
    if args.trace or serial is not None:
        command += ["--trace", str(trace_path)]
    if serial is not None:
        command += ["--pipewire-target", str(serial)]
    if args.workers is not None:
        command += ["--workers", str(args.workers)]
    if args.subblocks is not None:
        command += ["--subblocks", args.subblocks]
    if args.ahead_ms is not None:
        command += ["--ahead-ms", str(args.ahead_ms)]
    if args.spike_every is not None:
        command += ["--spike-every", str(args.spike_every), "--spike-us", str(args.spike_us)]
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
    run = {"reference": reference, "frames": frames, "repetition": repetition, **summary}
    if reference == "drums":
        # The kit the drums played: an installed one's folder, or the synthetic kit.
        run["stick_kit"] = str(args.stick_kit) if args.stick_kit else "synthetic"
    return run


def label(run: dict) -> str:
    """The run's reference, with the drums' kit."""
    kit = run.get("stick_kit")
    return f"{run['reference']} ({Path(kit).name})" if kit else run["reference"]


def markdown(evidence: dict) -> str:
    lines = [
        f"Host: {evidence['host']}, {evidence['cores']} logical CPUs. "
        f"Mode: {evidence['mode']}. Master: {evidence['master']}. "
        f"{evidence['seconds']} s measured per run.",
        "",
        "| Reference | Frames | Run | Helpers | Callback p50 µs | p99 µs | max µs "
        "| p99 of period | Over period | Master mean µs | Master of period "
        "| Parts | Master exposed µs | Exposed of period "
        "| Process CPU % | CPU p99 % (100 ms) | Peak RSS MiB |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: "
        "| ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for run in evidence["runs"]:
        result = run["result"]
        share = result.get("master_period_percent")
        exposed = result.get("master_exposed_percent")
        lines.append(
            f"| {label(run)} | {run['frames']} | {run['repetition']} | {result['workers']} "
            f"| {result.get('p50_us', '')} | {result.get('p99_us', '')} | {result['max_us']} "
            f"| {result.get('p99_period_percent', '')}% | {result['over_period']} "
            f"| {result.get('master_mean_us', '')} | {'' if share is None else f'{share}%'} "
            f"| {result.get('mean_parts', '')} | {result.get('master_exposed_us', '')} "
            f"| {'' if exposed is None else f'{exposed}%'} "
            f"| {result['process_cpu_percent']} | {result['cpu_window_p99']} "
            f"| {round(result['max_rss_kib'] / KIB_PER_MIB, 1)} |"
        )
    if evidence.get("gate"):
        lines += ["", "Gate:", ""]
        lines += [f"- {line}" for line in evidence["gate"]]
    for run in evidence["runs"]:
        devices = run.get("trace", {}).get("devices")
        if not devices:
            continue
        lines += [
            "",
            f"Devices, {label(run)} at {run['frames']} frames (run {run['repetition']}):",
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


def gate(args, runs: list[dict]) -> list[str]:
    """Each run that breaks a limit given on the command line, as a sentence."""
    failures = []
    for run in runs:
        result = run["result"]
        name = f"{label(run)} at {run['frames']} frames (run {run['repetition']})"
        if args.gate_master_percent is not None:
            share = result.get("master_period_percent")
            if share is None:
                failures.append(f"{name}: the trace has no master spans (run with --trace)")
            elif share > args.gate_master_percent:
                failures.append(
                    f"{name}: the master takes {share}% of the period, "
                    f"over the {args.gate_master_percent}% limit"
                )
        if args.gate_exposed_percent is not None:
            exposed = result.get("master_exposed_percent")
            if exposed is None:
                failures.append(f"{name}: the trace has no master spans (run with --trace)")
            elif exposed > args.gate_exposed_percent:
                failures.append(
                    f"{name}: the master runs alone for {exposed}% of the period, "
                    f"over the {args.gate_exposed_percent}% limit"
                )
        if args.gate_late_blocks is not None and result["over_period"] > args.gate_late_blocks:
            failures.append(
                f"{name}: {result['over_period']} blocks passed the period, "
                f"over the limit of {args.gate_late_blocks}"
            )
        if args.gate_p99_percent is not None:
            load = result.get("p99_period_percent")
            if load is None:
                failures.append(f"{name}: the run reported no callback p99")
            elif load > args.gate_p99_percent:
                failures.append(
                    f"{name}: the callback's p99 is {load}% of the period, "
                    f"over the {args.gate_p99_percent}% limit"
                )
    return failures


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
        "--frames", nargs="+", type=int, choices=BUFFER_SIZES, default=list(DEFAULT_BUFFER_SIZES)
    )
    parser.add_argument(
        "--master",
        choices=MASTERS,
        default="default",
        help="the master chain: the references' own, or a finished master's (ADR-0125)",
    )
    parser.add_argument(
        "--gate-master-percent",
        type=float,
        help="fail when the master's mean serial time exceeds this share of the period",
    )
    parser.add_argument(
        "--gate-exposed-percent",
        type=float,
        help="fail when the master's mean time alone after the other nodes (ADR-0127) "
        "exceeds this share of the period",
    )
    parser.add_argument(
        "--gate-late-blocks",
        type=int,
        help="fail when more blocks than this pass the period",
    )
    parser.add_argument(
        "--gate-p99-percent",
        type=float,
        help="fail when the callback's p99 exceeds this share of the period",
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
    parser.add_argument(
        "--ahead-ms",
        type=int,
        help="render ahead of the callback by up to this many milliseconds (ADR-0128)",
    )
    parser.add_argument(
        "--spike-every",
        type=int,
        help="make every Nth render slower by --spike-us, to test render-ahead under spikes",
    )
    parser.add_argument("--spike-us", type=int, default=0, help="the spike's length")
    parser.add_argument(
        "--subblocks",
        help="how blocks are cut into parts (ADR-0127): adaptive (default), off, or a part length",
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
    failures = gate(args, runs)
    evidence = {
        "host": platform.platform(),
        "processor": platform.processor(),
        "cores": os.cpu_count(),
        "mode": f"live on {args.target_node}" if serial is not None else "paced fake callback",
        "master": args.master,
        "seconds": args.seconds,
        "sample_rate": SAMPLE_RATE,
        "executable_sha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
        "runs": runs,
        "gate": failures,
    }
    (args.output / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    (args.output / "results.md").write_text(markdown(evidence))
    for failure in failures:
        print(f"gate: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RunFailed, subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
        print(f"reference projects: {error}", file=sys.stderr)
        sys.exit(1)
