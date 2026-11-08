"""Splits the configured CTest tests into shards of about equal run time.

Usage: ctest_shards.py BUILD_DIR COUNT OUTPUT_DIR
Writes OUTPUT_DIR/shard-<n>.txt (n = 1..COUNT), one test name per line, for
`ctest --tests-from-file`. Live tests (devices), parity tests (their own
gate job) and the tests in scripts/ci/quarantine.txt (each tracked by an open
issue) are left out. A test's weight is its measured time, a third of its
budget in cmake/TestBudgets.cmake (the budget is three times the measured
-j4 time); a test without an entry is under the fast default and weighs
UNLISTED_SECONDS. The heaviest test goes to the lightest shard first.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BUDGETS = ROOT / "cmake" / "TestBudgets.cmake"
QUARANTINE = Path(__file__).resolve().parent / "quarantine.txt"
EXCLUDED_LABELS = ("live", "parity")
# cmake/TestBudgets.cmake: budget = BUDGET_PER_MEASURED x measured time.
BUDGET_PER_MEASURED = 3
# A fast test without a budget entry measured under the fast default; most
# take well under a second.
UNLISTED_SECONDS = 1.0
BUDGET_LINE = re.compile(r"^set\(OSCILLATE_TEST_BUDGET_(\S+)\s+(\d+(?:\.\d+)?)\)\s*$")


def measured_seconds(budget_text):
    """Test name -> measured seconds, from the TestBudgets.cmake text."""
    seconds = {}
    for line in budget_text.splitlines():
        match = BUDGET_LINE.match(line)
        if match:
            seconds[match.group(1)] = float(match.group(2)) / BUDGET_PER_MEASURED
    return seconds


def quarantined(text):
    """Test name -> issue number, from the quarantine.txt text."""
    tests = {}
    for number, line in enumerate(text.splitlines(), start=1):
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if len(fields) != 2 or not fields[1].isdigit():
            raise ValueError(f"quarantine line {number} is not '<test> <issue number>': {line}")
        tests[fields[0]] = int(fields[1])
    return tests


def selected_tests(ctest_json, skipped=()):
    """Names of the tests in `ctest --show-only=json-v1` output to shard."""
    names = []
    for test in ctest_json["tests"]:
        labels = []
        for prop in test.get("properties", []):
            if prop["name"] == "LABELS":
                labels = prop["value"]
        if test["name"] in skipped:
            continue
        if not any(label in EXCLUDED_LABELS for label in labels):
            names.append(test["name"])
    return names


def split(names, seconds, count):
    """COUNT lists of names, heaviest first into the lightest shard."""
    if count < 1:
        raise ValueError(f"shard count {count} is below 1")
    shards = [[] for _ in range(count)]
    loads = [0.0] * count
    ordered = sorted(names, key=lambda name: (-seconds.get(name, UNLISTED_SECONDS), name))
    for name in ordered:
        lightest = min(range(count), key=lambda index: (loads[index], index))
        shards[lightest].append(name)
        loads[lightest] += seconds.get(name, UNLISTED_SECONDS)
    return shards, loads


def main():
    if len(sys.argv) != 4:
        sys.exit("Usage: ctest_shards.py BUILD_DIR COUNT OUTPUT_DIR")
    build_dir, count, output = sys.argv[1], int(sys.argv[2]), Path(sys.argv[3])
    listing = subprocess.run(
        ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    skipped = quarantined(QUARANTINE.read_text(encoding="utf-8"))
    for name, issue in sorted(skipped.items()):
        print(f"quarantined: {name} (#{issue})")
    names = selected_tests(json.loads(listing.stdout), skipped)
    seconds = measured_seconds(BUDGETS.read_text(encoding="utf-8"))
    shards, loads = split(names, seconds, count)
    output.mkdir(parents=True, exist_ok=True)
    for index, (shard, load) in enumerate(zip(shards, loads, strict=True), start=1):
        (output / f"shard-{index}.txt").write_text("".join(f"{name}\n" for name in shard))
        print(f"shard {index}: {len(shard)} tests, {load:.0f} s of measured test time")


if __name__ == "__main__":
    main()
