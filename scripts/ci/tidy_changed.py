"""Runs scripts/check-tidy.sh on the translation units a change touches.

Usage: tidy_changed.py PRESET BASE
A changed C++ source is checked itself. A changed header is checked through
the source of the same name beside it, or under src/ for a public header in
include/oscillate/. Only findings in the changed files are reported, so a
change is not held to findings in the unchanged headers it includes.
Sources missing from the preset's compilation database (a feature the preset
leaves out) are skipped. scripts/check-tidy.sh checks the whole tree.
"""

import json
import os
import subprocess
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE_DIRS = ("apps", "src", "tests")
HEADER_SUFFIXES = (".hpp", ".h")
PUBLIC_HEADER_ROOT = PurePosixPath("include/oscillate")


def changed_files(base):
    result = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", f"{base}...HEAD"],
        check=True,
        capture_output=True,
        text=True,
        cwd=ROOT,
    )
    return [line for line in result.stdout.splitlines() if line]


def candidate_sources(path):
    path = PurePosixPath(path)
    if path.suffix == ".cpp":
        return [path]
    if path.suffix not in HEADER_SUFFIXES:
        return []
    sources = [path.with_suffix(".cpp")]
    if path.is_relative_to(PUBLIC_HEADER_ROOT):
        sources.append(
            PurePosixPath("src") / path.relative_to(PUBLIC_HEADER_ROOT).with_suffix(".cpp")
        )
    return sources


def translation_units(changed, configured):
    """The configured sources, relative to the root, that cover the changed files."""
    selected = []
    for path in changed:
        for source in candidate_sources(path):
            if (
                source.parts[0] in SOURCE_DIRS
                and str(source) in configured
                and str(source) not in selected
            ):
                selected.append(str(source))
    return selected


def line_filter(changed):
    """clang-tidy --line-filter JSON naming every changed C++ file, all lines."""
    names = [path for path in changed if PurePosixPath(path).suffix in (".cpp", *HEADER_SUFFIXES)]
    return json.dumps([{"name": name} for name in names])


def configured_sources(preset):
    database = ROOT / "build" / preset / "compile_commands.json"
    with open(database, encoding="utf-8") as stream:
        entries = json.load(stream)
    root = os.path.realpath(ROOT)
    return {os.path.relpath(os.path.realpath(entry["file"]), root) for entry in entries}


def main():
    if len(sys.argv) != 3:
        sys.exit("Usage: tidy_changed.py PRESET BASE")
    preset, base = sys.argv[1], sys.argv[2]
    changed = changed_files(base)
    sources = translation_units(changed, configured_sources(preset))
    if not sources:
        print(f"No configured C++ translation unit changed against {base}.")
        return
    print(f"Checking {len(sources)} translation unit(s) against {base}.", flush=True)
    script = str(ROOT / "scripts" / "check-tidy.sh")
    environment = dict(os.environ, CLANG_TIDY_LINE_FILTER=line_filter(changed))
    result = subprocess.run([script, preset, *sources], check=False, cwd=ROOT, env=environment)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
