#!/usr/bin/env python3
"""Check local inline Markdown links without fetching remote URLs.

This intentionally checks file targets, not heading anchors or reference-style
links. It covers the link forms used in the repository documentation.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

ROOT = pathlib.Path(__file__).resolve().parent.parent
LINK = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")


def markdown_files() -> list[pathlib.Path]:
    names = subprocess.check_output(
        ["git", "ls-files", "-co", "--exclude-standard", "-z", "--", "*.md"],
        cwd=ROOT,
    ).split(b"\0")
    return [ROOT / name.decode("utf-8", "surrogateescape") for name in names if name]


def main() -> int:
    missing: list[tuple[pathlib.Path, str]] = []
    files = markdown_files()
    for file in files:
        content = file.read_text(encoding="utf-8")
        for match in LINK.finditer(content):
            raw = match.group(1).split(" ", 1)[0].strip("<>")
            if not raw or raw.startswith("#"):
                continue
            parsed = urlsplit(raw)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            target = (file.parent / unquote(parsed.path)).resolve()
            if not target.exists():
                missing.append((file.relative_to(ROOT), raw))
    for file, link in missing:
        print(f"{file}: missing local link {link}", file=sys.stderr)
    print(f"Checked {len(files)} Markdown files; {len(missing)} missing local links")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
