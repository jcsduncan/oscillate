#!/usr/bin/env bash
# The packages of the CI container image (scripts/ci/Dockerfile,
# CONTRIBUTING.md "CI container image"), and the step that keeps a job's
# container current with this checkout.
#
#   ci-image-deps.sh base|full   install the variant's packages unless the
#                                container already holds them for these inputs
#   ci-image-deps.sh --hash      print the hash of the image's inputs
#
# The image records the hash of its inputs and its variant in STAMP. A job
# whose checkout changed an input (a pull request editing install-deps.sh,
# say, or main before the new image's digest is pinned) gets a different
# hash, so this script installs the packages on top of the image; apt skips
# the ones already there. Runs as root in an Ubuntu 26.04 container.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
readonly STAMP=/etc/oscillate-ci/inputs
readonly INPUTS=(Dockerfile install-deps.sh ci-image-deps.sh)

# Every job's tools on top of the README list: checkout and the VST3 SDK
# fetch need git, the build cache ccache, the build-tree hand-off zstd, the
# Codecov uploader curl and gnupg. No build dependency belongs here: the
# Build workflow builds on this variant to prove the README list complete.
readonly BASE_PACKAGES=(ca-certificates git ccache zstd curl gnupg)

# The optional features (JACK, libsamplerate, libsndfile, libmpg123, X11 for
# the VST3 editors), the .deb and AppImage tools, and ffmpeg for the
# tools/listening/warp tests.
readonly FULL_PACKAGES=(
  libjack-jackd2-dev libsamplerate0-dev libsndfile1-dev libmpg123-dev libx11-dev
  dpkg-dev desktop-file-utils file qt6-wayland ffmpeg
)

inputs_hash() {
  local name
  for name in "${INPUTS[@]}"; do
    sha256sum <"$here/$name"
  done | sha256sum | cut -c1-16
}

if [[ ${1:-} == --hash ]]; then
  inputs_hash
  exit 0
fi

variant=${1:?usage: ci-image-deps.sh base|full|--hash}
case $variant in
  base) packages=("${BASE_PACKAGES[@]}") ;;
  full) packages=("${BASE_PACKAGES[@]}" "${FULL_PACKAGES[@]}") ;;
  *) echo "unknown variant '$variant'; use base or full" >&2; exit 2 ;;
esac

hash=$(inputs_hash)
read -r have_variant have_hash 2>/dev/null <"$STAMP" || true
if [[ ${have_hash:-} == "$hash" && (${have_variant:-} == "$variant" || ${have_variant:-} == full) ]]; then
  echo "The container holds the $variant packages for inputs $hash."
  exit 0
fi

echo "Installing the $variant packages for inputs $hash (container: ${have_variant:-none} ${have_hash:-none})."
"$here/install-deps.sh" "${packages[@]}"
# The workspace belongs to the runner user, not the container's root.
git config --system --add safe.directory '*'
mkdir -p "$(dirname "$STAMP")"
echo "$variant $hash" >"$STAMP"
