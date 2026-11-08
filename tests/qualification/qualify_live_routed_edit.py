#!/usr/bin/env python3
"""Opt-in routed send-gain handoff against a disposable PipeWire sink."""

from __future__ import annotations

import importlib.util
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HELPER_PATH = Path(__file__).with_name("qualify_live_common.py")
spec = importlib.util.spec_from_file_location("pipewire_qualification_helpers", HELPER_PATH)
assert spec is not None and spec.loader is not None
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def make_wav(path: Path) -> None:
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(48_000)
        output.writeframes(struct.pack("<h", 8192) * (48_000 * 6))


def levels(path: Path) -> tuple[int, int, int]:
    frames = list(struct.iter_unpack("<ff", helper.wave_data(path)))
    if len(frames) < 48_000 * 3:
        raise RuntimeError("routed monitor capture is too short")
    classified: list[tuple[int, str]] = []
    for start in range(0, len(frames) - 960, 960):
        window = frames[start : start + 960]
        if any(not math.isfinite(left) or not math.isfinite(right) for left, right in window):
            raise RuntimeError("routed monitor capture has nonfinite output")
        mean = sum((left + right) * 0.5 for left, right in window) / len(window)
        if abs(mean - 0.375) < 0.015:
            classified.append((start, "before"))
        elif abs(mean - 0.625) < 0.015:
            classified.append((start, "after"))
    before = [position for position, kind in classified if kind == "before"]
    after = [position for position, kind in classified if kind == "after"]
    if len(before) < 20 or len(after) < 20 or min(after) <= min(before):
        raise RuntimeError(
            f"routed send gain was not audible in order: before={len(before)} after={len(after)}"
        )
    if max(after) - min(after) < 48_000 * 2:
        raise RuntimeError("new send gain did not remain audible after failed graph retry")
    return len(frames), len(before), len(after)


def main() -> int:
    build = REPO / "build/routed-live-qualification"
    fixture = build / "oscillate_routed_live_fixture"
    player = build / "oscillate_routed_player"
    if not fixture.exists() or not player.exists():
        raise RuntimeError(
            "build the routed live fixture and player in build/routed-live-qualification"
        )
    for command in ("pw-cli", "pw-dump", "pw-record"):
        helper.required_tool(command)
    with tempfile.TemporaryDirectory(prefix="oscillate-routed-live-") as directory:
        root = Path(directory)
        source = root / "steady.wav"
        project = root / "project"
        recording = root / "monitor.wav"
        make_wav(source)
        created = helper.checked(
            [str(fixture), "create", str(project), str(source)], capture_output=True
        ).stdout.strip()
        send_match = re.search(r"send=([0-9a-f-]{36})", created)
        revision_match = re.search(r"revision=(\d+)", created)
        if send_match is None or revision_match is None:
            raise RuntimeError(f"fixture did not report a send and revision: {created}")
        name = f"oscillate_routed_edit_{os.getpid()}"
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
                f'node.name={name} node.description="Routed Edit Qualification" '
                "media.class=Audio/Sink audio.position=[ FL FR ] "
                "node.always-process=true priority.session=-10000 }\n"
            )
            sink.stdin.flush()
            sink_id, serial = helper.poll_until("routed sink", 3, lambda: helper.find_sink(name))
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
                "routed monitor links",
                3,
                lambda: helper.active_links(sink_id, "monitor") >= 2,
            )
            playing = subprocess.Popen(
                [str(player), "--pipewire", str(project), "5", str(serial)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            helper.poll_until(
                "routed player links",
                5,
                lambda: helper.active_links(sink_id, "playback") >= 2 or playing.poll() is not None,
            )
            if playing.poll() is not None:
                raise RuntimeError(f"routed player exited early: {playing.stdout.read()}")
            assert playing.stdin is not None
            time.sleep(0.7)
            playing.stdin.write(f"gain {send_match.group(1)} 1.5\n")
            playing.stdin.flush()
            time.sleep(0.9)
            playing.stdin.write("fail-add-track\n")
            playing.stdin.flush()
            time.sleep(0.4)
            playing.stdin.write("retry\n")
            playing.stdin.flush()
            time.sleep(0.4)
            playing.stdin.write("undo\n")
            playing.stdin.flush()
            result, _ = playing.communicate(timeout=9)
            print(result.strip())
            if playing.returncode != 0 or "render_failures=0" not in result:
                raise RuntimeError("routed player failed or reported a render failure")
            if result.count("activated-by sample=") != 2:
                raise RuntimeError("the gain edit and the undo did not both switch live")
            expected_failure = (
                f"model={int(revision_match.group(1)) + 2} "
                f"active={int(revision_match.group(1)) + 1} application=failed"
            )
            if expected_failure not in result or result.count("Graph application failed") < 2:
                raise RuntimeError("failed graph was not reported alongside the active revision")
            recovered_revision = int(revision_match.group(1)) + 3
            if (
                f"model={recovered_revision} active={recovered_revision} application=active"
                not in result
            ):
                raise RuntimeError("undo did not reactivate the recovered saved graph")
            helper.stop_process(recorder, interrupt=True)
            recorder = None
            capture_frames, before, after = levels(recording)
            inspected = helper.checked(
                [str(fixture), "inspect", str(project)], capture_output=True
            ).stdout.strip()
            if (
                f"revision={int(revision_match.group(1)) + 3}" not in inspected
                or "master_effects=0" not in inspected
                or "gain=1.5" not in inspected
            ):
                raise RuntimeError(f"accepted routed edits did not reopen: {inspected}")
            print(
                f"capture_frames={capture_frames} before_windows={before} "
                f"after_windows={after} reopened={inspected}"
            )
        finally:
            helper.stop_process(playing)
            helper.stop_process(recorder, interrupt=True)
            helper.stop_process(sink)
            helper.poll_until("routed sink cleanup", 3, lambda: helper.find_sink(name) is None)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Routed live qualification: {error}", file=sys.stderr)
        sys.exit(1)
