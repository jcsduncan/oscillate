"""Classify a pull request's changed files for CI job selection.

Usage: changes.py BASE_SHA HEAD_SHA
Writes code=, python= and docs= (true/false) to $GITHUB_OUTPUT, or to stdout
when that variable is unset. With no BASE_SHA (a push to main or a manual
run) every class is true.

Anything not known to be documentation counts as code, so an unfamiliar
path always builds.
"""

import fnmatch
import os
import subprocess
import sys

# Paths that never affect the compiled tree or the C++ tests.
DOC_ONLY = (
    "docs/*",
    "*.md",
    "mkdocs.yml",
    "LICENSE",
    ".coderabbit.yaml",
    "codecov.yml",
    ".github/ISSUE_TEMPLATE/*",
    ".github/workflows/docs.yml",
    ".github/workflows/pages.yml",
    "scripts/check-doc-links.py",
    "scripts/ci/docs-requirements.txt",
    "scripts/ci/mkdocs_hooks.py",
)
# Paths whose change needs the Python suites.
PYTHON = (
    "*.py",
    "ruff.toml",
    "scripts/ci/python-tests.sh",
    ".github/workflows/python.yml",
)
# Paths whose change needs the documentation build.
DOCS = DOC_ONLY + ("scripts/ci/changes.py",)
CLASSES = ("code", "python", "docs")


def matches(path, patterns):
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


def changed_files(base, head):
    result = subprocess.run(
        ["git", "diff", "--name-only", f"{base}...{head}"],
        check=True,
        capture_output=True,
        text=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def classify(files):
    return {
        "code": any(not matches(f, DOC_ONLY) for f in files),
        "python": any(matches(f, PYTHON) for f in files),
        "docs": any(matches(f, DOCS) for f in files),
    }


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else ""
    head = sys.argv[2] if len(sys.argv) > 2 else "HEAD"
    flags = classify(changed_files(base, head)) if base else dict.fromkeys(CLASSES, True)
    lines = "".join(f"{name}={'true' if flags[name] else 'false'}\n" for name in CLASSES)
    target = os.environ.get("GITHUB_OUTPUT")
    if target:
        with open(target, "a", encoding="utf-8") as output:
            output.write(lines)
    else:
        sys.stdout.write(lines)


if __name__ == "__main__":
    main()
