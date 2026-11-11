#!/usr/bin/env bash
# Installs the README "Build and run" packages (keep the two lists identical)
# plus any CI-only extras passed as arguments. Runs as root in an Ubuntu 26.04
# container. Usage: scripts/ci/install-deps.sh [extra-package...]
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
  cmake ninja-build g++ pkgconf libpipewire-0.3-dev libasound2-dev libdbus-1-dev \
  libssl-dev nlohmann-json3-dev qt6-base-dev qt6-declarative-dev \
  python3 python3-venv \
  "$@"
