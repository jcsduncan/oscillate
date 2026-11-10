#!/usr/bin/env bash
# Builds both variants of the CI container image (scripts/ci/Dockerfile) from
# the repository root, tagged <variant>-<hash> and <variant>-latest, and adds
# their sizes to the job summary. Usage: build-ci-image.sh IMAGE HASH
set -euo pipefail

image=${1:?usage: build-ci-image.sh IMAGE HASH}
hash=${2:?usage: build-ci-image.sh IMAGE HASH}
readonly MIB=$((1024 * 1024))

cd "$(dirname "$0")/../.."
for variant in base full; do
  docker build --target "$variant" -f scripts/ci/Dockerfile \
    -t "$image:$variant-$hash" -t "$image:$variant-latest" .
done
{
  echo "| Variant | Size |"
  echo "| --- | --- |"
  for variant in base full; do
    size=$(docker image inspect --format '{{.Size}}' "$image:$variant-$hash")
    echo "| $variant | $((size / MIB)) MiB |"
  done
} | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
