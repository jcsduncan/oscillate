# Oscillate — Product Requirements Document

**Status:** Draft for HLD and detailed specifications

**Target platform:** Ubuntu 26.04 LTS, x86-64

**Product direction:** A production-ready, Linux-first digital audio workstation for beginners through professional producers.

## 1. Product vision

Build a polished, reliable and extensible DAW that enables users to complete real music-production projects on Linux, without compromising the capabilities expected of a modern desktop production tool.

The product will combine an arrangement-first workflow, non-destructive audio editing, MIDI sequencing, instruments, automation, mixing and mastering. It will prioritise predictable behaviour, low-latency playback, project integrity and a maintainable architecture.

The product will use a custom C++ audio engine and DAW-specific components. Qt is the preferred candidate for general UI infrastructure, subject to the HLD’s dependency and licensing review. The product will not use JUCE.

## 2. Goals and success criteria

### Product goals

1. Enable users to create, arrange, edit, mix and export complete musical productions.
2. Deliver stable, responsive audio playback under a representative production workload.
3. Provide non-destructive editing and reliable recovery from application or system failures.
4. Support Linux VST3 instruments and effects with useful compatibility diagnostics.
5. Establish an extensible foundation for future workflows, including Session View and advanced MIDI features.
6. Maintain clear licensing, dependency and project-format governance.

### Launch acceptance criteria

- A user can complete a representative production using audio clips, MIDI clips, instruments, effects, automation, buses, sends and returns, then export a master and processed stems.
- Project save, reopen, media relinking, plugin-state restoration and crash recovery pass defined integrity tests.
- The application passes the agreed 32-track performance qualification on reference hardware at 48 kHz and 64- and 128-sample buffers.
- Audio playback meets the defined underrun threshold during controlled benchmark runs.
- Core workflows are usable at 1080p and 4K/high-DPI resolutions.
- Critical workflows pass structured usability testing, with measurable task-completion and critical-error criteria.
- Dependencies, plugin limitations, licensing obligations and known issues are documented before release.

## 3. Target users and workflow

The product serves beginners through professionals, with a hybrid arrangement-first workflow. At launch, it supports production from imported audio, drawn MIDI and instruments; capturing new audio or MIDI performances is outside v1.

The primary workflow is:

1. Create or open a project and configure the audio device.
2. Import audio, analyse tempo and key, and review waveform and beat-grid information.
3. Create audio and MIDI clips in the arrangement.
4. Edit, arrange, stretch and pitch-shift material non-destructively.
5. Add instruments and effects, build routing, and shape the mix.
6. Automate tracks, clips and plugin parameters.
7. Export a master and processed stems.
8. Save, reopen, revise and recover the project without losing source media or edits.

Session View is post-MVP. The MVP supports MIDI playback, instruments, a piano roll and MIDI recording from the computer keyboard, but not audio recording, hardware MIDI input or advanced MIDI editing.

## 4. Scope

### Included in v1

- Arrangement timeline and clip editing.
- Audio import and analysis.
- Non-destructive time-stretching, warping and pitch-shifting.
- Editable tempo and time-signature maps, musical and elapsed-time references.
- MIDI clips, a piano roll (with velocity and pianist pedal lanes) and VST3 instruments.
- MIDI recording from the computer keyboard, with overdub, a metronome and a count-in.
- Bundled instruments on one native synth platform (§13), including Crate, a chop sampler ([ADR-0063](decisions/0063-crate-chop-sampler.md)).
- Native EQ, compressor, reverb and limiter, plus the extended native effect and utility palette (§15).
- Linux VST3 effects and instruments.
- Track and clip automation.
- Mixer, groups, buses, sends, returns and sidechain routing.
- Plugin delay compensation.
- Metering, loudness and spectrum analysis.
- Offline and real-time export.
- Project media management, autosave, recovery and diagnostics.
- Starter sounds, templates and a demonstration project.

### Explicitly deferred

- Session View.
- Audio recording.
- Advanced MIDI editing.
- Hardware MIDI device input.
- Multichannel output beyond stereo.
- Other features that materially expand the launch scope without being required to complete the primary production workflow.

Deferred items must not constrain the architecture unnecessarily, but they are not launch commitments.

Stereo is the v1 output layout. MIDI notes are drawn in the piano roll or recorded from the computer keyboard; hardware MIDI controller input is outside v1.

## 5. Platform, packaging and licensing

### Platform

- Ubuntu 26.04 LTS, x86-64 only at launch.
- PipeWire and JACK are release-critical audio backends.
- ALSA fallback where practical.
- Native Ubuntu package and AppImage distributions.
- 1080p and 4K/high-DPI support.

### Framework and dependency policy

- Custom C++ audio engine and DAW-specific components.
- No JUCE.
- Qt is the preferred general UI framework candidate; final module selection and LGPL/GPL obligations must be validated before implementation commitments.
- Prefer MIT, BSD and Apache-licensed dependencies where technically suitable.
- The project is licensed GPL-3.0-or-later. Every dependency's terms must be compatible with that licence and with binary distribution.
- Avoid paid or restrictive dependencies unless there is a compelling technical justification and explicit approval.
- Maintain a component-level software bill of materials and licence inventory, including transitive dependencies and VST3 SDK terms.
- Use mature DSP libraries where appropriate rather than implementing general-purpose DSP infrastructure without a clear product advantage.

## 6. User experience and application structure

The interface will use a dark-first visual design, scalable typography, high contrast, accessible labels, keyboard access to core workflows and non-colour indicators for important states.

The main workspace integrates:

- Arrangement timeline.
- Track controls and mixer.
- Automation lanes.
- Piano roll.
- Plugin and instrument browser.
- Project and media management.
- Processing and diagnostics panels.

The master bus remains pinned at the bottom of the track area in every project. A subdued add-track row is always directly above the master, below all ordinary and bus tracks, with an accessible plus button for track choices. Choosing from this row inserts the new track immediately above it. Dropping a local audio file or an instrument into the project window creates the corresponding track above the add row; the instrument and its insert form one undoable edit.

Panels must be resizable and show/hide capable, with saved layouts. Detachable floating panels are not required for v1.

The arrangement beat/bar grid remains visible across empty audio, instrument and bus rows; row backgrounds must not hide timing references. Selecting any track, bus or master exposes a resizable bottom device dock showing source or instrument, ordered effects and output in signal-flow order. Cards show useful compact controls, bypass and saved-versus-audible processing status; effects can be reordered by drag or keyboard. A larger device editor opens from its card. A separate right context panel opens only for detailed clip, routing, automation or project tasks and may be closed to give the arrangement more width. Native effects and VST3 effects use the same chain workflow; native effects do not need to be distributed as VST3 binaries.

The top transport area keeps project BPM and meter visible and directly editable. An optional project key/scale setting describes musical intent and does not silently transpose audio or MIDI. It must persist with the project once introduced. Instrument and effect discovery share a searchable browser that distinguishes bundled devices from VST3 plugins and makes the selected insertion target clear. For VST3, the compact card is host-owned and uses exposed parameters; the larger editor uses the plugin-provided view where available, with an honest generic fallback.

Bundled effects and instruments need purpose-built, consistent device panels with responsive knobs, sliders, value entry, useful response or envelope displays and meters. Safe continuous controls remain usable while playback runs and become audible without stopping transport. Changes that require processor replacement or latency changes expose their application status and reason. The panels must remain clear and operable at 1080p and 4K/high DPI, with keyboard and accessible control equivalents.

First-run experience includes audio-device setup, an introduction to core workflows, a sample project and starter content. Contextual tooltips, searchable documentation and separate deeper guides must be available.

Track colours are user-configurable. Colour must not be the sole indicator of state.

## 7. Audio engine and playback

The engine will run in the same process as the application, with a dedicated real-time audio thread and strict separation between UI and audio processing.

### Real-time requirements

- The audio callback must never block on UI work, disk operations, network activity, cache generation or unbounded locks.
- Editing operations are queued and applied at safe processing boundaries.
- Live graph changes must preserve running instrument/effect state and tails where technically compatible. If a processor must be replaced and continuity cannot be preserved, use a bounded, visible transition rather than silently resetting it.
- When an accepted project edit is pending or fails to reach the audio engine, the UI must distinguish saved project state from active playback and provide an actionable retry or undo path.
- UI rendering, waveform generation, analysis, file I/O and background processing must not block the audio callback.
- Playback-critical work takes precedence over non-essential background jobs.
- Device disappearance must be detected and handled safely, preserving project state and offering device recovery or reselection.
- Audio-engine failures and plugin faults must be contained where possible, with bypass/mute, user notification and project preservation.

### Performance qualification

Reference workload: modern mid-range desktop, 32 GB RAM and SSD, with mixed native and VST3 processing, buses, sends, automation and a representative number of instruments.

Test at 48 kHz with both 64- and 128-sample buffers. The 32-track workload is the launch qualification target; approximately 100 tracks is a scalability goal, not a launch gate.

The HLD and test specification must define the reference machine, plugin set, run duration, background load, repetition count, stress threshold and acceptable underrun rate. The standard qualification run targets zero underruns.

Support 44.1, 48, 88.2 and 96 kHz sample rates.

Each project has a nominal sample rate. The engine renders at that rate. The PipeWire adapter requests it and reports any conversion performed by PipeWire when its graph runs at another rate. A JACK server-rate mismatch stops playback with instructions to change the server or project rate; the engine does no whole-mix resampling. Changing the project rate is an explicit, undoable operation that preserves elapsed-time positions and invalidates rate-dependent caches.

## 8. Audio media and project management

### Import

Support WAV, AIFF, FLAC and MP3. By default, import copies the original encoded source into managed project assets; decode as required rather than mandating source conversion. Users may explicitly link to external source files instead. Linked media must be identified in the project and clearly reported when unavailable.

On import, generate waveform and analysis information in the background.

Media at a different rate from the project is resampled for playback and export without changing the encoded original. Playback decoding, source-rate conversion and lookahead processing run outside the audio callback.

### Project structure

Projects use a folder structure with managed assets, collect-and-save support and stable media identifiers. Projects containing only managed media are self-contained. Collect-and-save copies any linked external media into managed project assets so the resulting project is self-contained. The project format must be versioned and include a readable manifest. A database is optional only if profiling demonstrates a need.

Project state and essential media must be separate from disposable processing caches. Source media must never be evicted by cache management.

Plugin state, automation, clip settings, tempo and time-signature maps, project sample rate and routing must survive save/reopen. Missing plugins must not destroy their identity, parameters, state, chain position, routing or automation references.

### Integrity and recovery

During greenfield development before the first public beta, only the current
project schema must open. Earlier development projects have no migration
guarantee. Compatibility and migration obligations begin with the first
publicly released project schema.

- Atomic and crash-safe project writes.
- Configurable autosave and crash-recovery snapshots.
- Named revisions or snapshots.
- Checksums, integrity checks and explicit relinking rules.
- For publicly released project schemas, pre-upgrade backups, migration rollback
  tests and versioned project migrations.
- On recovery, let users choose between the recovered state and the last saved project.
- Preserve user corrections when rebuilding analysis or derived data.

## 9. Arrangement and clip editing

The arrangement supports bars/beats and elapsed time, explicit snap controls, zoom, navigation, markers, loop regions and locators.

Audio edits are non-destructive. Users can split, move, duplicate, delete, trim, slip-edit, stretch and fade clips. Multi-track editing is supported.

Normal clip-edge dragging trims source material rather than changing speed. Extending a trimmed clip reveals the original source where available. Slip editing is available through a modifier or dedicated tool.

Same-track clips cannot overlap or play simultaneously. Dragging, duplication and resizing must respect neighbouring clip boundaries.

Editing gestures should be predictable and reversible. One logical operation produces one undo step; continuous gestures are coalesced, and compound operations are atomic.

## 10. Tempo, timebases and audio analysis

Projects have a global tempo and an editable tempo map. Tempo changes can be instantaneous or gradual, with linear BPM interpolation and simple ease-in/ease-out behaviour. Tempo events follow global snap unless snapping is disabled. An editable time-signature map defines bar and beat numbering; changes occur at bar boundaries and persist with the project.

Musical-time is the default for clips and track automation. Users can pin clips and automation to elapsed time. The UI must make the reference mode visible and straightforward to change.

A single authoritative timing implementation for tempo and meter must be used for playback, time conversion, MIDI, automation, warping and export. Playback and export must agree on timing semantics.

### Audio analysis

Analyse and display:

- BPM and confidence.
- Key and compatible-key information.
- Waveform and beat grid.
- Transients and onset information.
- Suggested warp markers.

Analysis runs in the background and never blocks editing. If detected tempo conflicts with the project tempo, show the estimate and confidence and offer an explicit conform action.

Analysis results, confidence and generated suggestions persist in the project. Generated suggestions must be distinguishable from user edits, and reanalysis must not overwrite manual corrections.

## 11. Warping, time-stretching and pitch-shifting

### Warp editing

Automatically suggest beat and warp markers. Show suggestions on the waveform and beat grid for review before applying them. Users can add, move and delete markers and audition the result against project tempo.

Applying a reviewed marker set is one atomic, undoable operation. Moving a marker adjusts the affected segment while preserving other locked or anchored markers. Warp settings belong to the clip instance; shared source audio remains unchanged.

### Stretch modes and quality

Automatically classify material as rhythmic, melodic or complex, and select a suitable stretch mode. Users can override the choice per clip. Persist classification, confidence, selected mode and override; reanalysis must not overwrite a manual selection.

Support wide stretch and compression ratios. At extreme ratios, warn about likely quality degradation but allow the operation. Preserve pitch by default during time-stretching; pitch change is a separate operation.

Real-time quality adapts to workload to preserve stable playback. Offline export uses the highest supported quality independently of playback quality. Any persistent limitation must be reported.

### Pitch-shifting

Provide semitone and cent controls plus musical-interval shortcuts. Pitch-shifting is tempo-independent, non-destructive and previewable. Support shifts in both directions and warn when quality is likely to degrade.

Preserve formants by default for vocals and other formant-sensitive material, with per-clip control. Select pitch-shifting algorithms automatically based on source material, with manual override.

Where supported, use a combined time-stretch/pitch-shift path. Sequential processing is an explicit compatibility fallback. Real-time and offline paths must preserve the same requested pitch and duration semantics, even where quality differs.

Settings are stored per clip instance. Shared source audio remains unchanged.

## 12. Processing jobs and caches

Expensive analysis, warping, pitch-shifting and derived-media work use disposable background caches.

### Scheduling

Prioritise:

1. Work needed by clips approaching playback.
2. Visible or currently edited material.
3. Speculative cache generation.

Cancel or supersede obsolete jobs when settings change. Latest settings take precedence, and stale results must never be installed.

Background concurrency adapts to system load and audio-engine headroom, subject to a user-adjustable resource limit. Scheduling and monitoring must never block the audio callback.

### Cache behaviour

Use a dedicated project-local cache directory, excluded from essential project data. Provide a configurable global cache budget and least-recently-used eviction.

Cache keys include source identity/version, clip-instance settings, algorithm and version, quality settings and relevant dependencies. Writes must be safe under interruption.

Reuse valid caches on reopen and regenerate missing or stale caches in the background. Cache loss must never damage the project. If a valid cache is unavailable during playback, use the real-time adaptive-quality fallback where possible.

Never evict source media, project state or user renders.

### Processing interface

Show unobtrusive per-clip status and a detailed processing panel with progress and queue information. Distinguish queued, running, completed, cancelled, failed, stale and unavailable states.

Users can cancel individual jobs and pause/resume non-essential work. Queued or throttled jobs show a concise reason without promising completion times.

Provide cache health, project cache usage, global budget and clear-cache controls. Clearing cache removes only reconstructable files. Cache failures must be reported, with safe fallback and quality warnings where appropriate.

## 13. MIDI and instruments

MIDI clips are created by drawing empty clips in the arrangement and adding notes in the piano roll, by importing a Standard MIDI File, or by recording from the computer keyboard.

The piano roll supports mouse-based note creation and editing, plus keyboard shortcuts for common operations. The computer keyboard plays the selected track's instrument; recording places each note at the project position the player heard when the key was pressed, overdubs onto existing notes and layers each pass of a looping clip. A metronome sounds on the output device only, never in exports, with a one-bar count-in before a take started from stop ([ADR-0086](decisions/0086-midi-recording-and-click.md)). Chords are ordinary notes in MIDI clips ([ADR-0084](decisions/0084-chord-clips.md)). Hardware MIDI input and advanced MIDI editing are outside the agreed v1 scope.

Support VST3 instruments and the bundled instruments. A fresh install must be able to play and export MIDI clips without a third-party plugin. The bundled instruments are:

- Crate, a chop sampler on the native synth platform ([ADR-0063](decisions/0063-crate-chop-sampler.md), replacing the simple sampler), with a small, royalty-clear starter sample set as presets;
- native synths sharing the same platform: Prism (default subtractive), Ember (vintage analogue poly; [ADR-0066](decisions/0066-ember-circuit-model.md)), Nova (flagship wavetable, sample, granular and spectral synth; [ADR-0057](decisions/0057-nova-flagship-synth.md)), Pulse (analogue-modelled drums), Halo (six-operator FM), Torque (mono bass), Drift (granular), Anvil (drawn kick; [ADR-0060](decisions/0060-anvil-kick-synth.md)) and Knock (modelled percussion; [ADR-0068](decisions/0068-knock-percussion.md));
- sampled instruments whose libraries are installed separately by a bundled script: Concert (the Salamander Grand Piano; [ADR-0071](decisions/0071-concert-sampled.md)), Fret (a fingered bass; [ADR-0088](decisions/0088-fret-sampled-bass.md)) Wren (a concert flute; [ADR-0089](decisions/0089-wren-sampled-flute.md)), Bow (string sections and solos; [ADR-0111](decisions/0111-bow-sampled-strings.md)) and Stick (multi-mic acoustic drum kits; [ADR-0114](decisions/0114-stick-sampled-drums.md)).

Each has factory presets, a designed panel, live telemetry, automatable parameters and export parity with live playback.

MIDI clips also carry sustain (with half-pedal), sostenuto and soft pedal lanes, honoured by every bundled instrument ([ADR-0035](decisions/0035-pedals-in-midi-clips.md)). Pitch bend, modulation and pressure are clip automation lanes on each synth's performance parameters. Instrument presets and plugin state must integrate with save/reopen, undo where applicable, and missing-plugin recovery.

MIDI clips must respect the shared tempo map and project timebase rules. Clip duplication and looping must preserve intended note timing and duration.

## 14. Automation

Support both track-level and clip-attached automation.

- Track automation controls mixer and device parameters at arrangement level.
- Clip envelopes are clip-specific and travel with their clips.
- Clip envelopes are edited in Arrangement View in v1; Session View support is deferred.
- For a parameter controlled by both sources, the active clip envelope takes precedence while the clip is active. When the envelope relinquishes control, the parameter returns to the value defined by the track automation curve at that time, with smoothing where needed to avoid an audible click. There is no implicit blending or summing.
- Conflicting sources must have explicit precedence and visible indicators.

By default, clip envelopes travel with their clip and track automation stays at its arrangement position. When moving a clip across track automation points or regions creates an ambiguous result, offer an explicit choice and allow the user to remember it.

### Editing and curves

Support linear, quadratic, exponential, ease-in/out and custom Bézier curves. Provide ordinary breakpoints, custom handles, per-segment interpolation and parameter-type defaults. Discrete parameters use step/hold behaviour.

Users can create automation from a parameter context menu or by dragging parameters into the arrangement. Provide searchable plugin parameters grouped by plugin and parameter category. New lanes appear immediately; collapsing a lane never deletes its data.

Lanes are nested beneath their owning track, independently collapsible and resizable. Support practical large lane counts through efficient rendering, search/filter, favourites and compact views.

Users can add points, drag them, enter numeric values, snap or disable snap, select multiple points, move/scale values, copy/paste points or regions, and use named reusable curve/region presets.

Copy/paste must preserve timing and shape where valid, map values safely to the target parameter range and warn about semantic incompatibility. Persist timebase, interpolation, handles, region duration and parameter-domain metadata.

### Precision

Use sample-accurate scheduling where supported. Smooth parameters only where needed to prevent zipper noise, without shifting intended timing. Where a plugin cannot meet requested precision, use its best supported behaviour and report the limitation.

Playback and export must use the same curve evaluation semantics.

## 15. Mixing, routing and native processing

Support audio, MIDI/instrument, group-bus and return tracks.

The mixer provides channel controls, insert effects, sends, routing destinations and visible signal flow. Users can reorder, bypass and manage effects, save chain presets, copy/paste chains and open custom plugin interfaces.

Track, group, return and master processing chains must have consistent insertion, ordering, bypass, automation, save/reopen and missing-processor behaviour. Dragging an effect into or within a chain is one undoable project edit; the UI shows whether the edited chain is active in the audio graph or pending/failed application.

Support flexible routing across tracks and buses, including sidechain routing to compatible plugin inputs.

### Native effects

Provide:

- Parametric EQ with interactive response, common filters and spectrum display.
- Compressor with core controls, selectable character/models and advanced sidechain filtering.
- Algorithmic and convolution reverb.
- Limiter.

The baseline processors are the Shape (EQ), Press (compressor), Expanse (algorithmic reverb) and Ceiling (limiter) engines on the generic native effect platform ([ADR-0040](decisions/0040-native-effect-platform.md), [ADR-0047](decisions/0047-classic-processors-as-engines.md)). Cast is the convolution reverb on the same platform ([ADR-0115](decisions/0115-cast-convolution-reverb.md)). The same platform adds:

- a filter (Contour), saturation (Forge), modulation (Orbit) and delay (Relay);
- a stereo imager (Vista) and a DJ three-band isolator (Deck, [ADR-0055](decisions/0055-deck-dj-isolator.md));
- a kick/bass masking resolver (Pocket, [ADR-0041](decisions/0041-pocket-kick-bass.md));
- a drawn volume shaper (Pump, [ADR-0058](decisions/0058-pump-volume-shaper.md)) and a tempo-locked gate and stutter (Chop, [ADR-0075](decisions/0075-chop.md));
- a resonance suppressor with dynamic EQ bands (Temper, [ADR-0064](decisions/0064-temper-resonance-suppressor.md)) and multiband dynamics (Strata, [ADR-0082](decisions/0082-strata-multiband.md));
- a clipper for drums and the master whose true peak never passes its ceiling, 0 dBFS or lower (Edge, [ADR-0108](decisions/0108-edge-clipper.md), [ADR-0113](decisions/0113-edge-adjustable-ceiling.md));
- a gate and downward expander with hysteresis, hold, lookahead and a filtered or external key, for drum bleed and gated reverbs (Hush, [ADR-0116](decisions/0116-hush-gate-expander.md));
- a transient shaper whose attack and sustain gains do not depend on the level, single-band for drums or in up to four bands for a bus or the master (Strike, [ADR-0118](decisions/0118-strike-transient-shaper.md));
- adaptive tonal balance that steers the programme's long-term spectrum toward a tilt, a drawn or learned curve or a reference track, with no latency (Keel, [ADR-0119](decisions/0119-keel-adaptive-tonal-balance.md));
- a spatial pass-by effect (Flyby, [ADR-0067](decisions/0067-flyby.md)) and background texture and ageing (Dust, [ADR-0069](decisions/0069-dust-texture.md));
- the utility suite ([ADR-0042](decisions/0042-native-utility-suite.md)): Lens (spectrum analyser, with match EQ into Shape and tonal balance targets measured from references, [ADR-0120](decisions/0120-match-eq-and-tonal-targets.md)), Loudness (EBU R128), Scope (oscilloscope and alignment), Tune (tuner), Trim (gain and stereo utility) and Signal (test generator).

Every native effect is insertable on any track, bus or master chain, saved with the project, automatable and rendered identically by live playback and export ([ADR-0043](decisions/0043-one-native-insert-path.md), [ADR-0050](decisions/0050-one-chain-per-track.md)).

### Metering and delay compensation

Provide peak and RMS metering, clipping indicators, spectrum analysis, and integrated, short-term and momentary loudness plus true peak following [EBU R128](https://tech.ebu.ch/publications/r128) / ITU-R BS.1770 measurement semantics.

Plugin delay compensation must operate automatically within defined engine limits across tracks, buses, sends and parallel paths. Recalculate safely when plugins are bypassed, removed or changed. If a path cannot be fully aligned, report the limitation clearly.

During graph and plugin-latency changes, maintain parallel-path alignment where possible. If a safe transition is unavailable, hold, bypass or mute the affected path with a visible explanation rather than knowingly mixing it out of alignment.

The HLD must define maximum compensation limits, latency accounting, safe recalculation and failure behaviour. Routing changes must be reflected accurately in the UI.

## 16. Plugins and compatibility

Linux VST3 is the initial plugin format. Support a core subset first and expand based on compatibility testing.

Provide a searchable plugin browser with categories, favourites and tags; drag-and-drop insertion; effect chains; custom GUIs; bypass; presets; and chain copy/paste.

Scan plugins out of process where practical. Quarantine failures and allow retry/rescan. Default to in-process plugin execution for performance, with an isolation path for problematic plugins. Isolation is best-effort and must not be represented as a guarantee against all failures.

Expose compatibility status: tested, known-problematic or unverified. Test instruments and effects, GUI behaviour, parameter automation, state save/restore and project reopening.

If a plugin is missing, retain a placeholder preserving plugin identity, parameters/state, chain position, routing and automation references. Allow relinking.

## 17. Export

Support offline and real-time export.

Master formats:

- WAV PCM, 16-bit, 24-bit and 32-bit float.
- Selectable sample rate.
- TPDF dither when reducing integer bit depth, flat or noise-shaped (a psychoacoustic shape by default in the export panel).
- FLAC.
- MP3.

Support processed stems with effect tails and a master render. Exact stem summation to the master is not guaranteed, but effects must not be missing and tails must not be truncated.

Export must use the authoritative tempo map, automation evaluation, routing and compensation semantics. Offline processing should use the highest supported processing quality.

An xrun during real-time export fails that render, records the event and preserves any previous valid output. Output-rate conversion runs outside the audio callback.

Acceptance tests must include repeat-render consistency, correct tail handling, effect inclusion and verification of output formats and bit depths.

## 18. Undo, save and project state

Undo/redo covers clip editing, fades, warping, pitch settings, automation, plugin parameters and mixing/routing changes.

One complete logical operation maps to one undo step. Continuous edits are coalesced by gesture, and compound operations are atomic. Undo must restore both the visible edit and its associated state.

Undo history is session-scoped and is not persisted. Named revisions provide cross-session return points.

Persist user choices separately from derived data. Rebuilding a cache or rerunning analysis must not overwrite manual choices.

## 19. Diagnostics, support and updates

Diagnostics are local by default. Anonymised crash/performance diagnostics require explicit opt-in. Diagnostic bundles are reviewed by the user before sharing.

Include useful engine, device, plugin, processing-job, cache and project-integrity diagnostics. Provide support documentation, known issues and public issue reporting.

Updates notify users but require explicit update action. Provide a documented rollback path appropriate to each distribution (including package-manager rollback for the native package), stable releases and an opt-in beta channel. Project migrations must be tested, backed up and reversible where practical.

## 20. Accessibility and usability validation

Support scalable text, high contrast, keyboard access to core workflows, accessible labels and non-colour state indicators. Formal screen-reader support is not a v1 commitment.

Conduct structured usability tests with representative users and realistic production tasks. Define task-completion, time-to-completion and critical-error criteria before release. Findings that prevent completion of a real production project are launch blockers.

## 21. Security, reliability and fault handling

- Treat project files and plugin content as potentially malformed or untrusted inputs.
- Validate project manifests, plugin state and imported media metadata.
- Prevent failed cache writes or background jobs from corrupting project state.
- Quarantine repeatedly failing plugins and make recovery paths visible.
- Ensure device loss, plugin failure and cache failure do not silently discard user edits.
- Test crash recovery, interrupted writes, project migrations, missing media and missing plugins.
- Provide actionable errors without exposing sensitive local paths or diagnostic data unnecessarily.

## 22. Non-functional requirements

### Responsiveness
Editing and navigation remain responsive during background analysis, cache generation and export preparation. Background work must yield to playback-critical tasks.

### Determinism
Tempo conversion, automation, routing, warp settings and export semantics must be consistent and testable. Repeated renders should meet defined consistency tolerances.

### Scalability
Qualify the launch target at 32 tracks; design for future workloads approaching 100 tracks. Large automation-lane counts, plugin lists and cache indexes must not degrade UI responsiveness unacceptably.

### Maintainability
Keep stable interfaces between the engine, project model, plugin host, media services, scheduler and UI. Version project formats, cache keys and algorithm settings. Add regression tests for critical semantics.

### Portability
Keep platform-specific audio and packaging concerns behind explicit interfaces. Ubuntu x86-64 is the only launch target, but the architecture should not unnecessarily preclude future Linux distributions or architectures.

## 23. Release gates

1. **Functional:** Core production workflow completed without missing critical features.
2. **Performance:** Reference workload passes at both target buffer sizes under controlled test conditions.
3. **Integrity:** Save/reopen, autosave, crash recovery, relinking and plugin-state tests pass; migration tests apply once a prior public project schema exists.
4. **Audio correctness:** Tempo maps, automation, warping, routing, delay compensation and export semantics are consistent.
5. **Plugin compatibility:** Defined compatibility suite passes; known limitations are published.
6. **Usability:** Core tasks meet agreed completion and critical-error criteria.
7. **Licensing:** Dependency inventory, SDK terms and distribution obligations are reviewed.
8. **Supportability:** Diagnostics, known issues, rollback and release processes are operational.

## 24. Risks and mitigations

- **Real-time instability:** Strict callback rules, bounded work, load testing and playback-first scheduling.
- **Plugin incompatibility:** Compatibility suite, out-of-process scanning, quarantine and best-effort isolation.
- **Project corruption or migration failure:** Atomic writes, versioning, snapshots, integrity checks and rollback tests.
- **Processing latency and resource pressure:** Adaptive quality, prioritised jobs, cancellation, cache limits and clear status.
- **Licensing exposure:** Early framework/module review, SBOM and explicit approval for exceptions.
- **Stretch, pitch and analysis licensing:** Candidate libraries may use AGPL or commercial terms that are incompatible with the project's GPL-3.0-or-later licence or with distribution. Before implementation, choose and review one viable path for each capability: accept compatible reciprocal source obligations and product licensing, approve a commercial licence, or justify and qualify an in-house implementation. No candidate is approved by mention alone. See [Rubber Band](https://breakfastquay.com/rubberband/license.html), [libkeyfinder](https://github.com/mixxxdj/libkeyfinder), [Essentia](https://essentia.upf.edu/licensing_information.html) and [SoundTouch](https://www.surina.net/soundtouch/license.html) licence information.
- **Scope growth:** Protect the MVP boundary; defer Session View, recording and advanced MIDI editing.
- **Underspecified performance targets:** Define reference hardware, workloads, durations and tolerances before qualification begins.

## 25. HLD and specification work

The next phase should turn this PRD into architecture and testable specifications. The HLD must resolve and document:

1. Audio graph representation, scheduling, buffer ownership and thread-safety contracts.
2. Safe propagation of routing, plugin, tempo and automation changes to the real-time engine.
3. Plugin-host boundaries, failure containment and the limits of plugin isolation.
4. Delay-compensation algorithm, limits and dynamic graph-update behaviour.
5. Project schema, media identity, migration and recovery protocols.
6. Processing scheduler, cache key format, invalidation and cancellation semantics.
7. Shared timing and curve-evaluation contracts across playback and export.
8. Framework/module licensing decisions and the supported dependency set.
9. Performance qualification methodology and measurable release thresholds.

These are HLD/specification deliverables, not reasons to leave the product requirements blank. Defaults in this PRD should be treated as the baseline unless the HLD demonstrates a material technical conflict.
