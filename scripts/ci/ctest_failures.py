"""Lists the failed tests in CTest JUnit reports as a Markdown summary.

Usage: ctest_failures.py REPORT.xml...
Each report's parent directory names its shard. Prints a Markdown table of
the tests with a <failure> (failed or timed out; skipped tests have none) for
$GITHUB_STEP_SUMMARY, or a line saying none failed. A missing report is
listed too: its shard never finished.
"""

import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

EXCERPT_LINES = 3


def failures(report_text):
    """(test name, failure message, output excerpt) for each failing testcase."""
    root = ElementTree.fromstring(report_text)
    found = []
    for case in root.iter("testcase"):
        failure = case.find("failure")
        if failure is None:
            continue
        output = case.findtext("system-out") or ""
        tail = [line for line in output.splitlines() if line.strip()][-EXCERPT_LINES:]
        found.append((case.get("name", "?"), failure.get("message", "fail"), " / ".join(tail)))
    return found


def cell(text):
    return text.replace("|", "\\|").replace("\n", " ")


def summary(reports):
    """Markdown for {shard: report text or None}."""
    rows = []
    for shard, text in sorted(reports.items()):
        if text is None:
            rows.append((shard, "(no report)", "missing", "the shard did not finish"))
            continue
        rows.extend((shard, name, status, excerpt) for name, status, excerpt in failures(text))
    if not rows:
        return "No test failed.\n"
    lines = [
        f"{len(rows)} failing test(s); each shard's artifact holds the full logs.",
        "",
        "| Shard | Test | Status | Last output |",
        "| --- | --- | --- | --- |",
    ]
    lines += [f"| {cell(a)} | `{cell(b)}` | {cell(c)} | {cell(d)} |" for a, b, c, d in rows]
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) < 2:
        sys.exit("Usage: ctest_failures.py REPORT.xml...")
    reports = {}
    for argument in sys.argv[1:]:
        path = Path(argument)
        reports[path.parent.name] = path.read_text(encoding="utf-8") if path.is_file() else None
    sys.stdout.write(summary(reports))


if __name__ == "__main__":
    main()
