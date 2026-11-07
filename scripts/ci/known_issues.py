"""Write the known-issues page from open issues labelled known-issue.

The Pages workflow runs this before `mkdocs build` and points the
OSCILLATE_KNOWN_ISSUES environment variable at the output, which
scripts/ci/mkdocs_hooks.py publishes as known-issues.md. The page is built
fresh on every deploy and never committed.

    python3 -I scripts/ci/known_issues.py --repository jcsduncan/oscillate \
        --output "$RUNNER_TEMP/known-issues.md"

GITHUB_TOKEN, when set, authenticates the GitHub API requests.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.parse
import urllib.request
from collections.abc import Callable
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

LABEL = "known-issue"
API_URL = "https://api.github.com"
PER_PAGE = 100
# 1000 open known issues is far beyond any real list; the bound stops a
# misbehaving API from looping forever.
MAX_PAGES = 10
TIMEOUT_SECONDS = 30
# Characters that would start Markdown formatting inside a title.
MARKDOWN_SPECIAL = "\\`*_{}[]()#+!|~"

Fetch = Callable[[str], list[dict[str, Any]]]


def label_url(repository: str) -> str:
    """The issue list on GitHub, filtered to open known issues."""
    query = urllib.parse.quote(f'is:issue is:open label:"{LABEL}"')
    return f"https://github.com/{repository}/issues?q={query}"


def fetch_page(url: str) -> list[dict[str, Any]]:
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    token = os.environ.get("GITHUB_TOKEN", "")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
        return json.load(response)


def open_known_issues(repository: str, fetch: Fetch = fetch_page) -> list[dict[str, Any]]:
    """Every open issue with the label; pull requests are left out."""
    issues: list[dict[str, Any]] = []
    for page in range(1, MAX_PAGES + 1):
        query = urllib.parse.urlencode(
            {"state": "open", "labels": LABEL, "per_page": PER_PAGE, "page": page}
        )
        batch = fetch(f"{API_URL}/repos/{repository}/issues?{query}")
        issues.extend(item for item in batch if "pull_request" not in item)
        if len(batch) < PER_PAGE:
            return issues
    raise RuntimeError(f"more than {MAX_PAGES * PER_PAGE} open {LABEL} issues")


def escape(text: str) -> str:
    """A title as plain Markdown text: no links, emphasis or HTML."""
    escaped = "".join(f"\\{c}" if c in MARKDOWN_SPECIAL else c for c in text)
    return (
        escaped.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\n", " ")
    )


def render(issues: list[dict[str, Any]], repository: str, generated: datetime) -> str:
    """The page: newest-updated issue first, each with its other labels."""
    lines = [
        "# Known issues",
        "",
        "Problems we know about and have not fixed yet. This list is built from the",
        f"[open issues labelled `{LABEL}`]({label_url(repository)}) each time the site",
        f"is published; it was last built on {generated:%Y-%m-%d %H:%M} UTC.",
        "",
        "Before reporting a problem, check whether it is here. If it is, add what you",
        "found to that issue rather than opening a new one.",
        "",
    ]
    if not issues:
        lines.append("No known issues are open.")
        return "\n".join(lines) + "\n"
    lines += ["| Issue | Labels | Updated |", "| --- | --- | --- |"]
    ordered = sorted(issues, key=lambda issue: (issue["updated_at"], issue["number"]), reverse=True)
    for issue in ordered:
        labels = sorted(
            label["name"] for label in issue.get("labels", []) if label["name"] != LABEL
        )
        shown = ", ".join(escape(name) for name in labels) or "–"
        title = f"[#{issue['number']} {escape(issue['title'])}]({issue['html_url']})"
        lines.append(f"| {title} | {shown} | {issue['updated_at'][:10]} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repository", required=True, help="owner/name")
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args(argv)
    issues = open_known_issues(arguments.repository)
    page = render(issues, arguments.repository, datetime.now(UTC))
    arguments.output.write_text(page, encoding="utf-8")
    print(f"{len(issues)} known issues written to {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
