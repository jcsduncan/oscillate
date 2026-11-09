# ADR-0104 — A neutral native control layer, with project binding above native_dsp and plugins

**Status:** Accepted; implemented. **Date:** 8 November 2026.
**Builds on:** [ADR-0027](0027-prism-native-synth.md), the native synth platform; [ADR-0040](0040-native-effect-platform.md), the native effect platform; [ADR-0059](0059-play-and-sparkle.md), note effects.

## Context

Synths, effects and note effects are all a versioned parameter table plus a kernel. The parameter table, the control half that glides panel values and applies automation lanes, the telemetry buffer, the musical position a kernel sees at a control tick and the compiled-document base class were written for the synths and lived in `native_dsp::synth`. The effect platform used them from there (`synth::ParameterControl`, `synth::TelemetryBuffer`, `synth::SynthTransport`), and the note effects took their parameter tables and lanes from there too, so a new effect depended on synth headers and synth names. The meter's bar grid, which only the hosts that compile a project snapshot need, lived beside the tempo-sync helpers and pulled the timeline's meter map into every engine.

## Decision

1. **One neutral layer.** `oscillate_native_control` (`src/native_dsp`, namespace `oscillate::native_dsp`) holds what every table-driven engine is built on:
   - `parameters.hpp`: `ParameterDescriptor`, `EngineDescriptor`, `Preset`, `NamedValue` and the value helpers (sanitise, normalise, resolve, sparse and preset values);
   - `parameter_control.hpp`: `ParameterControl`, `ParameterLane` and the control-tick interval;
   - `telemetry.hpp`: `TelemetryBuffer`;
   - `transport.hpp`: `Transport`, the musical position at a control tick, and the tempo-synced divisions;
   - `document_state.hpp`: `DocumentState`, the base of every compiled engine document.

   It depends only on `foundation`. The synth engines and the effect platform link it and take parameter control, telemetry, the transport and document state from its neutral headers. The note effects link it for their parameter tables and lanes only: they time their steps with their own `NoteClock`, publish no telemetry and have no document.
2. **Names say what they are.** The moved types lose their synth prefixes: `SynthTransport` is `Transport`, `SynthPreset` is `Preset` and `SynthDocumentState` is `DocumentState`.
3. **The bar grid belongs to the timeline.** `timeline::BarGrid` turns a meter map into the bar lines the callback reads. Hosts that compile a snapshot (the synth player, the native effect stage) build it; kernels only see the resulting `Transport`.

4. **Project binding sits above `native_dsp`.** The synths' project binding (`native_inserts/instrument_binding.hpp`) and the `NativeSynthPlayer` (`native_inserts/synth_player.hpp`), which compile automation lanes, the tempo map and the meter from a snapshot, live in `native_inserts` beside the effects' binding. `native_dsp` links neither `automation_project` nor `timeline`. Note effects count in project ticks through a `FrameTicks` source the host supplies (`native_inserts::TempoTicks`, the tempo map's exact conversion), so no native engine evaluates a tempo or meter map; engines still read the tick positions that project documents carry.
5. **Plugins take neutral inputs.** Binding a saved `project::Vst3PluginInstance` to a loaded host, and preparing a graph node from it, live in `application` (`application/vst3/project_binding.hpp`, `application/vst3/graph_node.hpp`). The plugins module's graph runtime takes the compiled tempo boundaries and a `Vst3TempoClock` for the musical position, and one `Vst3ParameterCurve` per automated parameter, so `plugins` links only `foundation` and the engine processor interface.

## Alternatives considered

- **Keep the types in `synth` and alias them into `effects`.** An effect would still include synth headers, and the aliases would hide where the types live.
- **A new module for the native binding.** `native_inserts` already binds native effects to the project for every chain builder; the instruments' binding belongs beside it.
- **Move the note effects above `native_dsp`.** They are kernels like the synths; only their clock needed the tempo map.
- **Put the layer in `oscillate_native_synth`.** That library holds the synths' DSP building blocks; effects that use no synth DSP would still link it for the parameter table.

## Consequences

- No effect or note effect includes a synth header for these types. An effect includes the neutral headers and its own DSP; a note effect includes the parameter-table and lane headers.
- The synth DSP building blocks (filters, oscillators, oversamplers) stay in `oscillate_native_synth`; effects that use them still include those headers.
- A tick lookup in a note effect is a virtual call into the host's tempo map; a VST3 segment's position and each automation lane's block are one virtual call each. All stay callback safe.
- The HLD (§§3, 8, 8.1, 8.2) describes the layers.
