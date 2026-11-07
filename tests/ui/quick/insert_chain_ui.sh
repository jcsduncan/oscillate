#!/usr/bin/env bash
set -euo pipefail

app=$1
fixture=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-quick-chain.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
export XDG_CONFIG_HOME="$fixture/config"

python3 - "$fixture/source.wav" <<'PY'
import struct
import sys
import wave

with wave.open(sys.argv[1], 'wb') as output:
    output.setnchannels(1)
    output.setsampwidth(2)
    output.setframerate(48000)
    output.writeframes(struct.pack('<h', 8192) * 4)
PY

for scale in 1 2; do
  export QT_SCALE_FACTOR=$scale
  screenshot="$fixture/chain-${scale}.png"
  selected="$fixture/chain-${scale}.selected.png"
  detailed="$fixture/chain-${scale}.detailed.png"
  result=$("$app" --create "$fixture/project-${scale}" --smoke \
    --smoke-grid-source "$fixture/source.wav" --smoke-chain-ui --screenshot "$screenshot")
  [[ $result == *"Qt Quick selected chain and pinned master ready"* ]]
  python3 - "$screenshot" "$selected" "$detailed" "$scale" <<'PY'
import struct
import sys
from pathlib import Path

scale = int(sys.argv[4])
for path in sys.argv[1:4]:
    data = Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    assert struct.unpack('>II', data[16:24]) == (1920 * scale, 1080 * scale)
PY
done
