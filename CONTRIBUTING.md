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

## Bundles

Each open issue carries one `bundle:` label naming the area it belongs to
(`bundle:routing`, `bundle:live-editing` and so on). Work takes a bundle
whole: every open issue with that label goes into one pull request that
closes them all (`Closes #N` for each), split into sequential pull requests
only when the change grows past about 1,500 lines. A new issue without a
bundle gets `bundle:triage` (`.github/workflows/issue-bundle.yml`) until it
is assigned one.

```sh
gh issue list --label bundle:routing --state open --limit 200
```

If the list reaches the limit, raise it: a bundle is taken whole.

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

- **Build and fast tests** (the required check's name; it runs every tier
  but `live`): the Quick preset on Ubuntu 26.04 is built once, then four
  parallel jobs run the fast and slow tiers, split by the measured times in
  `cmake/TestBudgets.cmake` (`scripts/ci/ctest_shards.py`), and a fifth runs
  the live/export parity gate (`ctest --preset quick-parity`,
  [tests/README.md](tests/README.md#liveexport-parity-gate)). The check's
  summary lists the failing tests with their last output; each shard's
  artifact holds the JUnit report and full logs. Tests listed in
  `scripts/ci/quarantine.txt`, each with the open issue tracking its CI
  failure, are left out until fixed.
- **Core line coverage**: the `coverage` preset and
  `scripts/check-coverage.sh`, which fails below 90% line coverage of
  `src/` and `include/`; the report is uploaded to Codecov.
- **pre-commit**: the module dependency check
  (`scripts/ci/check_module_deps.py`, [HLD §3](docs/HLD.md#3-modules-and-dependency-direction)),
  then the hooks above, on the files the pull request changes.
- **Python unit tests**: `scripts/ci/python-tests.sh`.
- **Documentation**: `scripts/check-doc-links.py` and a strict MkDocs build
  of the documentation site.

**CodeQL** (`codeql.yml`) analyses Python and the workflows on every pull
request, and C/C++ without a build (build mode `none`) on each push to main
that changes C/C++ source (the `cpp` class of `scripts/ci/changes.py`), with
the default query suite and the CodeQL CLI in the runner's toolcache. On pull
requests the C/C++ job passes without analysing. Results appear in the
repository's code scanning alerts.

An advisory **clang-tidy** job, not required for merging, runs the
`.clang-tidy` checks on the translation units a pull request touches
(`scripts/ci/tidy_changed.py`); `scripts/check-tidy.sh` checks the whole tree.

## Merging

Pull requests are squash-merged by auto-merge
(`gh pr merge N --auto --squash --subject "<title> (#N)" --body ""`), which
waits for every required check. One of them, **Review gate**, is a commit
status that `.github/workflows/review-gate.yml` sets on the head commit
(`scripts/ci/review_gate.py`). It is green only when:

1. CodeRabbit completed a review of the head commit: its check run there
   finished with success and says "Review completed". A head whose check
   run says "Prior review carried forward" or "No files to review" passes
   when an earlier commit of the same pull request had a completed review.
   A skipped, paused or rate-limited review never passes, although GitHub
   counts a skipped check as passed.
2. CodeRabbit posted no rate-limit or paused notice after that review.
3. No review thread is unresolved.

CodeRabbit does not review Release Please pull requests (head branch
`release-please--*` in this repository). Their gate is green when every
other check run and commit status on the head commit finished green and
no review thread is unresolved.

The gate is re-evaluated when CodeRabbit's check run completes, on pushes,
reviews and comments, and every ten minutes for every open pull request,
because resolving a thread fires no event. After a rate limit or pause,
comment `@coderabbitai review` and the gate follows the new review.

## Nightly and on-demand CI runs

These run on `main` every night; none is required for merging.

- **Nightly slow tier**: the slow tier and core line coverage (`nightly.yml`).
- **Sanitizers** (`sanitizers.yml`): the `sanitize` preset with the VST3
  host and the optional adapters runs every tier but `live` under
  ASan/UBSan; the `tsan` preset runs the `engine` area (the audio worker
  pool among it) and the routed playback cases. A failed run uploads the
  sanitizer reports as an artifact.
- **CodeQL with a build** (`codeql.yml`, weekly): C/C++ analysed from a
  traced build of the Quick preset, for full type information, under its own
  category (`/language:c-cpp/build-mode:manual`).
- **Tidy sweep** (`tidy-sweep.yml`): the `.clang-tidy` checks over every
  configured translation unit, with the VST3 host and the optional
  adapters, in eight parallel shards (`scripts/ci/tidy_shard.py`). The run's
  summary counts the distinct findings by check; the list is an artifact.

The **Test sweep** (`test-sweep.yml`) runs only on demand: every tier but
`live` on the Quick preset with the VST3 host and the optional adapters.
Its optional `tests` input narrows it with a `ctest -R` regex.

Three qualification workflows run every Monday on `main`, on demand, and on
pull requests that change their harness:

- **Live tier** (`live-tier.yml`): `scripts/live-tier.sh` on a PipeWire
  daemon with disposable null sinks and a JACK dummy server
  ([live tier](docs/qualification/live-tier.md)).
- **VST3 matrix** (`vst3-matrix.yml`): the
  [compatibility matrix](docs/qualification/vst3-compatibility-matrix.md)
  over pinned free plug-ins.
- **Reference performance** (`performance.yml`): the light and heavy
  [reference projects](docs/performance/reference-projects.md) through the
  fake callback.

Run any of them on a branch from the command line, then follow the run:

```sh
gh workflow run sanitizers.yml --ref my-branch
gh workflow run test-sweep.yml --ref my-branch
gh workflow run codeql.yml --ref my-branch
gh workflow run test-sweep.yml --ref my-branch -f tests='^native_nova'
gh run watch "$(gh run list --workflow test-sweep.yml --branch my-branch --limit 1 --json databaseId --jq '.[0].databaseId')"
```

`--ref` runs the workflow as that branch defines it, with that branch's own
build cache. A branch older than the workflow needs `main` merged in first.

Before a release, run the broad sweep:
GCC and Clang builds, the fast and slow tiers, clang-format, clang-tidy,
ASan/UBSan, TSan where concurrency changed, and coverage
([AGENTS.md](AGENTS.md#commands)). A release needs evidence for each PRD
release gate (PRD §23); coverage does not waive a blocker. CodeRabbit reviews each pull request with
the rules in `.coderabbit.yaml`, which follow [AGENTS.md](AGENTS.md).

## CI container image

`ghcr.io/jcsduncan/oscillate-ci` is the pinned Ubuntu 26.04 image with the
build dependencies installed (`scripts/ci/Dockerfile`). It comes in two
variants, whose package lists are in `scripts/ci/ci-image-deps.sh`:

- **base**: the README list (`scripts/ci/install-deps.sh`) and the CI tools
  (git, ccache, zstd, curl, gnupg), and no other build dependency, so a
  build in it shows the README list is complete.
- **full**: base plus the optional features (JACK, libsamplerate,
  libsndfile, libmpg123, X11), the .deb and AppImage tools and ffmpeg.

Every job that builds or runs the project uses one of them, pinned by
digest, and starts with `scripts/ci/ci-image-deps.sh <variant>`. That step
does nothing when the image was built from the checkout's
`Dockerfile`, `install-deps.sh` and `ci-image-deps.sh` (the image records
their hash); when a pull request changed one, it installs the variant's
packages on top of the image, so the change is tested before the image is
rebuilt. The live tier and the VST3 matrix install their servers and
plug-in runtimes on top. The install smoke tests keep a clean
`ubuntu:26.04`, and Dependabot (`docker`) keeps the Dockerfile's base
digest current.

When a pull request changes an input of the image, `ci-image.yml` builds
both variants and configures the project in each, which catches a package
the pull request removed. On `main` when an input changes, every Monday (for
security updates) and on demand, it pushes them, tagged `<variant>-<hash of the
inputs>` and `<variant>-latest`, with build provenance attestations. It then
opens the pull request `ci/pin-ci-image`, which moves every
`ghcr.io/jcsduncan/oscillate-ci@sha256:… # <variant>` pin in the workflows
to the new digests; that pull request runs the checks on the new image
before it is merged. It acts with `RELEASE_PLEASE_TOKEN`, which needs the
Workflows permission (read and write) on top of those listed under
Releases.

## Releases

Release Please (`.github/workflows/release-please.yml`) keeps a release pull
request open on `main` and, when it is merged, tags the release and creates
the GitHub release; the tag push starts the packaging workflow, which
attaches the packages. It acts with the `RELEASE_PLEASE_TOKEN` repository
secret: the checks of a pull request opened with a workflow's own token wait
for a maintainer's approval, and a tag pushed with it starts no workflow.
The secret is a fine-grained personal access token with
access to this repository only and these repository permissions: Contents,
Issues and Pull requests, read and write (Metadata read is implied).

## Licence of contributions

By contributing, you agree that your contribution is licensed under the
GNU General Public License version 3 or later, the licence of this project.
New third-party dependencies need compatible terms and the review described
in the [HLD](docs/HLD.md#11-dependency-and-licensing-decisions).
