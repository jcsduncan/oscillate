#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
if (($# > 1)) || { (($# == 1)) && [[ "$1" != coverage && "$1" != coverage-full ]]; }; then
  echo "Usage: $0 [coverage|coverage-full]" >&2
  exit 2
fi
preset="${1:-coverage}"
if ! command -v gcovr >/dev/null 2>&1; then
  echo "gcovr is required (install gcovr)." >&2
  exit 1
fi
if [[ ! -f "build/$preset/CMakeCache.txt" ]]; then
  echo "Configure and test first: cmake --preset $preset; cmake --build --preset $preset; ctest --preset $preset" >&2
  exit 1
fi

filters=(--filter 'src/' --filter 'include/')
gate=()
if [[ "$preset" == coverage-full ]]; then
  filters+=(--filter 'apps/daw_quick/')
  echo "Full C++ coverage report (no percentage gate while optional targets are brought under test)." >&2
else
  gate=(--fail-under-line 90)
fi
# GCC 15 gcov emits suspicious and negative branch counters even after a clean
# coverage run. gcovr reports each affected file and retains its valid lines.
# An inline function can appear on different source lines in separate GCC
# translation units. Merge those records at the earliest source line; this
# affects function attribution, while the line coverage gate stays unchanged.
gcovr --root . "${filters[@]}" \
  --exclude '^src/audio_io/pipewire_output\.cpp$' \
  --merge-mode-functions merge-use-line-min \
  --gcov-ignore-parse-errors suspicious_hits.warn_once_per_file \
  --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
  "${gate[@]}" "build/$preset"
