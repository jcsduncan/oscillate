#!/usr/bin/env python3
"""Opt-in 32-track PipeWire run against one explicitly named ALSA output."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import platform
import re
import selectors
import subprocess
import sys
import time
from datetime import UTC, datetime
from pathlib import Path

# Loaded by path, so the runner also works under python3 -I.
_COMMON_PATH = Path(__file__).with_name("qualify_live_common.py")
_common_spec = importlib.util.spec_from_file_location("qualify_live_common", _COMMON_PATH)
assert _common_spec is not None and _common_spec.loader is not None
_common = importlib.util.module_from_spec(_common_spec)
_common_spec.loader.exec_module(_common)
objects = _common.objects
required_tool = _common.required_tool
stop_process = _common.stop_process


REPO = Path(__file__).resolve().parents[2]
RESULT = re.compile(r"^live_result (.+)$", re.MULTILINE)
FIELD = re.compile(r"(\w+)=([^\s]+)")
RATE = 48_000
WARMUP_BLOCKS = 100


def workload_plan(frames: int, measured_seconds: int | None) -> tuple[int, int]:
    """Return measured blocks and bounded per-track source seconds."""
    if frames not in (64, 128):
        raise ValueError("frame size must be 64 or 128")
    if measured_seconds is None:
        return (4_500 if frames == 64 else 2_200), 8
    if measured_seconds < 60 or measured_seconds > 90:
        raise ValueError("long measured seconds must be from 60 to 90")
    blocks = (measured_seconds * RATE + frames - 1) // frames
    source_seconds = ((blocks + WARMUP_BLOCKS) * frames + RATE - 1) // RATE
    return blocks, source_seconds


def physical_sink(name: str) -> dict:
    matches = [
        item
        for item in objects()
        if item.get("type") == "PipeWire:Interface:Node"
        and item.get("info", {}).get("props", {}).get("node.name") == name
        and item.get("info", {}).get("props", {}).get("media.class") == "Audio/Sink"
        and item.get("info", {}).get("props", {}).get("device.api") == "alsa"
    ]
    if len(matches) != 1:
        raise RuntimeError("target must name exactly one physical ALSA Audio/Sink")
    return matches[0]


def player_node(sink_id: int, pid: int) -> dict:
    state = objects()
    client_ids = {
        item["id"]
        for item in state
        if item.get("type") == "PipeWire:Interface:Client"
        and str(item.get("info", {}).get("props", {}).get("application.process.id")) == str(pid)
    }
    output_ids = {
        item.get("info", {}).get("output-node-id")
        for item in state
        if item.get("type") == "PipeWire:Interface:Link"
        and item.get("info", {}).get("input-node-id") == sink_id
        and item.get("info", {}).get("state") == "active"
    }
    matches = [
        item
        for item in state
        if item.get("type") == "PipeWire:Interface:Node"
        and item.get("id") in output_ids
        and item.get("info", {}).get("props", {}).get("client.id") in client_ids
    ]
    if len(matches) != 1:
        raise RuntimeError("expected one linked qualification playback node for this process")
    return matches[0]


def pw_top_errors(output: str, node_ids: set[int]) -> dict[int, int]:
    """Read the last complete batch sample; ERR is node/driver errors, not xruns."""
    samples: list[dict[int, int]] = []
    current: dict[int, int] | None = None
    for line in output.splitlines():
        if line.strip().startswith("S   ID  QUANT"):
            if current is not None:
                samples.append(current)
            current = {}
            continue
        if current is None:
            continue
        fields = line.split(maxsplit=9)
        if len(fields) < 9 or not fields[1].isdigit() or not fields[8].isdigit():
            continue
        node_id = int(fields[1])
        if node_id in current:
            raise RuntimeError(f"pw-top reported node {node_id} twice in one sample")
        current[node_id] = int(fields[8])
    if current is not None:
        samples.append(current)
    complete = [sample for sample in samples if node_ids <= sample.keys()]
    if not complete:
        raise RuntimeError(f"pw-top omitted a complete sample for nodes {sorted(node_ids)}")
    return {node_id: complete[-1][node_id] for node_id in sorted(node_ids)}


def sample_errors(node_ids: set[int]) -> dict[int, int]:
    output = subprocess.check_output(
        ["pw-top", "--batch-mode", "--iterations", "2"], text=True, timeout=5
    )
    return pw_top_errors(output, node_ids)


def read_ready(player: subprocess.Popen[str], timeout: float) -> str:
    if player.stdout is None:
        raise RuntimeError("player output pipe is unavailable")
    received = bytearray()
    with selectors.DefaultSelector() as selector:
        selector.register(player.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not selector.select(max(0.0, deadline - time.monotonic())):
                break
            chunk = os.read(player.stdout.fileno(), 4096)
            if not chunk:
                break
            received.extend(chunk)
            if any(line.startswith(b"live_ready ") for line in received.splitlines()):
                return received.decode(errors="replace")
    raise RuntimeError(
        "player did not report a ready physical-output stream: "
        + received.decode(errors="replace")[-2_000:]
    )


def checked_result(
    output: str, frames: int, blocks: int, warmup: int, source_seconds: int = 8
) -> dict[str, str]:
    found = RESULT.search(output)
    if found is None:
        raise RuntimeError("player omitted live_result")
    fields = dict(FIELD.findall(found.group(1)))
    expected = {
        "requested_frames": frames,
        "observed_frames": frames,
        "source_seconds": source_seconds,
        "client_rate": 48_000,
        "graph_rate": 48_000,
        "target_frames": (blocks + warmup) * frames,
        "render_failures": 0,
        "missing_frames": 0,
        "worker_failed": 0,
        "oversized_blocks": 0,
        "invalid_buffers": 0,
        "vst3_insert": 1,
        "vst3_instrument": 1,
        "vst3_fault": 0,
    }
    for key, value in expected.items():
        if int(fields.get(key, -1)) != value:
            raise RuntimeError(f"unexpected {key}: {fields.get(key)} (expected {value})")
    if int(fields.get("completed_frames", 0)) < expected["target_frames"]:
        raise RuntimeError("live transport did not complete the target interval")
    measured = int(fields.get("measured_callback_blocks", 0))
    if measured <= 0 or int(fields.get("max_callback_duration_ns", 0)) <= 0:
        raise RuntimeError("missing callback timing evidence")
    if int(fields.get("callback_over_period_blocks", -1)) != 0:
        raise RuntimeError("callback exceeded the nominal block period")
    recovery = fields.get("graph_xrun_recovery_events")
    if recovery != "unavailable":
        if recovery is None or not recovery.isdecimal():
            raise RuntimeError("missing or invalid PipeWire node recovery evidence")
        if int(recovery) != 0:
            raise RuntimeError("PipeWire reported a node xrun recovery notice")
    if fields.get("xruns") not in ("unavailable", "0"):
        raise RuntimeError("backend reported a stream xrun")
    return fields


def run_once(
    executable: Path,
    target: dict,
    frames: int,
    blocks: int,
    warmup: int,
    source_seconds: int,
    scratch: Path,
) -> dict:
    target_id = target["id"]
    serial = target["info"]["props"]["object.serial"]
    command = [
        str(executable),
        "--pipewire-target",
        str(serial),
        "--frames",
        str(frames),
        "--blocks",
        str(blocks),
        "--warmup",
        str(warmup),
        "--repetitions",
        "1",
        "--source-seconds",
        str(source_seconds),
        "--vst3-insert",
        "1",
        "--vst3-instrument",
        "1",
    ]
    env = os.environ.copy()
    env["TMPDIR"] = str(scratch)
    player = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    try:
        ready_text = read_ready(player, 120 if source_seconds > 8 else 30)
        ready_at = time.monotonic()
        linked = player_node(target_id, player.pid)
        node_ids = {target_id, linked["id"]}
        before = sample_errors(node_ids)
        # Sample at half of a short run or 80% of a long run. Leave time for
        # pw-top and node checks before the stream disappears.
        run_seconds = (blocks + warmup) * frames / RATE
        sample_fraction = 0.8 if run_seconds > 20 else 0.5
        time.sleep(max(0.0, run_seconds * sample_fraction - (time.monotonic() - ready_at)))
        after = sample_errors(node_ids)
        linked_after = player_node(target_id, player.pid)
        if linked_after["id"] != linked["id"] or linked_after["info"]["props"].get(
            "object.serial"
        ) != linked["info"]["props"].get("object.serial"):
            raise RuntimeError("playback node identity changed between ERR samples")
        output_tail, _ = player.communicate(timeout=max(30, run_seconds * 2))
        output = ready_text + output_tail
        if player.returncode != 0:
            raise RuntimeError(f"physical-output rehearsal exited {player.returncode}:\n{output}")
        current = physical_sink(target["info"]["props"]["node.name"])
        if current["id"] != target_id or current["info"]["props"]["object.serial"] != serial:
            raise RuntimeError("physical sink identity changed during the run")
        fields = checked_result(output, frames, blocks, warmup, source_seconds)
        delta = {node_id: after[node_id] - before[node_id] for node_id in node_ids}
        record = {
            "frames": frames,
            "blocks": blocks,
            "warmup": warmup,
            "source_seconds": source_seconds,
            "target_id": target_id,
            "target_serial": serial,
            "player_id": linked["id"],
            "player_serial": linked["info"]["props"].get("object.serial"),
            "pw_top_err_before": before,
            "pw_top_err_after": after,
            "pw_top_err_delta": delta,
            "result": fields,
            "log": output,
        }
        if any(value < 0 for value in delta.values()):
            record["failure"] = "pw-top ERR counter reset during the run"
        elif any(delta.values()):
            record["failure"] = f"pw-top node/driver ERR rose during the run: {delta}"
        return record
    finally:
        stop_process(player)
        if player.stdout is not None:
            player.stdout.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--target-node", required=True, help="exact node.name of a physical ALSA Audio/Sink"
    )
    parser.add_argument(
        "--executable",
        type=Path,
        required=True,
        help="Release oscillate_streaming_32track_rehearsal binary",
    )
    parser.add_argument(
        "--artifact-dir",
        type=Path,
        required=True,
        help="build-tree directory for JSON evidence and temporary media",
    )
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument(
        "--measured-seconds",
        type=int,
        help="opt-in 60–90 second measured run; short default is unchanged",
    )
    args = parser.parse_args()
    if args.repetitions < 1 or args.repetitions > 3:
        raise RuntimeError("repetitions must be from 1 to 3")
    plans = {frames: workload_plan(frames, args.measured_seconds) for frames in (64, 128)}
    executable = args.executable.resolve(strict=True)
    artifact_dir = args.artifact_dir.resolve()
    if "build" not in artifact_dir.parts:
        raise RuntimeError("artifact directory must be inside a build tree")
    artifact_dir.mkdir(parents=True, exist_ok=True)
    scratch = artifact_dir / "scratch"
    scratch.mkdir(exist_ok=True)
    for tool in ("pw-dump", "pw-top"):
        required_tool(tool)
    target = physical_sink(args.target_node)
    print(
        f"Physical output {args.target_node} will play the 32-track fixture. "
        "Set its listening volume before running this command.",
        flush=True,
    )
    report = {
        "time_utc": datetime.now(UTC).isoformat(),
        "host": platform.node(),
        "kernel": platform.release(),
        "target_name": args.target_node,
        "target_serial": target["info"]["props"]["object.serial"],
        "executable": str(executable),
        "runs": [],
        "requested_measured_seconds": args.measured_seconds,
        "interpretation": "pw-top ERR includes node/driver errors, not just xruns; "
        "graph_xrun_recovery_events counts only recovery notices "
        "for this player node when every callback exposed "
        "SPA_IO_Position; callback timing and node recovery "
        "are not exact stream/device xrun counts.",
    }
    path = artifact_dir / "physical-32track.json"
    try:
        for frames, (blocks, source_seconds) in plans.items():
            for repetition in range(args.repetitions):
                print(f"frames={frames} repetition={repetition + 1}/{args.repetitions}", flush=True)
                recorded = False
                try:
                    record = run_once(
                        executable, target, frames, blocks, WARMUP_BLOCKS, source_seconds, scratch
                    )
                    record["repetition"] = repetition + 1
                    report["runs"].append(record)
                    recorded = True
                    if "failure" in record:
                        raise RuntimeError(record["failure"])
                except Exception as error:
                    if not recorded:
                        report["runs"].append(
                            {
                                "frames": frames,
                                "repetition": repetition + 1,
                                "source_seconds": source_seconds,
                                "failure": str(error),
                            }
                        )
                    raise
                finally:
                    path.write_text(json.dumps(report, indent=2) + "\n")
    finally:
        path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Development device slice passed; evidence: {path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (
        RuntimeError,
        ValueError,
        OSError,
        subprocess.TimeoutExpired,
        subprocess.CalledProcessError,
    ) as error:
        print(f"physical 32-track qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
