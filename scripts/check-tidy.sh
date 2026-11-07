#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
if (($# > 0)) && [[ ! "$1" =~ ^[a-z0-9_-]+$ ]]; then
  echo "Usage: $0 [configured-preset] [source.cpp ...]" >&2
  echo "CLANG_TIDY_LINE_FILTER, when set, is passed as clang-tidy's --line-filter." >&2
  exit 2
fi
preset="${1:-dev}"
if (($# > 0)); then
  shift
fi
if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "clang-tidy is required (install clang-tidy)." >&2
  exit 1
fi
if [[ ! -f "build/$preset/compile_commands.json" ]]; then
  echo "Configure first: cmake --preset $preset" >&2
  exit 1
fi

# Only inspect translation units configured in this compilation database.
# A source may have several feature configurations. Run every translation unit
# in its own clang-tidy process. Reusing one process across unrelated sources
# can carry broken parser state into later files and invent diagnostics there.
shard_root="build/$preset/tidy-shards"
python3 - "$preset" "$shard_root" "$@" <<'PY'
import json
import os
from pathlib import Path
import sys

root = Path.cwd().resolve()
entries = json.loads((Path('build') / sys.argv[1] / 'compile_commands.json').read_text())
shard_root = Path(sys.argv[2])
shard_root.mkdir(exist_ok=True)
by_file = {}
requested = {Path(argument).resolve() for argument in sys.argv[3:]}
for entry in entries:
    path = Path(entry['file']).resolve()
    try:
        relative = path.relative_to(root)
    except ValueError:
        continue
    if relative.parts[0] in {'apps', 'src', 'tests'} and path.suffix == '.cpp':
        by_file.setdefault(path, []).append(entry)
unknown = requested - by_file.keys()
if unknown:
    raise SystemExit('No configured C++ translation unit for: ' +
                     ', '.join(str(path) for path in sorted(unknown)))
if requested:
    by_file = {path: entries for path, entries in by_file.items() if path in requested}
shards = []
for index in range(max(map(len, by_file.values()), default=0)):
    shard = shard_root / str(index)
    shard.mkdir(exist_ok=True)
    selected = [(path, commands[index]) for path, commands in sorted(by_file.items())
                if len(commands) > index]
    (shard / 'compile_commands.json').write_text(json.dumps([entry for _, entry in selected]))
    (shard / 'files').write_bytes(b''.join(os.fsencode(path) + b'\0' for path, _ in selected))
    shards.append(shard)
(shard_root / 'index').write_bytes(b''.join(os.fsencode(shard) + b'\0' for shard in shards))
PY
mapfile -d '' shards < "$shard_root/index"
# Check every file, then fail if any reported, so one run lists all findings.
status=0
for shard in "${shards[@]}"; do
  mapfile -d '' files < "$shard/files"
  for file in "${files[@]}"; do
    clang-tidy ${CLANG_TIDY_LINE_FILTER:+"--line-filter=$CLANG_TIDY_LINE_FILTER"} -p "$shard" "$file" ||
      status=1
  done
done
exit "$status"
