# ADR-0037 — Live synth telemetry for panels

**Status:** Accepted; implemented. **Date:** 19 October 2026.

## Context

Panels could only show settings, never what the engine was actually doing. That left several features unbuilt or faked:
- Torque's sequencer strip had no playhead.
- Pulse's gain-reduction meter was a placeholder.
- Drift's grain cloud was only a preview of the settings.
- Concert's string and damper view (ADR-0036) needs to see which strings are ringing.

`KernelStatus` (voice counts, deferral) was the only channel from the engine to the panel.

## Decision

- **The kernel.** A kernel may publish telemetry: `SynthKernel::telemetry(std::span<float>)` writes up to `max_telemetry_values` (2,048) floats after every block. Each engine documents its layout as named offsets in its public header.
  - **Torque:** running, step, key, sweep, cutoff, gate.
  - **Pulse:** gain reduction, then twelve pad levels.
  - **Drift:** a count, then one record per grain: key, seconds behind the head, semitones, reverse, phase, frozen.
- **The runtime** publishes each frame through a lock-free single-producer, single-consumer triple buffer:
  - the callback fills its back slot and swaps it with the middle;
  - the control side swaps the middle with its front slot when a newer frame is waiting.

  So the callback never blocks or allocates, and the reader always gets the newest whole frame, never a torn one. `NativeSynthRuntime::read_telemetry(std::vector<float>&)` is the control-side read.
- **The panel side:**
  - `NativeSynthPanelAdapter::telemetry` is the panel's read. `SessionNativeSynthAdapter` takes a `TelemetryProbe` hook, wired through `RoutedPlayback::synth_telemetry` to the selected track's runtime.
  - `NativeSynthDeviceController::telemetry()` decodes the frame into a `QVariantMap` per engine, which panels poll while visible.
- **Live versus preview.** Telemetry is display data only. It never feeds back into sound, so exports are unaffected. When nothing is playing, panels show their settings preview as before.

## Consequences

- **Tests:**
  - torn-frame and no-allocation stress tests, with a reader thread racing the callback;
  - Torque's playhead is frame-exact against the sequencer;
  - Pulse's pad flashes and gain reduction;
  - Drift's grains are real, sit behind the head, and carry the frozen flag;
  - the controller decodes every layout.
- **Panels.** Torque, Pulse and Drift panels show live state when it is available. Concert starts with this channel built in.
