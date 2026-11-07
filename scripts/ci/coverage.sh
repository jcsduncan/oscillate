#!/usr/bin/env bash
# CI coverage report for the `coverage` preset, after its build and tests.
# 1. scripts/check-coverage.sh enforces the core line floor (90%, measured
#    90.81% in docs/quality/full-coverage-baseline.md) and prints the summary.
# 2. A second gcovr pass writes build/coverage/coverage.xml (Cobertura) for
#    Codecov. Keep its filters and parse options in step with
#    scripts/check-coverage.sh so both report the same source scope.
# Usage: scripts/ci/coverage.sh
set -euo pipefail

cd "$(dirname "$0")/../.."
preset=coverage
scripts/check-coverage.sh "$preset"

gcovr --root . --filter 'src/' --filter 'include/' \
  --exclude '^src/audio_io/pipewire_output\.cpp$' \
  --merge-mode-functions merge-use-line-min \
  --gcov-ignore-parse-errors suspicious_hits.warn_once_per_file \
  --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
  --cobertura-pretty --cobertura "build/$preset/coverage.xml" \
  "build/$preset"
echo "Wrote build/$preset/coverage.xml"
