# Contributing

Thank you for your interest in Oscillate. The project is pre-release, so the
most useful contributions are clear bug reports, plugin compatibility
results and focused fixes.

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

## Reporting a bug

Check the [known issues](https://jcsduncan.github.io/oscillate/known-issues/)
first; if the problem is listed, add what you found to that issue. Otherwise
open a GitHub issue with:

- what you did, what you expected and what happened;
- the commit you built and the build preset or CMake options you used;
- Ubuntu version, audio backend (PipeWire or JACK), buffer size and sample
  rate;
- for a plugin problem, the plugin name, version and vendor;
- for a performance problem, a trace recorded with `OSCILLATE_TRACE` or the
  command palette and summarised with
  [trace_report](tools/trace_report/README.md), if you can.

After a crash, Oscillate saves a [crash report](docs/crash-reports.md) on
your computer and offers it at the next start; read it, then attach it.
A [diagnostics bundle](docs/diagnostics.md) (**Save diagnostics** in the
command palette, or `oscillate --save-diagnostics`) gathers the session
description, build features, plug-in scan, logs and latest crash report in
one zip; read it, then attach it. [Support](docs/support.md) lists which
form to use and what to attach.
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
3. Build with the commands in the [README](README.md#build), add or
   update tests for the changed behaviour, and run the affected tests.
   Deliver a feature as a vertical slice that renders, saves and reopens a
   real project before broadening it.
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
| `perf:` | a performance improvement |
| `revert:` | reverting an earlier commit |
| `test:` | adding or correcting tests |
| `build:` | CMake, presets, dependencies or packaging |
| `ci:` | workflows and CI scripts |
| `chore:` | other maintenance |

`feat`, `fix`, `perf`, `refactor`, `test` and `revert` need a scope: a module
directory under `src/` (`app` is the Qt Quick application), or `tools`,
`deps` or `main`. Several scopes are comma-separated: `feat(app,native_dsp):`.
`docs`, `build`, `ci` and `chore` may omit the scope. Pull request titles are
checked in CI (`scripts/ci/check_pr_title.py`), because the squash commit
takes the title.

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

- **Build and fast tests**: the Quick preset on Ubuntu 26.04, the fast
  test tier without live devices, and the live/export parity gate
  (`ctest --preset quick-parity`, [tests/README.md](tests/README.md#liveexport-parity-gate)).
- **Core line coverage**: the `coverage` preset and
  `scripts/check-coverage.sh`, which fails below 90% line coverage of
  `src/` and `include/`; the report is uploaded to Codecov.
- **pre-commit**: the module dependency check
  (`scripts/ci/check_module_deps.py`, [HLD §3](docs/HLD.md#3-modules-and-dependency-direction)),
  then the hooks above, on the files the pull request changes.
- **Python unit tests**: `scripts/ci/python-tests.sh`.
- **Documentation**: `scripts/check-doc-links.py` and a strict MkDocs build
  of the documentation site.

An advisory **clang-tidy** job, not required for merging, runs the
`.clang-tidy` checks on the translation units a pull request touches
(`scripts/ci/tidy_changed.py`); `scripts/check-tidy.sh` checks the whole tree.

## Nightly and on-demand CI runs

These run on `main` every night; none is required for merging.

- **Nightly slow tier**: the slow tier and core line coverage (`nightly.yml`).
- **Sanitizers** (`sanitizers.yml`): the `sanitize` preset with the VST3
  host and the optional adapters runs every tier but `live` under
  ASan/UBSan; the `tsan` preset runs the `engine` area (the audio worker
  pool among it) and the routed playback cases. A failed run uploads the
  sanitizer reports as an artifact.
- **Tidy sweep** (`tidy-sweep.yml`): the `.clang-tidy` checks over every
  configured translation unit, with the VST3 host and the optional
  adapters, in eight parallel shards (`scripts/ci/tidy_shard.py`). The run's
  summary counts the distinct findings by check; the list is an artifact.

The **Test sweep** (`test-sweep.yml`) runs only on demand: every tier but
`live` on the Quick preset with the VST3 host and the optional adapters.
Its optional `tests` input narrows it with a `ctest -R` regex.

Run any of them on a branch from the command line, then follow the run:

```sh
gh workflow run sanitizers.yml --ref my-branch
gh workflow run test-sweep.yml --ref my-branch
gh workflow run test-sweep.yml --ref my-branch -f tests='^native_nova'
gh run watch "$(gh run list --workflow test-sweep.yml --branch my-branch --limit 1 --json databaseId --jq '.[0].databaseId')"
```

`--ref` runs the workflow as that branch defines it. For a branch older than
the workflow, start it from `main` and pass the branch as the `ref` input:
`gh workflow run test-sweep.yml -f ref=my-branch` (the run is then listed
under `main`).

Before a release, run the broad sweep:
GCC and Clang builds, the fast and slow tiers, clang-format, clang-tidy,
ASan/UBSan, TSan where concurrency changed, and coverage
([AGENTS.md](AGENTS.md#commands)). A release needs evidence for each PRD
release gate (PRD §23); coverage does not waive a blocker. CodeRabbit reviews each pull request with
the rules in `.coderabbit.yaml`, which follow [AGENTS.md](AGENTS.md).

## Licence of contributions

By contributing, you agree that your contribution is licensed under the
GNU General Public License version 3 or later, the licence of this project.
New third-party dependencies need compatible terms and the review described
in the [HLD](docs/HLD.md#11-dependency-and-licensing-decisions).
