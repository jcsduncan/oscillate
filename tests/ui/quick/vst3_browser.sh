#!/usr/bin/env bash
set -euo pipefail

app=$1
scanner_enabled=$2
fixture_bundle=${3:-}
workspace=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-vst3-browser.XXXXXX")
trap 'rm -rf -- "$workspace"' EXIT
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
export XDG_CONFIG_HOME="$workspace/config"

if [[ $scanner_enabled == OFF ]]; then
  output=$("$app" --create "$workspace/project" --smoke --smoke-vst3-browser-disabled)
  [[ $output == *"unavailable build refused insertion"* ]]
  exit 0
fi

mkdir -p "$workspace/plugins"
cp -a "$fixture_bundle" "$workspace/plugins/LoadedGain.vst3"
python3 - "$workspace/audio.wav" <<'PY'
from pathlib import Path
import struct
import sys
frames = struct.pack('<h', 8192) * 4096
payload = (b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 48000, 96000, 2, 16)
           + b'data' + struct.pack('<I', len(frames)) + frames)
Path(sys.argv[1]).write_bytes(b'RIFF' + struct.pack('<I', len(payload) + 4) + b'WAVE' + payload)
PY
inserted=$("$app" --create "$workspace/project" --smoke \
  --smoke-vst3-browser "$workspace/plugins/LoadedGain.vst3" \
  --smoke-routing-source "$workspace/audio.wav")
[[ $inserted == *"verified insert, undo, redo and save"* ]]
reopened=$("$app" --open "$workspace/project" --smoke --smoke-verify-vst3-browser)
[[ $reopened == *"insert reopened"* ]]
two_effects=$("$app" --create "$workspace/two-effects-project" --smoke \
  --smoke-vst3-browser "$workspace/plugins/LoadedGain.vst3" \
  --smoke-vst3-two-effects --smoke-routing-source "$workspace/audio.wav")
[[ $two_effects == *"serial effects and a third on another track"* ]]

mkdir -p "$workspace/no-helper"
cp "$app" "$workspace/no-helper/oscillate"
if missing_helper=$("$workspace/no-helper/oscillate" \
    --create "$workspace/missing-helper-project" --smoke \
    --smoke-vst3-browser "$workspace/plugins/LoadedGain.vst3" \
    --smoke-routing-source "$workspace/audio.wav" 2>&1); then
  echo "VST3 browser unexpectedly scanned without its packaged helper" >&2
  exit 1
fi
[[ $missing_helper == *"worker scan failed"* ]]
