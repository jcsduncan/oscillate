# Minimal Sub Pump demo

This opt-in tool writes an editable eight-bar minimal project at 124 BPM in A minor, then renders it to a 32-bit float WAV. It uses only built-in devices:

- **Kick:** Anvil's Minimal Tick (ADR-0060), tuned so its tail sits on A1 (55 Hz).
- **Sub:** Torque's Dub Sub, playing a rolling line between the kicks, about 5 dB under the kick below 80 Hz.
- **Bass:** Torque's Moog Bass, playing the same line an octave up, so it carries on small speakers.
- **Returns:** the two every new project starts with, "A · Reverb" and "B · Delay", with their sends at zero.
- **Pump:** one linked Pump (ADR-0058) on both basses, keyed from the kick with "duck tracks to". Under each kick it ducks the basses by 25 dB or more, and they return in time for each offbeat note.

The tool builds the song through the project session and renders it through the offline master, the same paths the DAW uses. It is excluded from the ordinary build.

From the repository root, with the Quick build configured:

```sh
scripts/build.sh build/quick --target oscillate_minimal_demo
build/quick/tools/minimal_demo/oscillate_minimal_demo \
  "$HOME/.oscillate/projects/Minimal Sub Pump" \
  "$HOME/Music/oscillate/Minimal Sub Pump.wav"
```

The tool refuses to replace an existing project or WAV. To inspect or edit the song, open it from the DAW's project list.
