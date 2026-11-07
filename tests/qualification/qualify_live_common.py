"""Shared helpers for the opt-in qualifications against a live PipeWire server."""

from __future__ import annotations

import hashlib
import json
import shutil
import signal
import struct
import subprocess
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
STARTER = REPO / "assets/starter/pluck.wav"
MANIFEST = REPO / "assets/starter/manifest.json"
RATE = 48_000
CHANNELS = 2


def required_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise RuntimeError(f"required live qualification tool is missing: {name}")


def checked(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, text=True, **kwargs)


def objects() -> list[dict[str, Any]]:
    return json.loads(subprocess.check_output(["pw-dump"], timeout=3))


def poll_until(label: str, timeout: float, predicate: Callable[[], Any]) -> Any:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)  # Readiness polling for a live external server.
    raise RuntimeError(f"timed out waiting for {label}")


def stop_process(process: subprocess.Popen[Any] | None, interrupt: bool = False) -> None:
    if process is None or process.poll() is not None:
        return
    if interrupt:
        process.send_signal(signal.SIGINT)
    else:
        process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def starter_integrity() -> None:
    manifest = json.loads(MANIFEST.read_text())
    expected = next(item for item in manifest["sounds"] if item["file"] == STARTER.name)
    if hashlib.sha256(STARTER.read_bytes()).hexdigest() != expected["sha256"]:
        raise RuntimeError("bundled starter WAV differs from its provenance manifest")


def find_sink(name: str) -> tuple[int, int] | None:
    matches = [
        item
        for item in objects()
        if item.get("type") == "PipeWire:Interface:Node"
        and item.get("info", {}).get("props", {}).get("node.name") == name
        and item.get("info", {}).get("props", {}).get("media.class") == "Audio/Sink"
    ]
    if len(matches) != 1:
        return None
    return matches[0]["id"], matches[0]["info"]["props"]["object.serial"]


def active_links(sink_id: int, direction: str) -> int:
    key = "output-node-id" if direction == "monitor" else "input-node-id"
    return sum(
        item.get("type") == "PipeWire:Interface:Link"
        and item.get("info", {}).get(key) == sink_id
        and item.get("info", {}).get("state") == "active"
        for item in objects()
    )


def wave_data(path: Path) -> memoryview:
    content = path.read_bytes()
    if len(content) < 44 or content[:4] != b"RIFF" or content[8:12] != b"WAVE":
        raise RuntimeError("monitor recording is not a RIFF/WAVE file")
    offset = 12
    format_ok = False
    audio = None
    while offset + 8 <= len(content):
        kind, length = struct.unpack_from("<4sI", content, offset)
        offset += 8
        if offset + length > len(content):
            raise RuntimeError("monitor WAV has a truncated chunk")
        if kind == b"fmt ":
            encoding, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", content, offset)
            format_ok = (encoding, channels, rate, bits) == (3, CHANNELS, RATE, 32)
        elif kind == b"data":
            audio = memoryview(content)[offset : offset + length]
        offset += length + (length & 1)
    if not format_ok or audio is None or len(audio) % 8:
        raise RuntimeError("monitor recording is not stereo 48 kHz float32")
    return audio


def create_sink(name: str) -> tuple[subprocess.Popen[str], int, int]:
    process = subprocess.Popen(
        ["pw-cli"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        if process.stdin is None:
            raise RuntimeError("cannot open disposable sink control pipe")
        process.stdin.write(
            "create-node adapter { factory.name=support.null-audio-sink "
            f'node.name={name} node.description="Oscillate Recovery Qualification" '
            "media.class=Audio/Sink audio.position=[ FL FR ] "
            "node.always-process=true priority.session=-10000 }\n"
        )
        process.stdin.flush()
        sink_id, serial = poll_until("disposable sink", 3, lambda: find_sink(name))
        return process, sink_id, serial
    except BaseException:
        stop_process(process)
        raise


def start_recorder(name: str, sink_id: int, path: Path) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(
        [
            "pw-record",
            "-P",
            "{ stream.capture.sink = true }",
            "--target",
            name,
            "--rate",
            str(RATE),
            "--channels",
            str(CHANNELS),
            "--format",
            "f32",
            str(path),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        poll_until(
            "two sink monitor links", 3, lambda: active_links(sink_id, "monitor") >= CHANNELS
        )
        return process
    except BaseException:
        stop_process(process, interrupt=True)
        raise
