#!/usr/bin/env bash
# Fetches the pinned VST3 SDK release with only the pluginterfaces submodule,
# which is all the host compiles against (docs/dependencies/vst3-sdk-source.md).
# CMake then checks the release manifest, headers and notices by digest.
# Usage: scripts/ci/fetch-vst3-sdk.sh DESTINATION
set -euo pipefail

destination=$1
tag=v3.8.1_build_84
commit=3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96

git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" \
  https://github.com/steinbergmedia/vst3sdk.git "$destination"
actual=$(git -C "$destination" rev-parse HEAD)
if [[ $actual != "$commit" ]]; then
  echo "fetch-vst3-sdk: $tag is $actual, expected $commit" >&2
  exit 1
fi
git -C "$destination" submodule update --quiet --init --depth 1 pluginterfaces
