"""Runs scripts/check-tidy.sh on the translation units a change touches.

Usage: tidy_changed.py PRESET BASE
BASE is the commit the change is measured against. In CI it is HEAD^1: the
pull request's merge commit's first parent, the base branch tip it merges
into, so commits that reached the base after the pull request opened never
count as the change's own.
A changed C++ source is checked itself. A changed header is checked through
the source of the same name beside it, or under src/ for a public header in
include/oscillate/. Only findings on the lines the change adds or edits are
reported, so a change is not held to older findings in the files it touches
or in the unchanged headers it includes.
Sources missing from the preset's compilation database (a feature the preset
leaves out) are skipped. scripts/check-tidy.sh checks the whole tree.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE_DIRS = ("apps", "src", "tests")
HEADER_SUFFIXES = (".hpp", ".h")
PUBLIC_HEADER_ROOT = PurePosixPath("include/oscillate")
# A clang-tidy diagnostic's primary line: path:line:column: severity: text [check].
DIAGNOSTIC = re.compile(
    r"^(?P<path>[^:\s][^:]*):(?P<line>\d+):\d+: (?:warning|error): .*\[[\w.,-]+\]$"
)
# A unified diff's file header and hunk header (new side: start, optional count).
DIFF_FILE = re.compile(r"^\+\+\+ b/(?P<path>.+)$")
DIFF_HUNK = re.compile(r"^@@ -\S+ \+(?P<start>\d+)(?:,(?P<count>\d+))? @@")
CPP_SUFFIXES = (".cpp", *HEADER_SUFFIXES)


def added_lines(diff):
    """{path: [[first, last], ...]} of the lines a zero-context diff adds or edits."""
    lines = {}
    path = None
    for text in diff.splitlines():
        if match := DIFF_FILE.match(text):
            path = match["path"]
            continue
        match = DIFF_HUNK.match(text)
        if not match or path is None:
            continue
        count = int(match["count"]) if match["count"] is not None else 1
        if count == 0:
            continue
        start = int(match["start"])
        lines.setdefault(path, []).append([start, start + count - 1])
    return lines


def changed_lines(base, cwd=ROOT):
    result = subprocess.run(
        ["git", "diff", "-U0", "--no-color", "--diff-filter=ACMR", f"{base}...HEAD"],
        check=True,
        capture_output=True,
        text=True,
        cwd=cwd,
    )
    return added_lines(result.stdout)


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


def line_filter(lines):
    """clang-tidy --line-filter JSON: each changed C++ file and its changed lines."""
    return json.dumps(
        [
            {"name": path, "lines": ranges}
            for path, ranges in lines.items()
            if PurePosixPath(path).suffix in CPP_SUFFIXES
        ]
    )


def changed_findings(output, lines, root):
    """The diagnostics in `output` whose primary location is a changed line.

    The line filter can keep a warning elsewhere when one of its notes falls
    on a changed line, so the primary location decides.
    """
    wanted = {os.path.normpath(path): ranges for path, ranges in lines.items()}
    found = []
    for text in output.splitlines():
        match = DIAGNOSTIC.match(text)
        if not match:
            continue
        path = os.path.normpath(match["path"])
        if os.path.isabs(path):
            path = os.path.relpath(path, root)
        line = int(match["line"])
        if any(first <= line <= last for first, last in wanted.get(path, [])):
            found.append(text)
    return found


def configured_sources(preset):
    database = ROOT / "build" / preset / "compile_commands.json"
    with open(database, encoding="utf-8") as stream:
        entries = json.load(stream)
    root = os.path.realpath(ROOT)
    return {
        os.path.relpath(os.path.realpath(os.path.join(entry["directory"], entry["file"])), root)
        for entry in entries
    }


def main():
    if len(sys.argv) != 3:
        sys.exit("Usage: tidy_changed.py PRESET BASE")
    preset, base = sys.argv[1], sys.argv[2]
    lines = changed_lines(base)
    sources = translation_units(list(lines), configured_sources(preset))
    if not sources:
        print(f"No configured C++ translation unit changed against {base}.")
        return
    print(f"Checking {len(sources)} translation unit(s) against {base}.", flush=True)
    script = str(ROOT / "scripts" / "check-tidy.sh")
    environment = dict(os.environ, CLANG_TIDY_LINE_FILTER=line_filter(lines))
    result = subprocess.run(
        [script, preset, *sources],
        check=False,
        cwd=ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    print(result.stdout, flush=True)
    findings = changed_findings(result.stdout, lines, os.path.realpath(ROOT))
    if findings:
        print(f"{len(findings)} finding(s) on the changed lines:")
        print("\n".join(findings))
        sys.exit(1)
    # A compile error anywhere means a changed translation unit was not checked;
    # a failed run with no parsed diagnostic is a tool failure.
    parsed = [line for line in result.stdout.splitlines() if DIAGNOSTIC.match(line)]
    if any("[clang-diagnostic-error" in line for line in parsed):
        print("A changed translation unit failed to compile.")
        sys.exit(1)
    if result.returncode != 0 and not parsed:
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()
