#!/usr/bin/env python3
"""Opt-in managed AIFF/FLAC/MP3 routed playback against a disposable PipeWire sink."""

from __future__ import annotations

import argparse
import importlib.util
import math
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HELPER_PATH = Path(__file__).with_name("qualify_live_common.py")
spec = importlib.util.spec_from_file_location("pipewire_qualification_helpers", HELPER_PATH)
assert spec is not None and spec.loader is not None
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--format", choices=("aiff", "flac", "mp3"), default="flac")
    format_name = parser.parse_args().format
    build = REPO / "build/routed-live-qualification"
    fixture = build / "oscillate_routed_live_fixture"
    player = build / "oscillate_routed_player"
    if not fixture.exists() or not player.exists():
        raise RuntimeError("build the routed live fixture and player with codec adapter enabled")
    for command in ("ffmpeg", "pw-cli", "pw-dump", "pw-record"):
        helper.required_tool(command)
    with tempfile.TemporaryDirectory(prefix=f"oscillate-routed-{format_name}-") as directory:
        root = Path(directory)
        source = root / f"source.{format_name}"
        project = root / "project"
        recording = root / "monitor.wav"
        encoder = {"aiff": "pcm_s16be", "flac": "flac", "mp3": "libmp3lame"}[format_name]
        helper.checked(
            [
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:sample_rate=48000:duration=5",
                "-c:a",
                encoder,
                str(source),
            ],
            capture_output=True,
        )
        created = helper.checked(
            [str(fixture), "create-encoded", str(project), str(source)],
            capture_output=True,
        ).stdout.strip()
        name = f"oscillate_routed_{format_name}_{os.getpid()}"
        sink = subprocess.Popen(
            ["pw-cli"],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        recorder = None
        playing = None
        try:
            assert sink.stdin is not None
            sink.stdin.write(
                "create-node adapter { factory.name=support.null-audio-sink "
                f'node.name={name} node.description="Routed {format_name.upper()} Qualification" '
                "media.class=Audio/Sink audio.position=[ FL FR ] "
                "node.always-process=true priority.session=-10000 }\n"
            )
            sink.stdin.flush()
            sink_id, serial = helper.poll_until(
                f"{format_name} sink", 3, lambda: helper.find_sink(name)
            )
            recorder = subprocess.Popen(
                [
                    "pw-record",
                    "-P",
                    "{ stream.capture.sink = true }",
                    "--target",
                    name,
                    "--rate",
                    "48000",
                    "--channels",
                    "2",
                    "--format",
                    "f32",
                    str(recording),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            helper.poll_until(
                f"{format_name} monitor links",
                3,
                lambda: helper.active_links(sink_id, "monitor") >= 2,
            )
            playing = subprocess.Popen(
                [str(player), "--pipewire", str(project), "4", str(serial)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            helper.poll_until(
                f"{format_name} player links",
                5,
                lambda: helper.active_links(sink_id, "playback") >= 2 or playing.poll() is not None,
            )
            result, _ = playing.communicate(timeout=8)
            if (
                playing.returncode != 0
                or "render_failures=0" not in result
                or "stream_missing_frames=0" not in result
                or "stream_worker_failed=0" not in result
            ):
                raise RuntimeError(f"routed {format_name} player failed: {result}")
            helper.stop_process(recorder, interrupt=True)
            recorder = None
            frames = list(struct.iter_unpack("<ff", helper.wave_data(recording)))
            active = 0
            for start in range(0, len(frames) - 960, 960):
                window = frames[start : start + 960]
                if any(
                    not math.isfinite(left) or not math.isfinite(right) for left, right in window
                ):
                    raise RuntimeError(f"{format_name} capture has nonfinite output")
                rms = math.sqrt(
                    sum((left * left + right * right) * 0.5 for left, right in window) / len(window)
                )
                active += rms > 0.01
            if active < 100:
                raise RuntimeError(
                    f"{format_name} capture contains too little audible audio: {active} windows"
                )
            reopened = helper.checked(
                [str(fixture), "inspect", str(project)],
                capture_output=True,
            ).stdout.strip()
            print(
                f"format={format_name} created={created} active_windows={active} "
                f"captured_frames={len(frames)} "
                f"reopened={reopened}"
            )
            print(result.strip())
        finally:
            helper.stop_process(playing)
            helper.stop_process(recorder, interrupt=True)
            helper.stop_process(sink)
            helper.poll_until(
                f"{format_name} sink cleanup", 3, lambda: helper.find_sink(name) is None
            )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Routed encoded qualification: {error}", file=sys.stderr)
        sys.exit(1)
