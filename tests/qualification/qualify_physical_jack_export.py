#!/usr/bin/env python3
"""Opt-in named-port physical JACK export evidence. Never selects a default output."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import struct
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RESULT = re.compile(r"^jack_export_result (.+)$", re.MULTILINE)
FIELD = re.compile(r"([a-z_]+)=([^\s]+)")


def parse_result(output: str, requested: int) -> dict[str, int | str]:
    match = RESULT.search(output)
    if match is None:
        raise ValueError("missing production JACK export result")
    fields = dict(FIELD.findall(match.group(1)))
    expected = {
        "published": "durable",
        "rate": "48000",
        "requested_frames": str(requested),
        "server_frames": str(requested),
        "xruns": "0",
        "over_period_blocks": "0",
        "render_failures": "0",
        "invalid_buffers": "0",
        "oversized_blocks": "0",
        "source_health": "passed",
        "named_connections": "passed",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise ValueError(f"{key}={fields.get(key)!r}; expected {value}")
    numeric = {
        key: int(value)
        for key, value in fields.items()
        if key not in ("published", "source_health", "named_connections")
    }
    if numeric["callback_blocks"] < 1:
        raise ValueError("missing callback deadline histogram")
    if numeric["expected_frames"] != numeric["captured_frames"] or numeric["expected_frames"] < 1:
        raise ValueError("capture interval is incomplete")
    if numeric["tail_frames"] > 10 * 48_000:
        raise ValueError("prepared tail exceeds ten seconds")
    return {
        **numeric,
        "published": "durable",
        "source_health": "passed",
        "named_connections": "passed",
    }


def wav_summary(
    path: Path, expected_frames: int, require_audible: bool
) -> dict[str, int | float | str]:
    data = path.read_bytes()
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("published output is not RIFF/WAVE")
    if struct.unpack_from("<I", data, 4)[0] + 8 != len(data):
        raise ValueError("WAV RIFF length is incomplete")
    fmt = None
    pcm = None
    offset = 12
    while offset + 8 <= len(data):
        kind = data[offset : offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        body = offset + 8
        if body + size > len(data):
            raise ValueError("WAV chunk exceeds file")
        if kind == b"fmt ":
            fmt = data[body : body + size]
        elif kind == b"data":
            pcm = memoryview(data)[body : body + size]
        offset = body + size + (size & 1)
    if fmt is None or len(fmt) < 16 or pcm is None:
        raise ValueError("WAV lacks format or samples")
    tag, channels, rate, _, alignment, bits = struct.unpack_from("<HHIIHH", fmt)
    extensible_float = (
        tag == 0xFFFE and len(fmt) >= 26 and struct.unpack_from("<H", fmt, 24)[0] == 3
    )
    if not (tag == 3 or extensible_float) or (channels, rate, alignment, bits) != (
        2,
        48_000,
        8,
        32,
    ):
        raise ValueError("output is not 48 kHz stereo float32")
    if len(pcm) != expected_frames * 8:
        raise ValueError("WAV data chunk has the wrong frame count")
    peak = 0.0
    for (sample,) in struct.iter_unpack("<f", pcm):
        if not math.isfinite(sample):
            raise ValueError("WAV contains a non-finite sample")
        peak = max(peak, abs(sample))
    if require_audible and peak < 0.005:
        raise ValueError("generated fixture did not produce audible samples")
    return {
        "frames": expected_frames,
        "bytes": len(data),
        "peak": round(peak, 6),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def sha256_or_none(path: Path) -> str | None:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def run_once(
    executable: Path,
    project: Path,
    target: Path,
    left: str,
    right: str,
    frames: int,
    end_bar: int,
    scratch: Path,
    require_audible: bool,
) -> dict:
    prior = sha256_or_none(target)
    command = [
        str(executable),
        "run",
        str(project),
        str(target),
        left,
        right,
        str(frames),
        str(end_bar),
    ]
    try:
        completed = subprocess.run(
            command,
            cwd=REPO,
            env={**os.environ, "TMPDIR": str(scratch)},
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=45,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        if sha256_or_none(target) != prior:
            raise RuntimeError("timed-out export changed the prior target") from error
        raise RuntimeError("JACK export timed out without replacing target") from error
    if completed.returncode:
        if sha256_or_none(target) != prior:
            raise RuntimeError(
                "failed export changed the prior target: " + completed.stdout[-2_000:]
            )
        raise RuntimeError(
            f"JACK export failed without replacing target: {completed.stdout[-2_000:]}"
        )
    fields = parse_result(completed.stdout, frames)
    wav = wav_summary(target, int(fields["expected_frames"]), require_audible)
    return {"result": fields, "wav": wav, "log": completed.stdout, "prior_target_sha256": prior}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run-physical",
        action="store_true",
        help="explicitly authorize this command to play through the named ports",
    )
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--artifact-dir", required=True, type=Path)
    parser.add_argument("--left-port", required=True)
    parser.add_argument("--right-port", required=True)
    parser.add_argument("--buffer-frames", required=True, type=int, choices=(64, 128))
    parser.add_argument("--end-bar", type=int, default=1)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument(
        "--project", type=Path, help="open a saved project; otherwise create the bounded fixture"
    )
    args = parser.parse_args()
    if not args.run_physical:
        parser.error("--run-physical is required; this command sends audio to named JACK ports")
    if not 1 <= args.end_bar <= 8 or not 1 <= args.repetitions <= 5:
        parser.error("end bar must be 1..8 and repetitions 1..5")
    if args.left_port == args.right_port:
        parser.error("name two distinct JACK input ports")
    artifact = args.artifact_dir.resolve()
    if not artifact.is_relative_to((REPO / "build").resolve()):
        parser.error("artifact directory must be inside this checkout's build tree")
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error("build oscillate_physical_jack_export first")
    artifact.mkdir(parents=True, exist_ok=True)
    scratch = artifact / "scratch"
    scratch.mkdir(exist_ok=True)
    project = args.project.resolve() if args.project else artifact / "fixture-project"
    if args.project is None and not project.exists():
        made = subprocess.run(
            [str(executable), "create-fixture", str(project)],
            cwd=REPO,
            env={**os.environ, "TMPDIR": str(scratch)},
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
        if made.returncode:
            raise RuntimeError("fixture creation failed: " + (made.stdout + made.stderr)[-2_000:])
    evidence = {
        "kind": "opt-in named-port physical JACK real-time WAV development qualification",
        "utc": datetime.now(UTC).isoformat(),
        "project": str(project),
        "created_fixture": args.project is None,
        "left_port": args.left_port,
        "right_port": args.right_port,
        "buffer_frames": args.buffer_frames,
        "end_bar": args.end_bar,
        "runs": [],
    }
    evidence_path = artifact / f"physical-jack-export-{args.buffer_frames}.json"
    for repetition in range(1, args.repetitions + 1):
        target = artifact / f"master-{args.buffer_frames}-{repetition}.wav"
        try:
            run = run_once(
                executable,
                project,
                target,
                args.left_port,
                args.right_port,
                args.buffer_frames,
                args.end_bar,
                scratch,
                args.project is None,
            )
            run["repetition"] = repetition
            evidence["runs"].append(run)
            print(
                f"frames={args.buffer_frames} repetition={repetition} "
                f"xruns=0 over_period=0 wav={target}",
                flush=True,
            )
        except Exception as error:
            evidence["failure"] = {"repetition": repetition, "reason": str(error)}
            raise
        finally:
            pending = evidence_path.with_suffix(".json.pending")
            pending.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
            pending.replace(evidence_path)
    print(f"evidence={evidence_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"qualification_error {error}", file=sys.stderr)
        sys.exit(1)
