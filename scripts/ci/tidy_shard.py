"""Runs scripts/check-tidy.sh on one shard of the configured translation units.

Usage: tidy_shard.py PRESET SHARD COUNT FINDINGS
The C++ sources under apps/, src/ and tests/ in the preset's compilation
database are sorted and dealt round-robin into COUNT shards; shard SHARD
(0-based) is checked. The nightly Tidy sweep runs every shard in parallel,
because one clang-tidy process per translation unit is too slow for one job.

The clang-tidy output passes through unchanged. A header's finding repeats for
every source that includes it, so each distinct finding is also written once to
the FINDINGS file, with its path relative to the tree where it lies inside it.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE_DIRS = ("apps", "src", "tests")
SOURCE_SUFFIX = ".cpp"
USAGE = "Usage: tidy_shard.py PRESET SHARD COUNT FINDINGS"
FINDING = re.compile(r"^(?P<path>[^:\s][^:]*)(?P<rest>:\d+:\d+: (?:error|warning): .*)$")


def parse_arguments(arguments):
    """(preset, shard, count, findings) from the command line, or ValueError naming the fault."""
    if len(arguments) != 4:
        raise ValueError(f"expected 4 arguments, got {len(arguments)}")
    preset, shard, count, findings = arguments
    try:
        shard, count = int(shard), int(count)
    except ValueError:
        raise ValueError(f"SHARD and COUNT must be integers, got {shard!r} and {count!r}") from None
    if count < 1 or not 0 <= shard < count:
        raise ValueError(f"SHARD {shard} is outside 0..{count - 1}")
    return preset, shard, count, findings


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


def database_sources(entries, root):
    """Each entry's source relative to `root`; a relative `file` resolves against `directory`."""
    return {
        os.path.relpath(os.path.realpath(os.path.join(entry["directory"], entry["file"])), root)
        for entry in entries
    }


def configured_sources(preset):
    database = ROOT / "build" / preset / "compile_commands.json"
    with open(database, encoding="utf-8") as stream:
        entries = json.load(stream)
    return database_sources(entries, os.path.realpath(ROOT))


def normalised_finding(line, root):
    """A clang-tidy error or warning line with a normalised path, or None for any other line.

    Inside `root` the path is made relative to it; `..` segments fold lexically, so a
    path outside the tree keeps its leading `..` segments.
    """
    match = FINDING.match(line.rstrip("\n"))
    if match is None:
        return None
    path = os.path.normpath(match["path"])
    if os.path.isabs(path):
        relative = os.path.relpath(path, root)
        if relative != os.pardir and not relative.startswith(os.pardir + os.sep):
            path = relative
    return path + match["rest"]


def distinct_findings(lines, root):
    """The sorted distinct clang-tidy findings in `lines`."""
    return sorted({finding for line in lines if (finding := normalised_finding(line, root))})


def run_shard(preset, sources):
    """Runs check-tidy.sh on `sources`, echoing its output; returns (exit status, output lines)."""
    script = str(ROOT / "scripts" / "check-tidy.sh")
    lines = []
    with subprocess.Popen(
        [script, preset, *sources],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    ) as process:
        for line in process.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            lines.append(line)
    return process.returncode, lines


def main():
    try:
        preset, shard, count, findings = parse_arguments(sys.argv[1:])
    except ValueError as error:
        sys.exit(f"{USAGE}\n{error}")
    sources = shard_sources(configured_sources(preset), shard, count)
    status, lines = 0, []
    if sources:
        print(
            f"Checking {len(sources)} translation unit(s) in shard {shard} of {count}.", flush=True
        )
        status, lines = run_shard(preset, sources)
    else:
        print(f"Shard {shard} of {count} has no translation unit.")
    findings = Path(findings)
    findings.parent.mkdir(parents=True, exist_ok=True)
    findings.write_text(
        "".join(f"{finding}\n" for finding in distinct_findings(lines, os.path.realpath(ROOT))),
        encoding="utf-8",
    )
    sys.exit(status)


if __name__ == "__main__":
    main()
