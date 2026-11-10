#!/usr/bin/env python3
"""Builds tonal balance targets from a folder of reference tracks (ADR-0120).

Commands:
  genres REFERENCES OUT_DIR   one target per genre tag with at least
                              --min-references tracks, and one from every
                              track (id "all"), as OUT_DIR/<id>.json
  target REFERENCES --name N  one target from every track, saved where Lens
                              finds the user's own targets
                              ($XDG_DATA_HOME/oscillate/tonal-targets), or
                              to --out

Every audio file under REFERENCES (any format ffmpeg reads) is decoded to
48 kHz stereo float into --work, then measured by oscillate_tonal_targets
with the band meter Lens runs, so a target and a mix in Lens read alike.
Needs ffmpeg and ffprobe, and the tool built:
  scripts/build.sh build/release --target oscillate_tonal_targets
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

AUDIO = {".mp3", ".flac", ".wav", ".aif", ".aiff", ".m4a", ".ogg", ".opus"}
DEFAULT_TOOL = "build/release/tools/tonal_targets/oscillate_tonal_targets"
DEFAULT_MIN_REFERENCES = 8
ALL_ID = "all"
ALL_NAME = "All References"
RATE = "48000"
CHANNELS = "2"


def references(folder):
    """Every audio file under `folder`, in a stable order."""
    root = pathlib.Path(folder)
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in AUDIO)


def genre(path):
    """The file's genre tag, or an empty string."""
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "quiet",
            "-show_entries",
            "format_tags=genre",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(path),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.stdout.strip()


def decode(path, work, index):
    """`path` as raw 48 kHz stereo float32 in `work`."""
    out = pathlib.Path(work) / f"{index:04d}.f32"
    subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-y",
            "-i",
            str(path),
            "-f",
            "f32le",
            "-ac",
            CHANNELS,
            "-ar",
            RATE,
            str(out),
        ],
        check=True,
    )
    return out


def slug(name):
    """A file name for a target: lower case, words joined by hyphens."""
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "target"


def build(tool, target_id, name, out, decoded):
    subprocess.run(
        [tool, "build", "--id", target_id, "--name", name, "--out", str(out)]
        + [str(p) for p in decoded],
        check=True,
    )


def user_targets():
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(
        os.path.expanduser("~"), ".local", "share"
    )
    return pathlib.Path(base) / "oscillate" / "tonal-targets"


def run(args, work):
    files = references(args.references)
    if not files:
        sys.exit(f"no audio files under {args.references}")
    decoded = [decode(path, work, i) for i, path in enumerate(files)]
    if args.command == "target":
        out = pathlib.Path(args.out) if args.out else user_targets() / f"{slug(args.name)}.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        build(args.tool, slug(args.name), args.name, out, decoded)
        return
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    groups = {}
    for path, audio in zip(files, decoded, strict=True):
        groups.setdefault(genre(path), []).append(audio)
    built = {ALL_ID: ALL_NAME}
    for name in sorted(groups):
        if not name or len(groups[name]) < args.min_references:
            print(f"skipped {name or '(untagged)'}: {len(groups[name])} references")
            continue
        target_id = slug(name)
        if target_id in built:
            sys.exit(f"genre tags {built[target_id]!r} and {name!r} would share {target_id}.json")
        built[target_id] = name
        build(args.tool, target_id, name, out_dir / f"{target_id}.json", groups[name])
    build(args.tool, ALL_ID, ALL_NAME, out_dir / f"{ALL_ID}.json", decoded)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--tool", default=DEFAULT_TOOL)
    parser.add_argument("--work", help="where decoded audio goes (default: a temporary folder)")
    commands = parser.add_subparsers(dest="command", required=True)
    genres = commands.add_parser("genres")
    genres.add_argument("references")
    genres.add_argument("out_dir")
    genres.add_argument("--min-references", type=int, default=DEFAULT_MIN_REFERENCES)
    target = commands.add_parser("target")
    target.add_argument("references")
    target.add_argument("--name", required=True)
    target.add_argument("--out")
    args = parser.parse_args()
    if args.work:
        pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
        run(args, args.work)
    else:
        with tempfile.TemporaryDirectory() as work:
            run(args, work)


if __name__ == "__main__":
    main()
