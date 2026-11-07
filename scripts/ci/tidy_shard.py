"""Runs scripts/check-tidy.sh on one shard of the configured translation units.

Usage: tidy_shard.py PRESET SHARD COUNT
The C++ sources under apps/, src/ and tests/ in the preset's compilation
database are sorted and dealt round-robin into COUNT shards; shard SHARD
(0-based) is checked. The nightly Tidy sweep runs every shard in parallel,
because one clang-tidy process per translation unit is too slow for one job.
"""

import json
import os
import subprocess
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE_DIRS = ("apps", "src", "tests")
SOURCE_SUFFIX = ".cpp"


def shard_sources(configured, shard, count):
    """The sorted C++ sources under SOURCE_DIRS that fall in shard `shard` of `count`."""
    if count < 1 or not 0 <= shard < count:
        raise ValueError(f"shard {shard} is outside 0..{count - 1}")
    sources = sorted(
        path
        for path in set(configured)
        if PurePosixPath(path).parts[0] in SOURCE_DIRS
        and PurePosixPath(path).suffix == SOURCE_SUFFIX
    )
    return sources[shard::count]


def configured_sources(preset):
    database = ROOT / "build" / preset / "compile_commands.json"
    with open(database, encoding="utf-8") as stream:
        entries = json.load(stream)
    root = os.path.realpath(ROOT)
    return {os.path.relpath(os.path.realpath(entry["file"]), root) for entry in entries}


def main():
    if len(sys.argv) != 4:
        sys.exit("Usage: tidy_shard.py PRESET SHARD COUNT")
    preset, shard, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    sources = shard_sources(configured_sources(preset), shard, count)
    if not sources:
        print(f"Shard {shard} of {count} has no translation unit.")
        return
    print(f"Checking {len(sources)} translation unit(s) in shard {shard} of {count}.", flush=True)
    script = str(ROOT / "scripts" / "check-tidy.sh")
    sys.exit(subprocess.run([script, preset, *sources], check=False, cwd=ROOT).returncode)


if __name__ == "__main__":
    main()
