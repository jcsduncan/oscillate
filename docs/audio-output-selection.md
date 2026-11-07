# Audio output selection slice

The Qt Quick workspace lists PipeWire `Audio/Sink` nodes by their object serial and
description. **System default** leaves target selection to PipeWire. A named
output passes its serial to the existing playback output boundary; the project
does not store the choice. The application saves it in per-user `QSettings`.

The output button in the workspace header opens an accessible device list and
Refresh action. Discovery runs on a worker through a bounded PipeWire registry
snapshot (two-second deadline); it never runs on the audio callback or blocks
startup. Closing during discovery can wait for that worker's bounded completion.
Changing output is disabled during playback or another active project operation.

If the selected sink disappears, playback stops, the choice remains visible as
missing, and refresh does not silently reroute it. The user may select another
sink or System default, then press Play. Restart follows the existing playback
preparation and transport contracts. PipeWire node IDs are not persisted or
offered as selection identities.

The deterministic Quick smoke injects two sinks and a device disconnect. It
checks selection persistence, the serial handed to playback, selection locking,
visible missing-device state and the default recovery choice. The audio I/O
contract test covers malformed registry values and source filtering. These
tests do not qualify a physical output device or measure underruns. The
optional live PipeWire tests remain a separate local qualification step.
