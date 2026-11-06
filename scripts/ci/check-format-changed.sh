#!/usr/bin/env bash
# Checks clang-format on the C/C++ files a change touches, against BASE (a
# commit, default origin/main); the same scope as the pre-commit clang-format
# hook. scripts/check-format.sh checks the whole tree.
# Usage: scripts/ci/check-format-changed.sh [BASE]
set -euo pipefail

cd "$(dirname "$0")/../.."
base=${1:-origin/main}
if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format is required (install clang-format)." >&2
  exit 1
fi

mapfile -d '' files < <(git diff -z --name-only --diff-filter=ACMR "$base" HEAD -- \
  'apps/*.cpp' 'apps/*.hpp' 'apps/*.h' 'apps/*.c' \
  'include/*.cpp' 'include/*.hpp' 'include/*.h' 'include/*.c' \
  'src/*.cpp' 'src/*.hpp' 'src/*.h' 'src/*.c' \
  'tests/*.cpp' 'tests/*.hpp' 'tests/*.h' 'tests/*.c' \
  'bench/*.cpp' 'bench/*.hpp' 'bench/*.h' 'bench/*.c' \
  'tools/*.cpp' 'tools/*.hpp' 'tools/*.h' 'tools/*.c')
if ((${#files[@]} == 0)); then
  echo "No C/C++ files changed against $base."
  exit 0
fi
echo "Checking ${#files[@]} changed file(s) against $base."
clang-format --dry-run --Werror "${files[@]}"
