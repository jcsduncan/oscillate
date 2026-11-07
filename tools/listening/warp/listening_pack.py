#!/usr/bin/env python3
"""Prepare peak-safe, approximately RMS-matched WAVs for human warp review.

This is an opt-in audition aid, not an audio-quality score. It never changes
the original source or audition renders. Requires FFmpeg and FFprobe.
"""

import argparse
import csv
import hashlib
import math
import subprocess
from pathlib import Path

from recorded_oracle import measure

DEFAULT_CASES = (
    "repitch_identity",
    "beats_half",
    "tones_up7",
    "texture_markers",
    "complex_half",
    "complex_pro_down7",
)
PEAK_CEILING = 0.9


def gain_for(target_rms: float, peak: float, rms: float) -> float:
    """Match RMS unless the sample-peak ceiling requires less gain."""
    if not all(math.isfinite(value) and value >= 0 for value in (target_rms, peak, rms)):
        raise ValueError("non-finite or negative level")
    if peak == 0 or rms == 0:
        return 1.0
    return min(target_rms / rms, PEAK_CEILING / peak)


def render_gain(source: Path, output: Path, gain: float) -> None:
    subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-v",
            "error",
            "-n",
            "-i",
            str(source),
            "-af",
            f"volume={gain:.17g}",
            "-c:a",
            "pcm_f32le",
            str(output),
        ],
        check=True,
    )


def prepare(
    source: Path, render_dir: Path, output_dir: Path, cases: tuple[str, ...]
) -> list[dict[str, str]]:
    manifest = (render_dir / "manifest.tsv").read_text()
    with source.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    metadata = dict(
        line[2:].split("\t", 1)
        for line in manifest.splitlines()
        if line.startswith("# ") and "\t" in line
    )
    if metadata.get("input_sha256") != digest:
        raise ValueError("source digest disagrees with audition manifest")
    rows = list(
        csv.DictReader(
            (line for line in manifest.splitlines() if line and not line.startswith("#")),
            delimiter="\t",
        )
    )
    by_case = {f"{row['mode']}_{row['case']}": row for row in rows}
    if len(by_case) != len(rows):
        raise ValueError("duplicate mode/case in audition manifest")
    if not cases or len(set(cases)) != len(cases):
        raise ValueError("select at least one unique case")
    for case in cases:
        if case not in by_case:
            raise ValueError(f"unknown audition case: {case}")
        row = by_case[case]
        if row["status"] != "ok" or Path(row["file"]).name != row["file"]:
            raise ValueError(f"unavailable or unsafe audition case: {case}")
    if output_dir.exists() and any(output_dir.iterdir()):
        raise ValueError("output directory must be empty")
    source_frames, source_peak, _, source_rms = measure(source)
    if source_frames != int(metadata["input_frames"]):
        raise ValueError("source frame count disagrees with audition manifest")
    # Use a common reference target that never raises the source above ceiling.
    reference_gain = min(1.0, PEAK_CEILING / source_peak) if source_peak else 1.0
    reference_rms = source_rms * reference_gain
    planned = [("source", source, source_frames, source_peak, source_rms, reference_gain)]
    for case in cases:
        row = by_case[case]
        path = render_dir / row["file"]
        frames, peak, _, rms = measure(path)
        if frames != int(row["expected_frames"]) or frames != int(row["output_frames"]):
            raise ValueError(f"{case}: frame count disagrees with manifest")
        planned.append((case, path, frames, peak, rms, gain_for(reference_rms, peak, rms)))
    output_dir.mkdir(parents=True, exist_ok=True)
    result = []
    for name, path, frames, _, _, gain in planned:
        output = output_dir / f"{name}.wav"
        render_gain(path, output, gain)
        actual_frames, actual_peak, clipped, actual_rms = measure(output)
        if actual_frames != frames or clipped or actual_peak > PEAK_CEILING + 1e-5:
            raise ValueError(f"{name}: rendered listening copy failed safety check")
        difference = (
            20 * math.log10(actual_rms / reference_rms) if actual_rms and reference_rms else 0.0
        )
        result.append(
            {
                "case": name,
                "file": output.name,
                "source_sha256": digest,
                "gain": f"{gain:.9f}",
                "peak": f"{actual_peak:.9f}",
                "rms_db_vs_reference": f"{difference:+.3f}",
                "frames": str(actual_frames),
            }
        )
    with (output_dir / "index.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=result[0].keys(), delimiter="\t")
        writer.writeheader()
        writer.writerows(result)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("render_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument(
        "--cases", nargs="+", default=DEFAULT_CASES, help="mode_case names from manifest.tsv"
    )
    args = parser.parse_args()
    for row in prepare(args.source, args.render_dir, args.output_dir, tuple(args.cases)):
        print("\t".join(row.values()))


if __name__ == "__main__":
    main()
