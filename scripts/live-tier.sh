#!/usr/bin/env bash
# The live test tier against a running PipeWire (and JACK, when jackd is
# installed): the audio_io live smokes, the 32-track streaming rehearsal on a
# disposable null sink, and both reference projects played live on another
# disposable sink. The PipeWire smoke plays silence to the default sink and
# the first listed one; everything audible goes to disposable sinks, and no
# device setting changes. Results and logs go to OUT_DIR.
#
# Usage: scripts/live-tier.sh BUILD_DIR OUT_DIR
# BUILD_DIR is a configured main build (for example build/release); this
# script turns on the live tests and the qualification programs, builds just
# their targets through scripts/build.sh and runs them.
set -euo pipefail

build_dir=${1:?usage: scripts/live-tier.sh BUILD_DIR OUT_DIR}
out_dir=${2:?usage: scripts/live-tier.sh BUILD_DIR OUT_DIR}
reference_seconds=${OSCILLATE_LIVE_REFERENCE_SECONDS:-10}
sink=oscillate-live-tier-$$
jack_server=oscillate_smoke_live_tier_$$

cd "$(dirname "$0")/.."
mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)
for tool in pw-cli pw-dump pw-record; do
  command -v "$tool" >/dev/null || { echo "live-tier: $tool is required" >&2; exit 1; }
done

cmake -DOSCILLATE_AUDIO_IO_LIVE_TESTS=ON -DOSCILLATE_BUILD_QUALIFICATION=ON \
  -DOSCILLATE_BUILD_VST3_SCANNER=ON "$build_dir" >"$out_dir/configure.log"
targets=(oscillate_audio_io_live_smoke oscillate_streaming_32track_rehearsal oscillate_reference_project)
if grep -q '^JACK_FOUND:INTERNAL=1' "$build_dir/CMakeCache.txt"; then
  targets+=(oscillate_jack_live_smoke)
fi
scripts/build.sh "$build_dir" --target "${targets[@]}"

status=0
record() { # NAME COMMAND...: runs one check, logs it and keeps going.
  local name=$1
  shift
  if "$@" >"$out_dir/$name.log" 2>&1; then
    echo "pass $name" | tee -a "$out_dir/summary.txt"
  else
    echo "FAIL $name" | tee -a "$out_dir/summary.txt"
    tail -n 20 "$out_dir/$name.log" >&2
    status=1
  fi
}
: >"$out_dir/summary.txt"

record pipewire-smoke ctest --test-dir "$build_dir" -R '^audio_io_pipewire_live$' --output-on-failure

if [[ " ${targets[*]} " == *" oscillate_jack_live_smoke "* ]] && command -v jackd >/dev/null; then
  jackd --no-realtime -n "$jack_server" -d dummy -r 48000 -p 64 >"$out_dir/jackd.log" 2>&1 &
  jackd_pid=$!
  sleep 2
  record jack-smoke env JACK_DEFAULT_SERVER="$jack_server" \
    ctest --test-dir "$build_dir" -R '^audio_io_jack_live' --output-on-failure
  kill "$jackd_pid" && wait "$jackd_pid" || true
else
  echo "skip jack-smoke (no JACK build or no jackd)" | tee -a "$out_dir/summary.txt"
fi

record streaming-32track python3 -I tests/qualification/qualify_live_streaming_32track.py \
  --executable "$build_dir/tests/qualification/oscillate_streaming_32track_rehearsal" \
  --vst3-insert --vst3-instrument

# A disposable sink for the reference projects: it lives as long as this
# pw-cli session, which ends with the script.
coproc PWCLI { pw-cli >/dev/null 2>&1; }
trap 'kill "$PWCLI_PID" 2>/dev/null || true' EXIT
echo "create-node adapter { factory.name=support.null-audio-sink node.name=$sink \
media.class=Audio/Sink audio.position=[ FL FR ] node.always-process=true \
priority.session=-10000 }" >&"${PWCLI[1]}"
sleep 1
record reference-live python3 -I tests/qualification/qualify_reference_projects.py \
  --executable "$build_dir/tests/qualification/oscillate_reference_project" \
  --output "$out_dir/reference" --seconds "$reference_seconds" --target-node "$sink"

exit "$status"
