#!/usr/bin/env bash
set -Eeuo pipefail
trap 'status=$?; printf "Quick waveform smoke failed at line %s (exit %s): %s\n" "$LINENO" "$status" "$BASH_COMMAND" >&2' ERR

app=$1
resampler_enabled=${2:-OFF}
fixture=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-quick-waveform.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
export XDG_CONFIG_HOME="$fixture/config"
export XDG_CACHE_HOME="$fixture/xdg-cache"

python3 - "$fixture/waveform.wav" <<'PY'
from pathlib import Path
import struct
import sys

frames = b''.join(struct.pack('<hh', 0, 0) for _ in range(48000))
frames += b''.join(struct.pack('<hh', 16384, -16384) for _ in range(48000))
payload = (b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 48000, 192000, 4, 16)
           + b'data' + struct.pack('<I', len(frames)) + frames)
Path(sys.argv[1]).write_bytes(b'RIFF' + struct.pack('<I', len(payload) + 4) + b'WAVE' + payload)
PY
"$app" --smoke --smoke-create-from-welcome "$fixture/waveform-home" >/dev/null
waveform_root="$fixture/waveform-home/.oscillate/projects/Quick Start"
"$app" --open "$waveform_root" --smoke --smoke-import "$fixture/waveform.wav" >/dev/null
waveform_ready=$("$app" --open "$waveform_root" --smoke --smoke-waveform)
[[ $waveform_ready == *"Qt Quick waveform analysis and source trim projected"* ]]
if [[ $resampler_enabled == ON ]]; then
  stems_ready=$("$app" --open "$waveform_root" --smoke --smoke-stems --smoke-output-rate 1)
else
  stems_ready=$("$app" --open "$waveform_root" --smoke --smoke-stems)
fi
[[ $stems_ready == *"Qt Quick WAV stem bundle exported"* ]]
if [[ $resampler_enabled == ON ]]; then
  # The newest stem bundle: the waveform pass's trim is saved when that
  # session closes.
  python3 - "$waveform_root" <<'PY'
from pathlib import Path
import struct
import sys
bundles = sorted(Path(sys.argv[1]).glob('stems-rev-*'),
                 key=lambda path: int(path.name.rsplit('-', 1)[1]))
assert bundles
stems = list(bundles[-1].glob('*.wav'))
assert stems
for stem in stems:
    assert struct.unpack_from('<I', stem.read_bytes(), 24)[0] == 44100
PY
fi
