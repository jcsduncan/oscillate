# The CI container image ghcr.io/jcsduncan/oscillate-ci (CONTRIBUTING.md,
# "CI container image"), built from the repository root by
# .github/workflows/ci-image.yml. Target base: the README packages and the
# CI tools. Target full: base plus the optional features and packaging tools.
# The package lists live in scripts/ci/ci-image-deps.sh.
FROM ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c AS base

LABEL org.opencontainers.image.source=https://github.com/jcsduncan/oscillate
LABEL org.opencontainers.image.description="Oscillate CI: Ubuntu 26.04 with the build dependencies"
LABEL org.opencontainers.image.licenses=GPL-3.0-or-later

COPY scripts/ci/Dockerfile scripts/ci/install-deps.sh scripts/ci/ci-image-deps.sh /opt/oscillate-ci/

# The weekly rebuild takes the security updates published since the base
# image's digest.
RUN apt-get update -qq \
  && DEBIAN_FRONTEND=noninteractive apt-get upgrade -y -qq \
  && /opt/oscillate-ci/ci-image-deps.sh base \
  && rm -rf /var/lib/apt/lists/*

FROM base AS full
RUN /opt/oscillate-ci/ci-image-deps.sh full \
  && rm -rf /var/lib/apt/lists/*
