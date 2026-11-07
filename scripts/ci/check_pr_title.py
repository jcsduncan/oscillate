"""Check a pull request title against the project's Conventional Commits rules.

Usage: check_pr_title.py "<title>"
Squash merges use the title as the commit subject, and Release Please builds
the changelog from it, so every title must be `type(scope): summary`.

- feat, fix, perf, refactor, test and revert need a scope.
- docs, build, ci and chore may omit it (Dependabot and Release Please
  titles, repository-wide changes).
- A scope names a module directory under src/ (app covers the Qt Quick
  application and plugin editor windows) or one of EXTRA_SCOPES.
"""

import pathlib
import re
import sys

SCOPED_TYPES = ("feat", "fix", "perf", "refactor", "test", "revert")
UNSCOPED_TYPES = ("docs", "build", "ci", "chore")
EXTRA_SCOPES = ("tools", "deps", "main")
TITLE = re.compile(
    r"^(?P<type>[a-z]+)(?:\((?P<scope>[a-z0-9_,]+)\))?(?P<breaking>!)?: (?P<summary>\S.*)$"
)
REPO = pathlib.Path(__file__).resolve().parents[2]


def known_scopes(root=REPO):
    modules = {p.name for p in (root / "src").iterdir() if p.is_dir()}
    return modules | set(EXTRA_SCOPES)


def problems(title, scopes):
    match = TITLE.match(title)
    if not match:
        return ["title must be `type(scope): summary`"]
    kind, scope = match["type"], match["scope"]
    if kind not in SCOPED_TYPES + UNSCOPED_TYPES:
        return [f"unknown type `{kind}`; use one of {', '.join(SCOPED_TYPES + UNSCOPED_TYPES)}"]
    found = []
    if scope is None and kind in SCOPED_TYPES:
        found.append(f"`{kind}` needs a scope, e.g. `{kind}(engine): …`")
    for part in (scope or "").split(","):
        if part and part not in scopes:
            extras = ", ".join(EXTRA_SCOPES)
            found.append(f"unknown scope `{part}`; use a module under src/ or one of {extras}")
    return found


def main():
    title = sys.argv[1] if len(sys.argv) > 1 else ""
    found = problems(title, known_scopes())
    for problem in found:
        print(f"PR title: {problem}")
    sys.exit(1 if found else 0)


if __name__ == "__main__":
    main()
