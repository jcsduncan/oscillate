#!/usr/bin/env bash
set -euo pipefail

app=$1
fixture_bundle=$2
workspace=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-vst3-instrument-ui.XXXXXX")
trap 'rm -rf -- "$workspace"' EXIT
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
export XDG_CONFIG_HOME="$workspace/config"

mkdir -p "$workspace/plugins"
cp -a "$fixture_bundle" "$workspace/plugins/TimedInstrument.vst3"
inserted=$("$app" --create "$workspace/project" --smoke \
  --smoke-vst3-instrument "$workspace/plugins/TimedInstrument.vst3")
[[ $inserted == *"verified insert, undo, redo and save"* ]]
reopened=$("$app" --open "$workspace/project" --smoke --smoke-verify-vst3-instrument)
[[ $reopened == *"reopened and played project MIDI"* ]]
