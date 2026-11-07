#!/usr/bin/env python3
"""Opt-in long encoded-source seek measurement; generates no repository assets."""

import argparse
import pathlib
import subprocess
import tempfile

SIGNAL = "aevalsrc=0.22*sin(2*PI*440*t)+0.11*sin(2*PI*997*t)|0.17*sin(2*PI*659*t):s=48000:d=600"


def run(binary: pathlib.Path, directory: pathlib.Path) -> None:
    for extension, codec_args in (
        ("flac", ["-c:a", "flac", "-compression_level", "3"]),
        ("mp3", ["-c:a", "libmp3lame", "-b:a", "192k"]),
    ):
        fixture = directory / f"long.{extension}"
        subprocess.run(
            [
                "ffmpeg",
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                SIGNAL,
                *codec_args,
                "-y",
                str(fixture),
            ],
            check=True,
        )
        print(f"fixture={fixture} bytes={fixture.stat().st_size}", flush=True)
        subprocess.run([str(binary), str(fixture)], check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--binary",
        type=pathlib.Path,
        default=pathlib.Path("build/compressed-seek-qualification")
        / "oscillate_compressed_seek_rehearsal",
    )
    parser.add_argument("--artifact-parent", type=pathlib.Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    with tempfile.TemporaryDirectory(
        prefix="oscillate-compressed-seek-",
        dir=args.artifact_parent,
    ) as temporary:
        run(binary, pathlib.Path(temporary))


if __name__ == "__main__":
    main()
