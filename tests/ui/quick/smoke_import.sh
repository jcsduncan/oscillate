#!/usr/bin/env bash
set -Eeuo pipefail
trap 'status=$?; printf "Quick import smoke failed at line %s (exit %s): %s\n" "$LINENO" "$status" "$BASH_COMMAND" >&2' ERR

app=$1
codec_enabled=${2:-OFF}
fixture=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-quick-import.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
export XDG_CONFIG_HOME="$fixture/config"
export XDG_CACHE_HOME="$fixture/xdg-cache"

"$app" --smoke --smoke-create-from-welcome "$fixture" >/dev/null

python3 - "$fixture" <<'PY'
from pathlib import Path
import struct
import sys

root = Path(sys.argv[1])
frames = b''.join(struct.pack('<hh', 8000 if i % 2 else -8000, 4000) for i in range(480))
payload = (b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 48000, 192000, 4, 16)
           + b'data' + struct.pack('<I', len(frames)) + frames)
(root / 'import.wav').write_bytes(b'RIFF' + struct.pack('<I', len(payload) + 4) + b'WAVE' + payload)
(root / 'import-stale.wav').write_bytes((root / 'import.wav').read_bytes())
(root / 'invalid.wav').write_bytes(b'not a wave')
PY

import_project="$fixture/.oscillate/projects/Quick Start"
imported=$("$app" --open "$import_project" --smoke --smoke-import "$fixture/import.wav")
[[ $imported == *"Qt Quick audio imported"* ]]
rm "$fixture/import.wav"
reopened_import=$("$app" --open "$import_project" --smoke --smoke-verify-import)
[[ $reopened_import == *"Qt Quick audio import reopened"* ]]

python3 - "$fixture/tempo.wav" <<'PY'
from pathlib import Path
import struct
import sys

rate = 48000
samples = [0] * (rate // 4 + rate // 2 * 17)
for beat in range(16):
    at = rate // 4 + beat * rate // 2
    samples[at:at + 120] = [20000] * 120
pcm = struct.pack('<' + 'h' * len(samples), *samples)
payload = (b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16)
           + b'data' + struct.pack('<I', len(pcm)) + pcm)
Path(sys.argv[1]).write_bytes(b'RIFF' + struct.pack('<I', len(payload) + 4) + b'WAVE' + payload)
PY
"$app" --smoke --smoke-create-from-welcome "$fixture/tempo-home" >/dev/null
tempo_root="$fixture/tempo-home/.oscillate/projects/Quick Start"
"$app" --open "$tempo_root" --smoke --smoke-import "$fixture/tempo.wav" >/dev/null
# The clip strip analyses a new clip's source straight after import, so the
# import run may already have cached its tempo before it quit. Start the
# generated step from an empty shared cache so it really analyses.
find "$XDG_CACHE_HOME/oscillate/source-analysis" -type f -name '*.cache' -delete 2>/dev/null || true
tempo_generated=$("$app" --open "$tempo_root" --smoke --smoke-tempo generated)
[[ $tempo_generated == *"Qt Quick tempo analysis generated "* ]]
[[ $tempo_generated != *"Qt Quick project view:"* ]]
tempo_key=$(sed -nE 's/^Qt Quick tempo analysis generated ([[:xdigit:]]{64})$/\1/p' <<< "$tempo_generated")
[[ ${#tempo_key} -eq 64 ]]
shared_tempo_cache="$XDG_CACHE_HOME/oscillate/source-analysis/v1/$tempo_key.cache"
if [[ ! -f "$shared_tempo_cache" ]]; then
  printf 'Shared tempo cache missing for %s; result: %s\n' "$tempo_key" "$tempo_generated" >&2
  find "$XDG_CACHE_HOME/oscillate/source-analysis/v1" -maxdepth 1 -type f -printf '%f %s bytes\n' >&2 || true
  exit 1
fi
tempo_cached=$("$app" --open "$tempo_root" --smoke --smoke-tempo cached)
[[ $tempo_cached == *"Qt Quick tempo analysis cached $tempo_key"* ]]
printf 'corrupt' > "$shared_tempo_cache"
tempo_rebuilt=$("$app" --open "$tempo_root" --smoke --smoke-tempo rebuilt)
[[ $tempo_rebuilt == *"Qt Quick tempo analysis rebuilt $tempo_key"* ]]
tempo_dropped=$("$app" --open "$tempo_root" --smoke --smoke-tempo-drop "$fixture/tempo.wav")
[[ $tempo_dropped == *"Qt Quick dropped track follows with its downbeat on bar "* ]]
if [[ $codec_enabled == ON ]]; then
  encoded_home="$fixture/encoded-home"
  "$app" --smoke --smoke-create-from-welcome "$encoded_home" >/dev/null
  encoded_root="$encoded_home/.oscillate/projects/Quick Start"
  encoded_fixture="$(dirname "$0")/../../codec/assets/sine.flac"
  encoded_imported=$("$app" --open "$encoded_root" --smoke --smoke-import "$encoded_fixture")
  [[ $encoded_imported == *"Qt Quick audio imported"* ]]
  encoded_reopened=$("$app" --open "$encoded_root" --smoke --smoke-verify-import)
  [[ $encoded_reopened == *"Qt Quick audio import reopened"* ]]
  mp3_home="$fixture/mp3-home"
  "$app" --smoke --smoke-create-from-welcome "$mp3_home" >/dev/null
  mp3_root="$mp3_home/.oscillate/projects/Quick Start"
  mp3_fixture="$(dirname "$0")/../../codec/assets/sine.mp3"
  mp3_imported=$("$app" --open "$mp3_root" --smoke --smoke-import "$mp3_fixture")
  [[ $mp3_imported == *"Qt Quick audio imported"* ]]
  mp3_reopened=$("$app" --open "$mp3_root" --smoke --smoke-verify-import)
  [[ $mp3_reopened == *"Qt Quick audio import reopened"* ]]
fi
[[ $reopened_import == *"revision 3, 3 tracks; clip delegates: 1"* ]]

for case in failure stale close undo; do
  mkdir -p "$fixture/import-$case-home"
  "$app" --smoke --smoke-create-from-welcome "$fixture/import-$case-home" >/dev/null
done
failure_root="$fixture/import-failure-home/.oscillate/projects/Quick Start"
failed_import=$("$app" --open "$failure_root" --smoke --smoke-import-fail "$fixture/invalid.wav")
[[ $failed_import == *"Qt Quick WAV import failure preserved"* ]]
[[ $("$app" --open "$failure_root" --smoke) == *"revision 2, 2 tracks; clip delegates: 0"* ]]
[[ -z $(find "$failure_root/assets" -type f -print -quit) ]]

stale_root="$fixture/import-stale-home/.oscillate/projects/Quick Start"
stale_import=$("$app" --open "$stale_root" --smoke --smoke-import-stale "$fixture/import-stale.wav")
[[ $stale_import == *"Qt Quick stale WAV import rejected"* ]]
[[ -z $(find "$stale_root/assets" -type f -print -quit) ]]

close_root="$fixture/import-close-home/.oscillate/projects/Quick Start"
closed_import=$("$app" --open "$close_root" --smoke --smoke-import-close "$fixture/import-stale.wav")
[[ $closed_import == *"Qt Quick WAV import cancelled before close"* ]]
[[ $("$app" --open "$close_root" --smoke) == *"revision 2, 2 tracks; clip delegates: 0"* ]]
[[ -z $(find "$close_root/assets" -type f -print -quit) ]]

undo_root="$fixture/import-undo-home/.oscillate/projects/Quick Start"
undone_import=$("$app" --open "$undo_root" --smoke --smoke-import-undo "$fixture/import-stale.wav")
[[ $undone_import == *"Qt Quick WAV import undone"* ]]
[[ $("$app" --open "$undo_root" --smoke) == *"revision 4, 2 tracks; clip delegates: 0"* ]]

python3 - "$fixture/arrange.wav" <<'PY'
from pathlib import Path
import struct
import sys

frames = b''.join(struct.pack('<hh', 6000, -6000) for _ in range(96000))
payload = (b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 48000, 192000, 4, 16)
           + b'data' + struct.pack('<I', len(frames)) + frames)
Path(sys.argv[1]).write_bytes(b'RIFF' + struct.pack('<I', len(payload) + 4) + b'WAVE' + payload)
PY
"$app" --smoke --smoke-create-from-welcome "$fixture/arrange-home" >/dev/null
arrange_root="$fixture/arrange-home/.oscillate/projects/Quick Start"
"$app" --open "$arrange_root" --smoke --smoke-import "$fixture/arrange.wav" >/dev/null
arranged=$("$app" --open "$arrange_root" --smoke --smoke-audio-gestures)
[[ $arranged == *"Qt Quick audio gestures saved"* ]]
reopened_arrangement=$("$app" --open "$arrange_root" --smoke --smoke-verify-audio-gestures)
[[ $reopened_arrangement == *"Qt Quick audio gestures reopened"* ]]

for case in edit stale fail close stop; do
  "$app" --smoke --smoke-create-from-welcome "$fixture/live-$case-home" >/dev/null
  live_root="$fixture/live-$case-home/.oscillate/projects/Quick Start"
  "$app" --open "$live_root" --smoke --smoke-without-returns --smoke-import "$fixture/arrange.wav" >/dev/null
  result=$("$app" --open "$live_root" --smoke "--smoke-live-$case")
  case "$case" in
    edit) [[ $result == *"Qt Quick live audio edit applied"* ]] ;;
    stale) [[ $result == *"Qt Quick live audio stale preparation rejected"* ]] ;;
    fail) [[ $result == *"Qt Quick live audio failed re-prime preserved edit"* ]] ;;
    close) [[ $result == *"Qt Quick live audio close awaited handoff"* ]] ;;
    stop) [[ $result == *"Qt Quick live audio Stop awaited handoff"* ]] ;;
  esac
  if [[ $case != close ]]; then
    reopened_live=$("$app" --open "$live_root" --smoke --smoke-verify-live-edit)
    [[ $reopened_live == *"Qt Quick live audio edit reopened"* ]]
  fi
done
