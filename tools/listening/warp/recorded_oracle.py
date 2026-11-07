#!/usr/bin/env python3
"""Independent, opt-in structural check of a recorded warp audition set.

FFmpeg decodes both the source and rendered WAVs. This checks the audition
manifest without relying on our media reader or warp implementation. It does
not rate audible quality. Requires FFmpeg/FFprobe, Python's standard library,
and files kept outside the repository's tracked content.
"""

import argparse
import array
import csv
import hashlib
import json
import math
import subprocess
import sys
from pathlib import Path


def stream_info(path: Path) -> tuple[int, int]:
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "a:0",
            "-show_entries",
            "stream=sample_rate,channels",
            "-of",
            "json",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    streams = json.loads(result.stdout).get("streams", [])
    if len(streams) != 1:
        raise ValueError(f"{path}: expected one audio stream")
    return int(streams[0]["sample_rate"]), int(streams[0]["channels"])


def decoded_chunks(path: Path):
    process = subprocess.Popen(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-i",
            str(path),
            "-f",
            "f32le",
            "-acodec",
            "pcm_f32le",
            "-",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert process.stdout is not None
        while data := process.stdout.read(65536):
            if len(data) % 8:
                raise ValueError(f"{path}: unaligned stereo float output")
            values = array.array("f")
            values.frombytes(data)
            yield values
        assert process.stderr is not None
        error = process.stderr.read().decode(errors="replace")
        if process.wait() != 0:
            raise ValueError(f"FFmpeg failed for {path}: {error.strip()}")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if process.stdout is not None:
            process.stdout.close()
        if process.stderr is not None:
            process.stderr.close()


def measure(path: Path) -> tuple[int, float, int, float]:
    if stream_info(path) != (48000, 2):
        raise ValueError(f"{path}: expected 48 kHz stereo")
    frames = 0
    peak = 0.0
    clipped = 0
    energy = 0.0
    for values in decoded_chunks(path):
        frames += len(values) // 2
        for sample in values:
            if not math.isfinite(sample):
                raise ValueError(f"{path}: non-finite sample")
            absolute = abs(sample)
            peak = max(peak, absolute)
            clipped += absolute >= 1.0
            energy += float(sample) * sample
    if not frames:
        raise ValueError(f"{path}: empty audio")
    return frames, peak, clipped, math.sqrt(energy / (2 * frames))


def identical_decoded_audio(first: Path, second: Path) -> bool:
    first_chunks = decoded_chunks(first)
    second_chunks = decoded_chunks(second)
    sentinel = object()
    try:
        while True:
            left = next(first_chunks, sentinel)
            right = next(second_chunks, sentinel)
            if left is sentinel or right is sentinel:
                return left is sentinel and right is sentinel
            if left != right:
                return False
    finally:
        first_chunks.close()
        second_chunks.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("render_dir", type=Path)
    args = parser.parse_args()
    manifest = (args.render_dir / "manifest.tsv").read_text()
    with args.source.open("rb") as source_file:
        digest = hashlib.file_digest(source_file, "sha256").hexdigest()
    metadata = dict(
        line[2:].split("\t", 1)
        for line in manifest.splitlines()
        if line.startswith("# ") and "\t" in line
    )
    if metadata.get("input_sha256") != digest:
        raise ValueError("source digest disagrees with audition manifest")
    source_frames, source_peak, _, source_rms = measure(args.source)
    if source_frames != int(metadata["input_frames"]):
        raise ValueError("source frame count disagrees with audition manifest")
    rows = list(
        csv.DictReader(
            (line for line in manifest.splitlines() if line and not line.startswith("#")),
            delimiter="\t",
        )
    )
    if not rows:
        raise ValueError("no render rows")
    failures = []
    for row in rows:
        if row["status"] != "ok":
            failures.append(f"{row['file']}: {row['status']}")
            continue
        if Path(row["file"]).name != row["file"]:
            failures.append(f"{row['file']}: render path must be a filename")
            continue
        file = args.render_dir / row["file"]
        frames, peak, clipped, rms = measure(file)
        if frames != int(row["expected_frames"]) or frames != int(row["output_frames"]):
            failures.append(f"{file.name}: frame count")
        if abs(peak - float(row["peak"])) > 1e-5:
            failures.append(f"{file.name}: peak")
        if clipped != int(row["clipped_samples"]):
            failures.append(f"{file.name}: clipped sample count")
        row["oracle_peak"] = peak
        row["oracle_clipped"] = clipped
        row["rms_db_vs_source"] = 20 * math.log10(rms / source_rms)
    unity = args.render_dir / "repitch_identity.wav"
    identity_matches = identical_decoded_audio(args.source, unity)
    if not identity_matches:
        failures.append("Re-Pitch identity differs from source")
    print(f"source_sha256={digest} source_frames={source_frames} source_peak={source_peak:.6f}")
    print(
        f"renders={len(rows)} structural_failures={len(failures)} "
        f"repitch_identity={'bit_exact' if identity_matches else 'different'}"
    )
    print("mode\tmaximum_peak\tcases_above_unity\tclipped_samples\trms_db_range")
    for mode in dict.fromkeys(row["mode"] for row in rows):
        group = [row for row in rows if row["mode"] == mode and "oracle_peak" in row]
        if not group:
            continue
        levels = [row["rms_db_vs_source"] for row in group]
        print(
            f"{mode}\t{max(row['oracle_peak'] for row in group):.3f}\t"
            f"{sum(row['oracle_peak'] >= 1 for row in group)}\t"
            f"{sum(row['oracle_clipped'] for row in group)}\t"
            f"{min(levels):+.2f}..{max(levels):+.2f}"
        )
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"oracle error: {error}", file=sys.stderr)
        sys.exit(1)
