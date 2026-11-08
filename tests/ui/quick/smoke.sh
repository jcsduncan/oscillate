#!/usr/bin/env bash
set -Eeuo pipefail
trap 'status=$?; printf "Quick smoke failed at line %s (exit %s): %s\n" "$LINENO" "$status" "$BASH_COMMAND" >&2' ERR

app=$1
codec_enabled=${2:-OFF}
resampler_enabled=${3:-OFF}
source_resampler_enabled=${4:-OFF}
fixture=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-quick.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
export XDG_CONFIG_HOME="$fixture/config"
export XDG_CACHE_HOME="$fixture/xdg-cache"

welcome=$("$app" --smoke --smoke-home "$fixture")
[[ $welcome == *"Qt Quick welcome ready; projects: 0"* ]]

# Projects created from the welcome screen start with the A and B returns.
created_from_welcome=$("$app" --smoke --smoke-create-from-welcome "$fixture")
[[ $created_from_welcome == *"revision 2, 2 tracks; clip delegates: 0"* ]]

[[ -f "$fixture/.oscillate/projects/Quick Start/state/CURRENT" ]]
opened_from_welcome=$("$app" --smoke --smoke-open-from-welcome "$fixture/.oscillate/projects/Quick Start")
[[ $opened_from_welcome == *"revision 2, 2 tracks; clip delegates: 0"* ]]

if duplicate=$("$app" --smoke --smoke-create-from-welcome "$fixture" 2>&1); then
  echo "duplicate project creation unexpectedly succeeded" >&2
  exit 1
fi
[[ $duplicate == *"already exists"* ]]

if missing=$("$app" --smoke --smoke-open-from-welcome "$fixture/missing" 2>&1); then
  echo "opening a missing project unexpectedly succeeded" >&2
  exit 1
fi
[[ $missing == *"not in the project list"* ]]

welcome_with_projects=$("$app" --smoke --smoke-home "$fixture")
[[ $welcome_with_projects == *"Qt Quick welcome ready; projects: 1"* ]]

export XDG_CONFIG_HOME="$fixture/other-config"
welcome_without_recent=$("$app" --smoke --smoke-home "$fixture")
[[ $welcome_without_recent == *"Qt Quick welcome ready; projects: 1"* ]]
opened_from_library=$("$app" --smoke --smoke-home "$fixture" --smoke-open-from-welcome "$fixture/.oscillate/projects/Quick Start")
[[ $opened_from_library == *"revision 2, 2 tracks; clip delegates: 0"* ]]

created=$("$app" --create "$fixture/project" --smoke)
[[ $created == *"revision 4, 1 tracks; clip delegates: 1"* ]]

reopened=$("$app" --open "$fixture/project" --smoke)
[[ $reopened == *"revision 4, 1 tracks; clip delegates: 1"* ]]

automation=$("$app" --create "$fixture/automation" --smoke --smoke-automation)
[[ $automation == *"Qt Quick arrangement automation added, dragged, deleted, drawn and bent on the clips"* ]]

send_automation=$("$app" --create "$fixture/send-automation" --smoke --smoke-send-automation)
[[ $send_automation == *"Qt Quick send gain automation edited, undone, redone and saved"* ]]
send_automation_reopened=$("$app" --open "$fixture/send-automation" --smoke --smoke-verify-send-automation)
[[ $send_automation_reopened == *"Qt Quick send gain automation reopened"* ]]

python3 - "$fixture/routing.wav" <<'PY'
from pathlib import Path
import struct
import sys

frames = struct.pack('<h', 8192) * 4096
payload = (b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 48000, 96000, 2, 16)
           + b'data' + struct.pack('<I', len(frames)) + frames)
Path(sys.argv[1]).write_bytes(b'RIFF' + struct.pack('<I', len(payload) + 4) + b'WAVE' + payload)
PY
# Each routing step starts from a fresh profile, so neither relies on
# layout preferences an earlier scenario saved.
export XDG_CONFIG_HOME="$fixture/routing-config"
routing=$("$app" --create "$fixture/routing" --smoke --smoke-routing --smoke-routing-source "$fixture/routing.wav")
[[ $routing == *"Qt Quick routed audio played, edited, stopped and reopened"* ]]
export XDG_CONFIG_HOME="$fixture/routing-reopen-config"
routing_reopened=$("$app" --open "$fixture/routing" --smoke --smoke-verify-routing)
[[ $routing_reopened == *"Qt Quick routing reopened"* ]]

if [[ $source_resampler_enabled == ON ]]; then
  python3 - "$fixture/rate-audio.wav" <<'PY'
from pathlib import Path
import struct
import sys

frames = struct.pack('<h', 8192) * 4410
payload = (b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 44100, 88200, 2, 16)
           + b'data' + struct.pack('<I', len(frames)) + frames)
Path(sys.argv[1]).write_bytes(b'RIFF' + struct.pack('<I', len(payload) + 4) + b'WAVE' + payload)
PY
  rate_audio=$("$app" --create "$fixture/rate-audio" --smoke --smoke-rate-audio \
    --smoke-routing-source "$fixture/rate-audio.wav")
  [[ $rate_audio == *"Qt Quick source-rate audio played without bus routing"* ]]
fi

midi_created=$("$app" --create "$fixture/midi" --smoke --smoke-midi-workflow)
[[ $midi_created == *"Qt Quick MIDI workflow saved"* ]]
[[ $midi_created == *"1 tracks; clip delegates: 2"* ]]
midi_reopened=$("$app" --open "$fixture/midi" --smoke --smoke-verify-midi)
[[ $midi_reopened == *"Qt Quick MIDI workflow reopened"* ]]
[[ $midi_reopened == *"1 tracks; clip delegates: 2"* ]]

for format in 0 1 2; do
  rendered=$("$app" --open "$fixture/midi" --smoke --smoke-export "$fixture/master-$format.wav" "$format")
  [[ $rendered == *"Qt Quick WAV master exported"* ]]
done
if [[ $resampler_enabled == ON ]]; then
  converted=$("$app" --open "$fixture/midi" --smoke --smoke-export "$fixture/master-44100.wav" 0 --smoke-output-rate 1)
  [[ $converted == *"Qt Quick WAV master exported"* ]]
  python3 - "$fixture/master-44100.wav" <<'PY'
from pathlib import Path
import struct
import sys
data = Path(sys.argv[1]).read_bytes()
assert struct.unpack_from('<I', data, 24)[0] == 44100
PY
fi
python3 - "$fixture" <<'PY'
from pathlib import Path
import struct
import sys

root = Path(sys.argv[1])
for index, (encoding, bits) in enumerate(((3, 32), (1, 24), (1, 16))):
    data = (root / f"master-{index}.wav").read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    assert struct.unpack_from("<I", data, 4)[0] + 8 == len(data)
    assert data[12:16] == b"fmt "
    assert struct.unpack_from("<H", data, 20)[0] == encoding
    assert struct.unpack_from("<I", data, 24)[0] == 48000
    assert struct.unpack_from("<H", data, 34)[0] == bits
    offset = 12
    while offset + 8 <= len(data):
        kind = data[offset:offset + 4]
        length = struct.unpack_from("<I", data, offset + 4)[0]
        if kind == b"data":
            samples = data[offset + 8:offset + 8 + length]
            assert len(samples) == length and length > 0
            assert any(samples), "rendered starter-sound note was silent"
            break
        offset += 8 + length + length % 2
    else:
        raise AssertionError("missing WAV data chunk")
PY

if [[ $codec_enabled == ON ]]; then
  flac_render=$("$app" --open "$fixture/midi" --smoke --smoke-export "$fixture/master.flac" 3)
  [[ $flac_render == *"Qt Quick FLAC master exported"* ]]
  mp3_render=$("$app" --open "$fixture/midi" --smoke --smoke-export "$fixture/master.mp3" 5)
  [[ $mp3_render == *"Qt Quick MP3 master exported"* ]]
  [[ $(head -c 4 "$fixture/master.flac") == fLaC ]]
  [[ -s "$fixture/master.mp3" ]]
fi

# A folder already holding the export's name refuses the publish rename for
# every user, root included (CI containers run as root, which ignores a
# read-only folder's permissions). The failed export leaves it untouched.
mkdir -p "$fixture/occupied/master.wav"
cp "$fixture/master-1.wav" "$fixture/occupied/master.wav/kept.wav"
if failed=$("$app" --open "$fixture/midi" --smoke --smoke-export-fail "$fixture/occupied/master.wav" 1 2>&1); then
  [[ $failed == *"Qt Quick WAV master failure preserved"* ]]
else
  echo "$failed" >&2
  exit 1
fi
[[ -d "$fixture/occupied/master.wav" ]]
cmp "$fixture/master-1.wav" "$fixture/occupied/master.wav/kept.wav"
[[ $(find "$fixture/occupied" -mindepth 1 | wc -l) -eq 2 ]]

closing=$("$app" --open "$fixture/midi" --smoke --smoke-export-close "$fixture/closing.wav" 0)
[[ $closing == *"Qt Quick WAV master finished before close"* ]]
[[ -s "$fixture/closing.wav" ]]

transport=$("$app" --create "$fixture/transport" --smoke --smoke-transport)
[[ $transport == *"Qt Quick transport played and stopped"* ]]
[[ ! -e "$fixture/transport/unexpected-playback-master.wav" ]]

output_failure=$("$app" --create "$fixture/transport-failure" --smoke --smoke-transport-fail)
[[ $output_failure == *"Qt Quick transport output failure reported"* ]]

audio_device=$("$app" --create "$fixture/audio-device" --smoke --smoke-audio-device)
[[ $audio_device == *"Qt Quick audio output selection and recovery ready"* ]]
