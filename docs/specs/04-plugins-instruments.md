# FS-04 — Plugins, native effects and instruments

**Status:** Draft. **Dependencies:** processor API, project serialization, UI panel contract. **PRD:** §§13, 15–16.

## Functional contract

- **PI-01** Scan Linux VST3 plugins without blocking the UI or audio callback. Crashes, hangs and invalid metadata are recorded per plugin; repeated failures quarantine that plugin until user retry/rescan.
- **PI-02** Browser supports search, categories, tags, favourites, compatibility status and drag/drop insertion. Insert, reorder, bypass, remove, preset and chain copy/paste are undoable model commands.
- **PI-03** Effect and instrument plugins receive supported audio/MIDI buses and parameter events. The editor bridge supplies the VST3 Linux run loop for editor timers and file-descriptor handlers on the UI thread. X11 editor embedding and Wayland/XWayland or native Wayland support require compatibility spikes before release claims. If an editor cannot attach, a generic parameter view remains available and reports why. Editor open/close must never block the audio callback. Unsupported bus or precision capabilities are reported. Real-plugin editor compatibility remains an open PI-03 acceptance gate.
- **PI-04** State save/reopen restores plugin identity, parameter state, opaque state and chain location. A missing plugin stays as a placeholder with automation and routing references intact; relink is explicit.
- **PI-05** In-process hosting is the performance default. Optional isolated hosting is best-effort, with measurable added latency and a clear warning about limits. A plugin failure must never silently delete its project state.
- **PI-06** A fresh install includes a bundled sampler (Crate, [spec 09](09-crate-sampler.md)) and a small royalty-clear sample set. A user can create and edit notes in the basic piano roll with mouse actions and common keyboard shortcuts, hear them, save/reopen and export without installing a third-party plugin.
- **PI-06a** Selecting a MIDI clip opens a piano-roll tab in the resizable bottom dock while keeping the arrangement visible. The roll scrolls across all 128 MIDI pitches by mouse wheel or keyboard, offers a Find Notes action and 1/4, 1/8, 1/16, 1/32, 1/64 and 1/128 grids (one list, `application::midi_grid_steps_per_bar`, that the roll, the bridge and Quantise share). Clicking an empty grid cell adds a note with that grid duration. Clicking a note selects it; dragging moves it to the active grid, dragging its right edge changes its duration, and Delete or double click removes it. Existing notes retain their exact starts and lengths when the display grid changes. Each edit is undoable, reaches playback through the project revision, and survives save/reopen.
- **PI-06b** Dragging an occupied piano-roll cell moves that note to another step and pitch within the one-bar editor. The destination preview follows the pointer. A move preserves note identity, duration and velocity, commits one undo step, and updates playback and save/reopen state. An occupied destination or drop outside the editor rejects without changing the note; a drag ending in the original cell is a no-op.
- **PI-06c** A MIDI clip has a Loop switch, off for a new clip, in the piano-roll header and the clip's context menu. With Loop off, dragging the clip's right edge to a bar sets its length: the placement end and the pattern both move there, so lengthening gives the piano roll room for notes. Shortening keeps notes and pedal points after the new end in the clip, silent and shown greyed in the piano roll, and lengthening again restores them (Live parity); a note crossing the end is cut there. With Loop on, the right edge repeats the saved pattern without copying note records. Switching Loop sets the pattern to the clip's current length either way, so the whole clip repeats, or the repeats stop with the first pass's notes in place. The clip remains in its chosen timebase: musical placement ends at the target tick, and elapsed placement ends at the target bar's sample under the current tempo map, its length in ticks being that end's tick less its anchor tick. Each completed resize or loop switch is one undo step; an occupied same-track interval rejects atomically, and the result plays and reopens with the same note identities.
- **PI-06d** Quantise moves all note starts in the selected one-bar MIDI pattern to the nearest active grid position as one atomic undo step. It preserves durations, velocities and note identities. A collision at the same pitch and start rejects the entire operation with an actionable message. Notes near the pattern end stay within its bounds without truncation. Snap-to-key will use a durable project key/scale once that project setting is defined; an audio source's detected key is never silently treated as the project key.
- **PI-06e** A Pulse MIDI clip opens a drum-lane view with its twelve named pads at MIDI keys 36–47. Piano and Drums are switchable views of the same notes and edit commands; neither creates a second note format. Notes outside the pad map remain saved and playable, and the editor shows their count with an All Notes action that opens the piano view. Other built-in instruments and VST3 instruments open the piano view unless they provide a verified note map. Grid, quantise, drag, undo and save/reopen semantics are identical in both views.
- **PI-06f** Clicking or keyboard-activating a Pulse pad auditions its current sound on the selected audio output while transport is stopped or playing. Audition does not add a MIDI note or alter project state. Device setup and sound preparation stay off the audio callback and UI thread; a failed preview reports an actionable error.
- **PI-07** Native EQ, compressor, algorithmic and convolution reverb, and limiter expose stable parameter IDs, state, bypass and latency through the same processor interface. Their controls and meters are usable at 1080p and high DPI.
- **PI-07a** Every bundled effect and instrument has a purpose-built Qt Quick device panel with continuous knobs or sliders for continuous values, clear units/ranges, numeric entry, reset/default, visible bypass and useful signal feedback. EQ exposes an interactive response; dynamics show gain reduction and detector/sidechain state. A bundled instrument exposes its source, tone and envelope controls through the same interaction system. Pointer, keyboard and accessibility actions reach the same parameter command; one continuous gesture is one undo step. Controls that are safe to change during playback remain enabled and become audible at a bounded block boundary without stopping the transport. A change that needs a new processor or latency topology reports its pending/failed application status and the reason; the UI does not silently grey out the whole device.

The compressor now publishes the greatest makeup-free gain reduction applied in its latest block through a lock-free control-side reading. The Quick device panel shows that reading for the exact active compressor slot and shows it as unavailable when transport is stopped, paused, the graph is changing, or the saved slot is bypassed or retired. Sidechain detector gain contributes to the same reading. This is a block reading sampled by the UI, not a calibrated peak-hold meter; detector/sidechain state display and the remaining full PI-07a interactions are still open.

The limiter publishes the greatest attenuation actually applied to a non-silent delayed frame in its latest block. Its Quick device panel reads the exact callback-accepted limiter slot and shows an unavailable value when stopped, paused, draining, during graph handoff, or after that slot is bypassed or retired. A pending saved edit can leave the previous audible limiter active; the saved-versus-active status remains visible separately from the meter. The reading measures digital sample gain reduction, not true-peak limiting or calibrated loudness.

The bundled sampler is Crate, a chop sampler on the native synth platform ([ADR-0063](../decisions/0063-crate-chop-sampler.md), [spec 09](09-crate-sampler.md)). It replaces the earlier one-sample sampler (`SamplerPatch`, schema 1.26–1.43), which is removed with no migration. Its sample is managed media in the document's `sample` slot, its chops are document regions, and per-chop controls, presets, automation, macros, note effects, pedals, freeze and export come from the platform.

The compact EQ panel edits its first band's frequency, Q/width and gain, including live adjustments where the routed native-chain contract permits them. The expanded editor selects and edits any of up to eight bands, adds/removes bands, and offers peaking, low/high shelf and low/high pass shapes. Compatible filter-type changes and one-band append/removal use the bounded live EQ gate in ER-01b/ER-01d; unsupported or ambiguous structural changes stay saved but inactive until playback restarts. It plots the combined frequency response of the same prepared digital biquad coefficients used by playback. Its 20 Hz–20 kHz log grid is computed on the UI/control side from saved settings. Invalid settings show no curve and an unavailable label instead of a fabricated approximation. A 2,048-frame master output spectrum is overlaid only for the master path and is visual feedback, not a calibrated EQ-response measurement. The remaining full PI-07a controls are open.

- **PI-07b** Chop ([ADR-0075](../decisions/0075-chop.md)) is a tempo-locked gate and stutter with no latency. A pattern of 16 or 32 steps of 1/8, 1/16 or 1/32 runs from the song position. Each step has a level, a gate length, 1–4 ratchets, a repeat (2, 4 or 8 replays of its first slice, falling by Repeat Pitch and Repeat Decay) and a chance, with Swing (50–75 %) and Rotate over the pattern.
  - **Timing:** step edges land on the project grid to the sample, after a seek and across a loop.
  - **No clicks:** every edge fades over Smooth (0.5–10 ms), and repeats crossfade equal-power.
  - **Chance:** rolls from (seed, pattern count, step), so playback and export vary the same bars the same way.
  - **Determinism:** any block split renders bit-identically at a constant tempo, and Init is the input bit for bit.
  - **Limits:** repeats replay up to a fully swung 1/8 at 20 BPM; below that a repeat step plays through.
  - **Panel:** lanes are drawn by dragging, and a stroke is one undo step.

  Evidence: `native_chop_contract` and `quick_chop_panel_contract`, with the platform contract run over every Chop preset.
- **PI-08** A routing or topology edit that leaves a plugin configuration compatible reuses the live plugin instance and preserves its internal history. A replacement is created off the callback from saved state; loss of live tail/history uses the engine's reported transition policy.
- **PI-09** A verified continuous VST3 parameter may be changed during playback only when its manual value can be saved as one project revision and delivered to the matching live instance through a bounded, acknowledged event. The UI shows that revision as pending until the callback has applied it. Undo restores the saved value and uses the same active-revision rule. An automated parameter, changed plugin identity, unverified metadata, discrete-only control or changed latency is deferred with an explicit reason until its separate runtime contract exists. Save/reopen restores committed intent even if the preceding live run could not apply it; the UI must distinguish that prior audible state from saved intent. No UI control may imply a manual value is active merely because its control-side editor clone accepted it.

The generic VST3 effect and instrument-source parameter editors accept a verified, sample-offset-capable value while playback continues even if the live slot lacks parameter or latency qualification. It saves the plugin state and parameter reference as one revision, then labels that revision **saved, not yet audible**, with the preflight reason and a Stop/Play retry. A second saved-only value may supersede the first during the same run. A value arriving while a qualified live event awaits its callback receipt, or while the active graph identity is stale for another reason, is refused without saving so it cannot overtake an unresolved event. A plugin restart request downgrades the edit to saved-only. Unknown, invalid and unsupported-precision controls remain rejected. Native-editor `performEdit` values on a qualified control may be previewed through a coalescing, revision-free mailbox while the gesture is active. Each callback consumes at most one latest preview per block; graph identity, automation and latency checks are the same as for a committed event. Preview has no saved revision or applied receipt and must not be presented as committed. The completed `endEdit` saves one project revision and its committed event supersedes an unconsumed preview. Closing the editor saves remaining opaque component state without an Apply step; a plugin restart or unqualified control remains saved-only with an explicit reason. A pending committed event still prevents a competing value until its receipt is resolved.
- **PI-10** Production edits remain operable while playback runs, including clip and MIDI edits, source selection, instrument and effect controls, chain changes, routing, automation and mix controls. An accepted edit never stops the transport implicitly. A compatible control change becomes audible at a bounded audio boundary; a processor, source or latency change prepares a replacement off the callback and publishes it through a measured graph transition that preserves compatible processor history and handles tails. Until that transition exists for a change, the saved revision and audible revision remain distinct and the UI shows the reason. An edit is not called live merely because its saved state changed. Device reconfiguration and opening a different project are separate transport operations with explicit transition feedback.

VST3 effect and instrument-source manual-event slots expose the currently published graph revision, processor instance ID and compatibility key. Submission names all three plus the saved edit revision. Graph publication alone does not activate a slot: both the simple and production routed publishers activate it at the accepted callback block switch, after retiring the old graph identity. The routed chain adapters forward that lifecycle to embedded VST3 stages. A prepared, held, rejected or superseded runtime refuses events. A topology revision that reuses the processor retains its DSP state but invalidates any pending event for the old graph revision; the producer collects that failed receipt and submits a fresh saved revision against the new identity. Headless effect and instrument transactions now save clone-captured state, preflight the active slot and expose a revision/sample receipt. Routed control resolves exact saved effect and instrument slots only in the accepted graph; the instrument provider exposes its slot only with independent fixed-latency qualification. This handshake provides no generic plugin latency-stability proof. Quick manual editing remains gated on real-plugin qualification and a UI transaction that reports saved, pending and applied states.

## Component contract

The host owns lifecycle transitions `discovered → validated → instantiated → active → suspended/disposed`; failures never skip state cleanup. Live runtime instances have stable IDs and exclusive live-render ownership. Plugin identity uses format, vendor/class ID, version and path metadata but is not reduced to a filename. State blobs have size limits and validation. Parameters have stable IDs, ranges, units, step/discrete metadata and automation capabilities. Changes are delivered by bounded event buffers. The scanner helper protocol is versioned, timeout-bounded and treats scanned binaries as untrusted.

The routed Quick host applies a manual-control qualification to each saved VST3 effect instance, not to its entire binary or chain. A production record must match the pinned binary hash, class ID, parameter ID, project sample rate and processing block size. Preparation intersects those IDs with the loaded instance's continuous, writable, sample-offset-capable controls; an unknown or ineligible ID is not admitted. An unqualified effect in the same graph remains unqualified. Whole-binary admission is reserved for explicit synthetic fixtures; it is not inferred from a production parameter record. A new plugin version, sample rate or block size needs independent evidence.

The `plugin_ui` target owns the UI-thread editor bridge connecting VST3 hosting to a native X11 window. It provides Linux `IRunLoop` timers and file-descriptor handlers; the Quick shell can attach effect and instrument views while playback runs. Completed native gestures capture component state and parameter references from the suspended host and commit one exact-revision project edit to the corresponding effect slot or instrument source. Closing the editor captures remaining state. An unrelated project edit may advance the revision while the editor remains open: the editor can commit against the current revision only if its saved plugin target is unchanged. A changed or removed target still rejects the gesture. The current app can save such an edit without stopping playback, but a changed plugin instance is not yet republished to the active graph. This is an open PI-10 implementation gap, shown as saved but not audible. A failed attach falls back to the generic parameter view. The generic processor API remains `ProcessorRuntime::process(stereo, BlockContext)`; MIDI, latency, state, bypass and parameter facilities live in adapter-specific types. The synthetic attachment tests cover lifecycle and run-loop behaviour, but do not establish Wayland support or compatibility with representative third-party plugin editors. Exact VST3 SDK release and bundled-file inventory remain release review items.

## Acceptance evidence

| ID | Evidence |
| --- | --- |
| PI-01/05 | Scanner fixtures for valid, crashing, hanging and malformed plugins; quarantine/retry; fault during initialization and state restore; isolated-host latency measurement. |
| PI-02/03 | Browser and command tests; effect/instrument integration tests for MIDI, buses, bypass, parameter events and chain ordering. Before PI-03 closes, add `plugin_ui` run-loop timer/fd and attach/detach tests, plus an editor spike on Ubuntu 26.04 Wayland and X11 sessions with two reference plugins and generic-view fallback. |
| PI-04 | Golden save/reopen tests, missing-plugin placeholders, relink with and without version change, preserved automation and opaque state. |
| PI-06/07/07a | Fresh-install piano-roll task and demonstration-project render without third-party plugins; native DSP reference signals, parameter range/latency tests, CPU and sound-quality review. At 1080p and 4K, manipulate every bundled device by pointer and keyboard while transport plays, verify audible timing, saved values, one-step undo and clear status for an unsafe change. |
| PI-08 | Stateful fixture plugin keeps oscillator phase and reverb tail through a topology-only change; incompatible replacement produces a bounded, disclosed transition. |
| PI-09 | Synthetic plugin fixture checks audible block-boundary timing, exact saved and active revision acknowledgements, save/reopen, undo, mailbox saturation and plugin failure. Automated-lane precedence, changed latency and a stale or replaced instance fail closed without changing the audible parameter. |

The compatibility suite records tested plugin versions and known limitations. Passing synthetic fixtures is not a claim that all VST3 plugins work.

VST3 effects and instruments receive project sample, musical position and
instantaneous BPM from the shared tempo map at each process-call start.
Stepped events split a callback block at their sample boundary; gradual curves
use the BPM at each call start because VST3 `ProcessContext` carries one BPM for
that call. The host does not promise sample-accurate tempo variation inside a
single plugin call. Playback and offline export use the same preparation and
transport rule, with timing quality still subject to real-plugin qualification.

The bounded routed-live implementation accepts three serial verified VST3
effects on one audio track. Its prior two-effect paths remain available: two
serial effects on one audio or bus path, or two on separate audio tracks. One
bounded MIDI instrument path permits up to two serial verified VST3
effects on the instrument's source track, with stable slots, summed latency,
stateful runtime reuse, and matching stopped master/stem export. The existing
one-effect paths on a separate audio track or group/return bus remain available;
combining those paths with the new two-effect source chain needs separate
qualification. Every unsupported combination reports saved versus audible
state instead of silently omitting an effect. The bus owns its saved slot, live
processor and processed export path in audio-only and mixed instrument projects.
An additional exact layout has two direct VST3 instrument source tracks with
one verified VST3 effect on each. Per-instance source and effect evidence is
required; Quick playback, offline master/selected stems and fake-JACK export
tests cover independent paths, delay alignment and tails. Extra tracks, sends,
automation or effects are not admitted by that Quick layout, and real plugin
and physical device qualification remains separate.
Each processor retains its runtime and
process-fault probe through compatible held route edits. Initial fixed
latency enters route PDC. Missing scans and changed saved plugin state fail
the new graph application while preserving saved intent and prior valid
playback. The control API can query a fault by track or return the first fault
for legacy callers. The Quick browser can append either supported effect
shape. A saved normalized VST3 effect parameter without an opaque component
state is applied on the control side before the first audible block in live
playback and bounded offline export. Unknown parameter IDs fail closed. A saved
component state remains authoritative when both forms are present; export
rejects unresolved references rather than publishing an ambiguous render.
The Quick fallback editor can enter a supported normalized value when the
verified host is available. Each accepted value commits one project revision;
missing or unsupported values remain read-only. A newly edited value is
captured as component state where the plugin supplies one and as a saved
parameter reference for recovery and diagnostics.
Continuous normalized VST3 instrument parameter lanes now use the shared
track/clip curve evaluator. A prepared source sends offset-sorted, bounded
parameter changes with MIDI notes in each host block; saved parameter
references supply the unautomated baseline. Unknown or unsupported automation
parameters fail source preparation, and the previous audible revision remains identified
until the user restarts playback with the new source. A saved opaque component
state takes precedence over static values; static values without opaque state
are staged before activation and flushed before the first audible block.
Replacement while playing, dynamic latency, arbitrary native/VST3 chains and
a custom plugin editor remain outstanding.
Removing or bypassing a sole active zero-latency native EQ, compressor or reverb from an audio/bus path
can publish at a held boundary while the old stage drains a bounded tail into
the now-dry path. Bypass preserves the saved slot and settings. A second insert
change while that tail is active can start a new zero-latency processor on the
same route; the draining tail and new processor sum independently. Limiter
removal, sidechain paths and compound edits still require a
separate transition contract.
The same held transition permits a sole master EQ or compressor to be added,
bypassed, re-enabled or removed while playback runs. EQ uses its measured
finite filter-tail bound; compressor zero-input output has no audible release
tail. A master limiter's lookahead still needs a separate latency contract.
Removing the final effect from a two-stage, zero-latency native track or bus
chain reuses the unchanged earlier processor and drains only the removed
stage's bounded zero-input tail. The edit must be isolated; sidechain targets,
VST3 stages, limiter latency and removal of the earlier stage remain guarded.
The same rule applies to a two-stage master chain: an unchanged, zero-latency
prefix keeps its runtime history while the removed final stage drains. Earlier
stage removal and multi-stage rewrites remain guarded.
The verified VST3 instrument may feed a bounded ordered native chain or one
VST3 effect on its own path. The saved source identity remains distinct from
effect slot IDs; live graph edits reuse compatible prepared processors, while
offline master/stem export accounts for the chain's latency and finite tails.
An audio stem keeps the instrument path muted in its temporary render graph
to preserve the master's parallel-path compensation.
The bounded offline VST3 export slice is documented in
[its contract](../export/vst3-effect-slice.md). See
[ADR-0017](../decisions/0017-live-vst3-track-composition-proposal.md)
and the [routed fixture baseline](../performance/routed-vst3-single-insert.md).
