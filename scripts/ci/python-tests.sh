#!/usr/bin/env bash
# Runs the hardware-free Python unittest suites. tools/listening/warp needs
# ffmpeg and ffprobe; tests/qualification only exercises the result parsers
# here, the live qualification runs themselves need real devices.
set -euo pipefail

cd "$(dirname "$0")/../.."
for suite in scripts/ci scripts/packaging tools/listening/warp tests/qualification; do
  echo "== $suite"
  python3 -I -m unittest discover -s "$suite" -p 'test_*.py'
done
