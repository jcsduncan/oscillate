# Contributing

Thank you for your interest in Oscillate. The project is pre-release, so the
most useful contributions are clear bug reports, plugin compatibility
results and focused fixes.

## Reporting a bug

Open a GitHub issue with:

- what you did, what you expected and what happened;
- the commit you built and the build preset or CMake options you used;
- Ubuntu version, audio backend (PipeWire or JACK), buffer size and sample
  rate;
- for a plugin problem, the plugin name, version and vendor;
- for a performance problem, a trace recorded with `OSCILLATE_TRACE` or the
  command palette and summarised with
  [trace_report](tools/trace_report/README.md), if you can.

Attach a small project that reproduces the problem when possible. Do not
attach copyrighted media you are not allowed to share. For a security
problem, follow [SECURITY.md](SECURITY.md) instead of opening a public issue.

## Proposing a change

1. For anything beyond a small fix, open an issue first and describe the
   behaviour you want. Changes to scope follow the [PRD](docs/PRD.md);
   changes to module boundaries, the project format or a real-time contract
   need an ADR in [docs/decisions](docs/decisions/README.md).
2. Follow [AGENTS.md](AGENTS.md): real-time rules, data-integrity rules,
   DSP measurement policy, test registration and the local checks.
3. Build with the commands in the [README](README.md#build-and-run), add or
   update tests for the changed behaviour, and run the affected tests.
4. Open a pull request against `main` with one coherent change. Describe the
   requirement IDs, any API or schema change, the tests you ran, and the
   real-time and licensing effects.

## Commit messages

Commits and pull request titles follow
[Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/):
`<type>(<optional scope>): <summary>`, for example
`fix(export): keep stem names when the master export fails`. Use one of
these types:

| Type | For |
| --- | --- |
| `feat:` | a user-visible feature |
| `fix:` | a bug fix |
| `docs:` | documentation only |
| `refactor:` | a code change that neither fixes a bug nor adds a feature |
| `test:` | adding or correcting tests |
| `build:` | CMake, presets, dependencies or packaging |
| `ci:` | workflows and CI scripts |
| `chore:` | other maintenance |

Mark a breaking change with `!` after the type (`feat!:`) or a
`BREAKING CHANGE:` footer. Release Please reads these messages on `main` to
choose the next version and write `CHANGELOG.md`, so do not edit the
changelog or the version in `CMakeLists.txt` by hand.

## Local tooling

Install the [pre-commit](https://pre-commit.com/) hooks once per checkout:

```sh
python3 -m venv ~/.venvs/oscillate-lint
~/.venvs/oscillate-lint/bin/pip install -r scripts/ci/lint-requirements.txt
~/.venvs/oscillate-lint/bin/pre-commit install
```

On each commit the hooks check the staged files only: trailing whitespace,
final newlines, YAML syntax and merge-conflict markers, clang-format for
C/C++ (the release pinned in `scripts/qa-requirements.txt`), Ruff lint and
format for Python (`ruff.toml`), and actionlint for workflows. The tree is not
yet clang-formatted as a whole, so the hook only reformats files you touch.
Run `pre-commit run --from-ref origin/main --to-ref HEAD` to check a whole
branch. Python scripts run with `python3 -I`, keep their imports at the top
of the file and do not use `hasattr` in place of knowing an API.

## Pull request checks

A pull request to `main` runs these checks; they must pass before merging.
`scripts/ci/changes.py` skips the build, Python and documentation jobs when
a pull request changes none of their inputs; a skipped job counts as passed.

- **Build and fast tests**: the Quick preset on Ubuntu 26.04 and the fast
  test tier without live devices.
- **Core line coverage**: the `coverage` preset and
  `scripts/check-coverage.sh`, which fails below 90% line coverage of
  `src/` and `include/`; the report is uploaded to Codecov.
- **pre-commit**: the hooks above, on the files the pull request changes.
- **Python unit tests**: `scripts/ci/python-tests.sh`.
- **Documentation**: `scripts/check-doc-links.py` and a strict MkDocs build
  of the documentation site.

The slow test tier runs nightly. CodeRabbit reviews each pull request with
the rules in `.coderabbit.yaml`, which follow [AGENTS.md](AGENTS.md).

## Licence of contributions

By contributing, you agree that your contribution is licensed under the
GNU General Public License version 3 or later, the licence of this project.
New third-party dependencies need compatible terms and the review described
in the [HLD](docs/HLD.md#11-dependency-and-licensing-decisions).
