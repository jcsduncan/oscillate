#!/usr/bin/env bash
# Build without endangering the desktop: the build runs in its own systemd
# scope with a memory ceiling, at low CPU and I/O priority and six jobs, so memory
# pressure throttles or stops the build instead of the session's OOM killer
# taking the desktop. Without a user systemd manager (containers, CI) it
# builds uncapped. Usage: scripts/build.sh [BUILD_DIR] [cmake --build args...]
set -euo pipefail

build_dir=${1:-build/quick}
shift || true
jobs=${OSCILLATE_BUILD_JOBS:-6}
memory_high=${OSCILLATE_BUILD_MEMORY_HIGH:-10G}
memory_max=${OSCILLATE_BUILD_MEMORY_MAX:-14G}

mkdir -p "$build_dir/scratch"
TMPDIR=$(cd "$build_dir/scratch" && pwd)
export TMPDIR
build=(nice -n 15 ionice -c2 -n7 cmake --build "$build_dir" -j"$jobs" "$@")
# Containers and CI runners have no user systemd manager; build uncapped there.
if systemd-run --user --scope --quiet --collect true 2>/dev/null; then
  exec systemd-run --user --scope --quiet --collect \
    -p MemoryHigh="$memory_high" -p MemoryMax="$memory_max" -p MemorySwapMax=0 \
    "${build[@]}"
fi
echo "build.sh: no user systemd manager; building without a memory cap" >&2
exec "${build[@]}"
