# Afterglow Circuit demo generator

This opt-in tool writes an editable, synth-only 120 BPM progressive melodic
techno project into a new folder and renders it to an offline 32-bit float
WAV. No sample or plugin download is
needed. The song uses Pulse, Torque, Ember, Nova, Halo and Drift; the effects
are native devices. The tool is excluded from the ordinary build.

From the repository root, with the Quick build configured:

```sh
mkdir -p build/quick/scratch
scripts/build.sh build/quick --target oscillate_demo_song
build/quick/tools/demo_song/oscillate_demo_song \
  "$HOME/.oscillate/projects/Afterglow Circuit Copy" \
  "$PWD/build/quick/scratch/Afterglow Circuit full tail.wav"
```

The tool refuses to replace an existing project or WAV. Open the project from
the DAW's project list to inspect or edit the arrangement. The WAV render uses
the app's export path, which currently reserves about 56 seconds of potential
effect tail even though this song fades to silence much earlier. For a listening
copy with ten seconds after the final bar, trim the rendered WAV without
changing its sample format:

```sh
mkdir -p "$HOME/.oscillate/exports"
ffmpeg -i "$PWD/build/quick/scratch/Afterglow Circuit full tail.wav" \
  -t 202 -c:a pcm_f32le "$HOME/.oscillate/exports/Afterglow Circuit Copy.wav"
```
