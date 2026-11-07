#!/usr/bin/env python3
"""Opt-in 32-track managed-WAV rehearsal on a disposable PipeWire null sink."""

from __future__ import annotations

import argparse
import importlib.util
import math
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# Loaded by path, so the runner also works under python3 -I.
_COMMON_PATH = Path(__file__).with_name("qualify_live_common.py")
_common_spec = importlib.util.spec_from_file_location("qualify_live_common", _COMMON_PATH)
assert _common_spec is not None and _common_spec.loader is not None
_common = importlib.util.module_from_spec(_common_spec)
_common_spec.loader.exec_module(_common)
CHANNELS = _common.CHANNELS
REPO = _common.REPO
active_links = _common.active_links
create_sink = _common.create_sink
find_sink = _common.find_sink
poll_until = _common.poll_until
required_tool = _common.required_tool
start_recorder = _common.start_recorder
stop_process = _common.stop_process
wave_data = _common.wave_data


def monitor_evidence(path: Path, minimum_frames: int) -> tuple[int, float, int]:
    samples = wave_data(path)
    frames = len(samples) // (CHANNELS * 4)
    if frames < minimum_frames:
        raise RuntimeError(f"monitor recorded only {frames} frames")
    peak = 0.0
    active_windows = 0
    for start in range(0, len(samples), 960 * CHANNELS * 4):
        chunk = samples[start : start + 960 * CHANNELS * 4]
        if len(chunk) < 960 * CHANNELS * 4:
            break
        energy = 0.0
        for left, right in struct.iter_unpack("<ff", chunk):
            if not math.isfinite(left) or not math.isfinite(right):
                raise RuntimeError("monitor contains nonfinite samples")
            peak = max(peak, abs(left), abs(right))
            energy += left * left + right * right
        if math.sqrt(energy / (2 * 960)) > 0.005:
            active_windows += 1
    required_windows = max(3, min(20, minimum_frames // 960))
    if peak < 0.01 or active_windows < required_windows:
        raise RuntimeError(
            f"monitor is not durably audible: peak={peak:.4f}, active={active_windows}"
        )
    return frames, peak, active_windows


def qualify(
    executable: Path,
    frames: int,
    blocks: int,
    warmup: int,
    root: Path,
    vst3_insert: bool,
    vst3_instrument: bool,
) -> None:
    name = f"oscillate_streaming32_{frames}_{root.name.replace('-', '_')}"
    recording = root / "monitor.wav"
    sink = recorder = player = None
    try:
        sink, sink_id, serial = create_sink(name)
        recorder = start_recorder(name, sink_id, recording)
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
        ]
        if vst3_insert:
            command.extend(("--vst3-insert", "1"))
        if vst3_instrument:
            command.extend(("--vst3-instrument", "1"))
        player = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        # Scanner-backed project preparation can take longer than sink startup.
        # Wait for actual links, not merely elapsed time, before measuring audio.
        poll_until(
            "32-track playback links",
            20,
            lambda: active_links(sink_id, "playback") >= CHANNELS or player.poll() is not None,
        )
        if active_links(sink_id, "playback") < CHANNELS:
            output, _ = player.communicate(timeout=2)
            raise RuntimeError(f"32-track player did not connect to disposable sink: {output}")
        output, _ = player.communicate(timeout=40)
        print(output.strip())
        if player.returncode != 0:
            raise RuntimeError(f"32-track live player exited with {player.returncode}")
        result = re.search(r"^live_result (.+)$", output, re.MULTILINE)
        if result is None:
            raise RuntimeError("32-track live player omitted its result")
        fields = dict(re.findall(r"(\w+)=([^\s]+)", result.group(1)))
        expected = {
            "requested_frames": frames,
            "observed_frames": frames,
            "client_rate": 48_000,
            "graph_rate": 48_000,
            "target_frames": (blocks + warmup) * frames,
            "render_failures": 0,
            "missing_frames": 0,
            "worker_failed": 0,
            "oversized_blocks": 0,
            "invalid_buffers": 0,
            "vst3_insert": int(vst3_insert),
            "vst3_instrument": int(vst3_instrument),
            "vst3_fault": 0,
        }
        if any(int(fields.get(key, -1)) != value for key, value in expected.items()):
            raise RuntimeError(f"unexpected 32-track live counters: {fields}")
        if int(fields.get("completed_frames", 0)) < expected["target_frames"]:
            raise RuntimeError("live transport did not complete requested audio")
        measured = int(fields.get("measured_callback_blocks", 0))
        over_period = int(fields.get("callback_over_period_blocks", -1))
        max_duration_ns = int(fields.get("max_callback_duration_ns", 0))
        if measured <= 0 or not 0 <= over_period <= measured or max_duration_ns <= 0:
            raise RuntimeError(f"invalid callback timing observations: {fields}")
        print(
            f"callback_timing measured_blocks={measured} "
            f"over_nominal_period_blocks={over_period} "
            f"max_duration_ns={max_duration_ns}"
        )
        if fields.get("xruns") != "unavailable":
            xruns = int(fields["xruns"])
            print(f"backend_xruns={xruns}")
            if xruns != 0:
                raise RuntimeError("PipeWire backend reported an xrun")
        else:
            print("backend_xruns=unavailable (no attributable PipeWire stream counter)")
    finally:
        stop_process(player)
        stop_process(recorder, interrupt=True)
        stop_process(sink)
    poll_until("disposable 32-track sink removal", 3, lambda: find_sink(name) is None)
    count, peak, active = monitor_evidence(recording, (blocks * frames) // 2)
    print(
        f"monitor_frames={count} monitor_peak={peak:.4f} "
        f"active_20ms_windows={active} disposable_sink_cleaned=true"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--executable",
        type=Path,
        default=REPO / "build/streaming-32/oscillate_streaming_32track_rehearsal",
    )
    parser.add_argument(
        "--frames", type=int, choices=(64, 128), help="one requested size; default runs both"
    )
    parser.add_argument("--blocks", type=int, default=2_000)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument(
        "--vst3-insert",
        action="store_true",
        help="use one scanned synthetic VST3 effect in the 32-track project",
    )
    parser.add_argument(
        "--vst3-instrument",
        action="store_true",
        help="use one scanned synthetic VST3 instrument with the 32 audio tracks",
    )
    args = parser.parse_args()
    if args.blocks <= 0 or args.warmup <= 0:
        raise RuntimeError("blocks and warmup must be positive")
    if not args.executable.is_file():
        raise RuntimeError("build the opt-in streaming 32-track rehearsal target first")
    for tool in ("pw-cli", "pw-dump", "pw-record"):
        required_tool(tool)
    for frames in (args.frames,) if args.frames else (64, 128):
        with tempfile.TemporaryDirectory(prefix="oscillate-streaming32-live-") as directory:
            qualify(
                args.executable,
                frames,
                args.blocks,
                args.warmup,
                Path(directory),
                args.vst3_insert,
                args.vst3_instrument,
            )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"32-track live qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
