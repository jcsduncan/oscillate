#!/usr/bin/env python3
"""Opt-in PipeWire capture of the desktop playback controller."""

from __future__ import annotations

import importlib.util
import math
import os
import struct
import subprocess
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
STARTER = _common.STARTER
active_links = _common.active_links
checked = _common.checked
create_sink = _common.create_sink
poll_until = _common.poll_until
required_tool = _common.required_tool
start_recorder = _common.start_recorder
starter_integrity = _common.starter_integrity
stop_process = _common.stop_process
wave_data = _common.wave_data


def qualify(artifacts: Path) -> None:
    starter_integrity()
    build_dir = REPO / "build/live-desktop-playback"
    checked(
        [
            "cmake",
            "-S",
            str(REPO / "tests/qualification"),
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
        ]
    )
    checked(
        [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            "oscillate_desktop_playback_fixture",
            "-j4",
        ]
    )
    fixture = build_dir / "oscillate_desktop_playback_fixture"
    name = f"oscillate_desktop_playback_{os.getpid()}"
    audio_recording = artifacts / "desktop-audio-clip.wav"
    sink = recorder = player = None
    try:
        sink, sink_id, serial = create_sink(f"{name}_audio")
        recorder = start_recorder(f"{name}_audio", sink_id, audio_recording)
        player = subprocess.Popen(
            [str(fixture), str(serial), str(STARTER), str(artifacts / "audio-project")],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        poll_until(
            "desktop audio clip output links",
            5,
            lambda: active_links(sink_id, "playback") >= CHANNELS or player.poll() is not None,
        )
        if active_links(sink_id, "playback") < CHANNELS:
            output, _ = player.communicate(timeout=2)
            raise RuntimeError(f"desktop audio clip did not connect: {output.strip()}")
        output, _ = player.communicate(timeout=8)
        if player.returncode != 0 or "source underruns 0" not in output:
            raise RuntimeError(f"desktop audio clip failed: {output.strip()}")
        print(output.strip())
    finally:
        stop_process(player)
        stop_process(recorder, interrupt=True)
        stop_process(sink)
    audio = wave_data(audio_recording)
    audio_peak = 0.0
    for left, right in struct.iter_unpack("<ff", audio):
        if not math.isfinite(left) or not math.isfinite(right):
            raise RuntimeError("desktop audio clip monitor has nonfinite samples")
        audio_peak = max(audio_peak, abs(left), abs(right))
    if len(audio) // 8 < 48_000 or audio_peak < 0.02:
        raise RuntimeError(f"desktop audio clip monitor too short or silent: peak={audio_peak}")
    print(f"desktop audio clip monitor frames={len(audio) // 8} peak={audio_peak:.4f}")


def main() -> None:
    for tool in ("cmake", "ninja", "pw-cli", "pw-dump", "pw-record"):
        required_tool(tool)
    with tempfile.TemporaryDirectory(prefix="oscillate-desktop-") as root:
        qualify(Path(root))


if __name__ == "__main__":
    main()
