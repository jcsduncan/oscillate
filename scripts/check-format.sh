#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format is required (install clang-format)." >&2
  exit 1
fi

mapfile -d '' files < <(find apps include src tests -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) -print0)
if ((${#files[@]})); then
  clang-format --dry-run --Werror "${files[@]}"
fi
