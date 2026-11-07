# ADR-0043 — One native insert path

**Status:** Accepted and implemented; the classic kinds become engines in ADR-0047. Superseded in part by [ADR-0090](0090-render-graph-builder.md): export reaches `native_inserts` through `render_graph`. **Date:** 20 October 2026.
**Builds on:**
- ADR-0023, the unified insert chain;
- ADR-0040, the native effect platform (its model, schema and Quick decisions stand);
- ADR-0027, the native synth live-edit pattern.

## Context

The 20 October review found the 13 ADR-0040 effects built and tested, but impossible to insert. Today each native processor kind is hand-switched separately in six chain builders:
- the live track chain and the live master chain (`routed_playback_control.cpp`);
- the legacy master graph (`graph_control.cpp`);
- the offline track and master chains (`offline_master.cpp`);
- the VST3 export drivers' tail and latency loops (`vst3_effect_offline.cpp`, `vst3_instrument_offline.cpp`);
- the real-time export tail (`realtime_export_run.cpp`).

There are about 270 type-specific references. Several of the switches fall through silently:
- the legacy chain projection labels unknown kinds "convolution";
- the legacy master graph ends in `std::get<NativeLimiter>`;
- automation preparation ends in `std::get<Vst3ParameterTarget>`.

Adding the ADR-0040 alternative as a seventh branch in every one of those places would make the problem worse.

## Decision

### 1. One factory, in its own library

`oscillate::native_inserts` (`src/native_inserts`) depends only on project, engine, timeline, native_dsp and native_effects. It owns:
- `prepare_native_stage(const project::EffectProcessor&, const StageContext&) -> Result<PreparedStage>`.
  - `PreparedStage` = `{runtime, kind, generation, latency_frames, export_tail_frames, transition_tail_frames, sidechain_capable}`.
  - Every native kind is prepared here, and only here.
- `guarded_frames_after_quiesce(const engine::ProcessorRuntime&)`: the one place that knows the concrete runtime types.
- `NativeEffectStage`: the `engine::SidechainProcessorRuntime` adapter around `NativeEffectRuntime`.
  - It works out `SynthTransport` from the tempo map.
  - It retains state across control overlays (`control_overlay_revision_independent`).
  - It owns an automation-plan mailbox (the `NativeSynthPlayer` pattern) and exposes `applied_revision` and telemetry.

Live playback, the offline export and the VST3 export drivers all call the factory. Application and export both link `native_inserts`, so the change doesn't deepen the application↔export inversion.

The legacy master graph (`graph_control.cpp`) keeps its fixed EQ/compressor/reverb order and rejects anything else explicitly, never with a throwing `std::get`.

### 2. The generic effect slot

ADR-0040's model stands:
- `NativeEffectPatch{engine, engine_version, parameters, generation}` is appended to both processor variants.
- Its parameters are sparse ids and values.
- The model validates structure only.
- Engines and values are resolved by the factory and by the session, as native instruments are.

**Schema 1.33** adds the effect kind `native_effect` and the automation target `native_effect_parameter`.

**Master chains.** Any number of patches may be added, in saved order. The existing one-of-each rule still applies to the five legacy kinds.

### 3. Sidechains stay sends

A detector is still a `SendRoute{purpose: sidechain, target_insert}` on the source track. Every edge, latency, detector-bus and solo-stem mechanism is keyed on that send.

- **Model:** a sidechain may target any enabled native effect patch.
- **Route plan:** checks the target through the receipt. `BoundNativeInsert` gains `sidechain_capable`, which comes from `EffectEngine::sidechain`.
- **Live chains:** a sidechained chain may use the live value path, since a value offer doesn't touch the detector wiring.

### 4. Live edits never rebuild

- **Value edits:** a patch value edit keeps the same slot, engine, version and latency. It is offered to the running runtime's mailbox and acknowledged in `observe()` through `applied_revision`, exactly like a native instrument.
- **Latency changes:** an edit that changes the engine's latency takes the rebuild path.
- **Automation:** effect automation is prepared once per revision by `automation_project`. It is offered to the stage's plan mailbox live, and built once per chain offline, so export renders the same lanes as live playback.

### 5. Automation target

`NativeEffectParameterTarget{slot, parameter_id}`:
- The lane is owned by the track whose chain holds the slot.
- Master-chain effect automation needs a master lane owner. That is a separate, later decision, and until it is made it is rejected explicitly.

### 6. Quick

- The insert chain projects `nativeKind: "effect"` with the engine id.
- The browser lists the effect registry.
- The device chain hosts each effect's designed panel through an effect adapter on the same seam as the synth panels (`values`, `submit`, `status`, `telemetry`), keyed by path and slot.

## Delivery

1. The `native_inserts` factory, with every builder moved onto it for the existing kinds. This changes no behaviour: all existing tests pass unchanged.
2. The model, codec 1.33, the adapter, routing receipts, and the live and offline chains for patches, with export-equals-live parity tests.
3. The live value path and effect automation.
4. Sidechain targets for patches (for Pocket, Scope and Lens).
5. Quick: the browser, the insert chain projection, the panels in the device chain, telemetry polling.

## As built

**Phase 1** (`24dd4d4a`).
- `oscillate::native_inserts` prepares every legacy kind.
- The live, legacy-graph, offline and both VST3 export builders use it, and so does the JACK export tail.
- The offline master is a saved-order stage list.

**Phase 2.**
- **Model:** `NativeEffectPatch` and `NativeEffectParameterTarget` are in the model, with structural validation.
- **Editing rules:**
  - A slot keeps its engine; changing to a different effect is remove plus add.
  - Removing a slot drops its lanes.
  - A sidechain may target a patch.
- **Codec 1.33:** `native_effect` and `native_effect_parameter`.
- **Bindings:** `effect_binding` resolves sparse patches against the registry.
- **`NativeEffectStage`:** the `SidechainProcessorRuntime` adapter, with tempo and meter transport and a slot automation-plan mailbox.
- **Route plan:**
  - The route plan accepts `NativeRouteKind::effect` receipts, carrying the engine's latency and `sidechain_capable`.
  - Built-in effects may repeat on the master.
- **Live and offline chains:**
  - Both prepare patches with the same automation, bounded live as the sampler's.
  - Offline treats a refused effect block as guarded and fails the render.
- **Session commands:**
  - `add_effect_engine`;
  - `native_effect_values` and `set_native_effect_values`, with gesture coalescing;
  - `set_effect_automation_point`, `set_effect_automation_value`, `remove_effect_automation_point` and `remove_effect_automation_lane`.
- **Measured:** `application_effect_export_parity` renders every one of the 13 engines on a synth track (with a two-point lane) and on the master. Offline export equals live routed playback at 64- and 128-frame blocks, with a largest difference of exactly 0.
- **Still restricted:** a built-in effect is not yet admitted into a mixed chain with a VST3 effect, because the mixed route allows one native and one VST3 stage of the legacy kinds.

**Phases 3 and 4.** Built-in effects now edit live, with no rebuild.

- **Panel value edits:**
  - offered to the running `NativeEffectStage` mailbox when the engine and latency are unchanged;
  - superseded in place;
  - acknowledged in `observe()`;
  - on tracks, sidechained tracks and the master.
- **Lane edits:**
  - an isolated lane edit on one effect slot builds a new automation plan off the callback and offers it to the stage;
  - the chain's saved slots are left untouched.
- **Sidechains:** Contour, Pocket, Lens and Scope take a sidechain send as their detector, live and offline.
- **Tests:**
  - `application_routed_effect_live` edits values, lanes, a keyed Pocket and a master Vista during playback. No edit holds the graph or allocates in the callback, and each is reported audible only after its stage acknowledges it.
  - The parity test adds a keyed case for every effect that reads a sidechain, and every one is exact.

## Consequences

- A new native effect needs no plumbing.
- The five classic kinds remained separate model types for file compatibility, prepared in one place. The project is greenfield, so ADR-0047 moves them onto engines one at a time.
- Master-chain effect automation is explicitly out of scope until a master lane owner exists.
