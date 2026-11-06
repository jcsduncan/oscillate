#!/usr/bin/env python3
"""Opt-in Qt Quick playback through a disposable PipeWire sink."""

from __future__ import annotations

import argparse
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

from qualify_live_common import (
    CHANNELS,
    REPO,
    active_links,
    checked,
    create_sink,
    find_sink,
    poll_until,
    required_tool,
    start_recorder,
    stop_process,
    wave_data,
)


def make_wav(path: Path, source_rate: int) -> None:
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(source_rate)
        output.writeframes(struct.pack("<h", 8192) * (source_rate * 6))


def check_audio(recording: Path, source_rate: int, expected_level: float) -> tuple[int, int, float]:
    audio = wave_data(recording)
    frames = list(struct.iter_unpack("<ff", audio))
    if len(frames) < 48_000 * 2:
        raise RuntimeError(f"Quick monitor capture too short: {len(frames)} frames")
    active_windows = 0
    peak = 0.0
    for start in range(0, len(frames) - 960, 960):
        window = frames[start : start + 960]
        if any(not math.isfinite(left) or not math.isfinite(right) for left, right in window):
            raise RuntimeError("Quick monitor contains nonfinite audio")
        mean = sum((left + right) * 0.5 for left, right in window) / len(window)
        peak = max(peak, max(max(abs(left), abs(right)) for left, right in window))
        if abs(mean - expected_level) < 0.015:
            active_windows += 1
    # Sinc conversion can overshoot at the source's hard start/stop edges; the
    # long steady windows still need to match the routed level precisely.
    peak_tolerance = 0.05 if source_rate != 48_000 else 0.025
    if active_windows < 50 or abs(peak - expected_level) > peak_tolerance:
        raise RuntimeError(
            f"Quick monitor lacks routed managed audio: peak={peak:.4f}, "
            f"matching 20ms windows={active_windows}"
        )
    return len(frames), active_windows, peak


def qualify(
    quick: Path, fixture: Path, root: Path, source_rate: int, requested_frames: int, limiter: bool
) -> None:
    source = root / "steady.wav"
    project = root / "project"
    recording = root / "monitor.wav"
    make_wav(source, source_rate)
    created = checked(
        [str(fixture), "create", str(project), str(source)], capture_output=True
    ).stdout.strip()
    match = re.search(r"revision=(\d+)", created)
    if match is None:
        raise RuntimeError(f"routed fixture omitted saved revision: {created}")
    source.unlink()  # The Quick path must read its own managed media.
    sink = recorder = player = None
    name = f"oscillate_quick_{os.getpid()}"
    try:
        sink, sink_id, serial = create_sink(name)
        recorder = start_recorder(name, sink_id, recording)
        environment = os.environ.copy()
        environment.update(
            {
                "QT_QPA_PLATFORM": "offscreen",
                "QSG_RHI_BACKEND": "software",
                "XDG_CONFIG_HOME": str(root / "config"),
            }
        )
        command = [
            str(quick),
            "--open",
            str(project),
            "--qualify-pipewire",
            str(serial),
            "--qualify-buffer-frames",
            str(requested_frames),
        ]
        if limiter:
            command.append("--qualify-master-limiter")
        player = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=environment,
        )
        poll_until(
            "Quick playback links",
            8,
            lambda: active_links(sink_id, "playback") >= CHANNELS or player.poll() is not None,
        )
        if active_links(sink_id, "playback") < CHANNELS:
            output, _ = player.communicate(timeout=2)
            raise RuntimeError(f"Quick did not connect to selected sink: {output.strip()}")
        output, _ = player.communicate(timeout=15)
        saved_revision = re.search(r"Quick PipeWire qualified revision: (\d+)", output)
        if limiter and saved_revision is None:
            raise RuntimeError(f"Quick limiter settings were not saved: {output.strip()}")
        expected_revision = int(saved_revision.group(1)) if saved_revision else int(match.group(1))
        stopped = (
            "Playback stopped; limiter lookahead rendered to output; model revision "
            if limiter
            else "Playback stopped; model revision "
        )
        if (
            player.returncode != 0
            or not re.search(
                rf"Quick PipeWire active: Playing routed revision {expected_revision}, sample \d+",
                output,
            )
            or f"Quick PipeWire stopped: {stopped}{expected_revision} retained" not in output
        ):
            raise RuntimeError(f"Quick play/stop status failed: {output.strip()}")
        if "Routed output errors" in output or "Routed source failed" in output:
            raise RuntimeError(f"Quick reported render/source errors: {output.strip()}")
        buffer = re.search(r"Quick PipeWire buffer: (.+)", output)
        if buffer is None or not re.search(
            rf"\bRequest(?:ed)? {requested_frames} frames\b", buffer.group(1)
        ):
            raise RuntimeError(
                f"Quick omitted {requested_frames}-frame buffer request: {output.strip()}"
            )
        print(output.strip())
    finally:
        stop_process(player)
        stop_process(recorder, interrupt=True)
        stop_process(sink)
    poll_until("disposable Quick sink removal", 3, lambda: find_sink(name) is None)
    expected_level = 10 ** (-12 / 20) if limiter else 0.375
    frames, windows, peak = check_audio(recording, source_rate, expected_level)
    print(
        f"Quick source rate={source_rate} requested buffer={requested_frames} "
        f"limiter={'on' if limiter else 'off'} monitor frames={frames} "
        f"routed_windows={windows} peak={peak:.4f}; "
        "disposable sink cleaned"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quick", type=Path, default=REPO / "build/quick/apps/daw_quick/oscillate")
    parser.add_argument(
        "--fixture",
        type=Path,
        default=REPO / "build/routed-live-qualification/oscillate_routed_live_fixture",
    )
    parser.add_argument(
        "--source-rate",
        type=int,
        choices=(44_100, 48_000),
        default=48_000,
        help="fixture WAV rate; 44100 needs a source-resampler-enabled build",
    )
    parser.add_argument(
        "--buffer-frames",
        type=int,
        choices=(64, 128),
        help="run one requested size; default qualifies both 64 and 128",
    )
    parser.add_argument(
        "--limiter",
        action="store_true",
        help="save and exercise the Quick master sample-peak limiter",
    )
    args = parser.parse_args()
    for tool in ("pw-cli", "pw-dump", "pw-record"):
        required_tool(tool)
    if not args.quick.is_file() or not args.fixture.is_file():
        raise RuntimeError("build the Qt Quick app and routed live fixture first")
    for requested_frames in (args.buffer_frames,) if args.buffer_frames else (64, 128):
        with tempfile.TemporaryDirectory(prefix="oscillate-quick-live-") as directory:
            qualify(
                args.quick,
                args.fixture,
                Path(directory),
                args.source_rate,
                requested_frames,
                args.limiter,
            )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"Quick live qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
