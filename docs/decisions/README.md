# Architecture decisions

Every architecture decision record (ADR). An ADR records the decision as made;
the code is the current implementation.

Numbering: take the next free number before writing a new ADR. Numbers are
never reused. 0070 was never assigned.

| ADR | Decision | Status |
| --- | --- | --- |
| 0001 | [Processor runtime survives topology rebuilds](0001-processor-runtime-state.md) | Accepted; implemented |
| 0002 | [Prepared graph publication and reclamation](0002-graph-publication.md) | Accepted; implemented |
| 0003 | [Plugin scanning and execution boundaries](0003-plugin-isolation.md) | Accepted; implemented for scanning |
| 0004 | [Generation-based atomic project saves](0004-atomic-project-save.md) | Accepted; implemented; amended by ADR-0039 and ADR-0091 |
| 0005 | [Delay-compensation transitions](0005-delay-compensation-transitions.md) | Accepted; implemented for the bounded routed graph |
| 0006 | [Media prefetch and adaptive playback processing](0006-prefetch-processing.md) | Accepted; implemented |
| 0007 | [Musical timing and meter representation](0007-timing-representation.md) | Accepted; implemented |
| 0008 | [Atomic project commands and revision publication](0008-project-command-revisions.md) | Accepted; implemented |
| 0009 | [Durable source identity and stereo render context for M2](0009-m2-source-and-render-contract.md) | Accepted; implemented |
| 0010 | [Derived-media identity and stale-result rejection](0010-derived-media-identity.md) | Accepted; implemented, shared source-analysis cache and budgets included |
| 0011 | [Device loss and in-process reselection](0011-device-recovery.md) | Accepted; control contract tested, not used by the app; in-process resume not built |
| 0012 | [Qt modules for the desktop UI](0012-qt-ui-modules.md) | Superseded by ADR-0022; Core/Gui licensing boundary still applies |
| 0013 | [Maintained JSON codec for project manifests](0013-manifest-json-library.md) | Accepted; implemented; current-schema-only reading superseded by ADR-0095 from the first public beta |
| 0014 | [M3 MIDI clips, event timing and sampler boundary](0014-m3-midi-event-contract.md) | Accepted; implemented |
| 0015 | [VST3 SDK baseline for host work](0015-vst3-sdk-baseline.md) | Accepted for the SDK selection; pinned source inventory recorded, compiled-file inventory and host compatibility remain open |
| 0016 | [Encoded audio codec boundary](0016-encoded-audio-codec-boundary.md) | Accepted; implemented (worker decoding, FLAC/MP3 export, resampling) |
| 0017 | [Live VST3 track composition](0017-live-vst3-track-composition-proposal.md) | Superseded by ADR-0050 |
| 0018 | [Bounded routed mixer handoff](0018-routed-mixer-publication.md) | Accepted; implemented |
| 0019 | [Persisted automation identity and preparation](0019-persisted-automation-contract.md) | Accepted; implemented |
| 0020 | [Convolution reverb core and FFT boundary](0020-convolution-reverb.md) | Accepted; offline export only, as an opt-in build; not in release packages |
| 0021 | [Stable send-gain automation binding](0021-send-gain-automation.md) | Implemented; focused live/offline qualification complete |
| 0022 | [Custom Qt Quick UI for the DAW](0022-qt-quick-ui-direction.md) | Accepted for the production UI direction; module and packaging qualification open |
| 0023 | [Unified insert chain and visible buses](0023-unified-insert-chain-and-bus-ui.md) | Accepted; implemented through ADR-0043 and ADR-0050 |
| 0024 | [Live routed source handoff contract](0024-live-source-handoff-contract.md) | Superseded in part by ADR-0093 (MIDI) and ADR-0094 (held handoff) |
| 0025 | [Planned loop processor history](0025-planned-loop-processor-history.md) | Accepted; implemented |
| 0026 | [Continuous warp stream and musical-time warp model](0026-continuous-warp-stream.md) | Accepted; points 4–8 implemented |
| 0027 | [Prism: default native synth and native-synth platform](0027-prism-native-synth.md) | Accepted; implemented |
| 0028 | [Ember: vintage analog synth (second native engine)](0028-ember-vintage-synth.md) | Accepted; implemented; circuit model in ADR-0066 |
| 0029 | [Bounded live mixer-control overlay](0029-live-mixer-control-overlay.md) | Implemented for admitted channel controls and existing send gains |
| 0030 | [Pulse: analog-modelled drum synth (fourth native engine)](0030-pulse-drum-synth.md) | Accepted; implemented |
| 0031 | [Halo: six-operator FM synth (fifth native engine)](0031-halo-fm-synth.md) | Accepted; implemented |
| 0032 | [Torque: multi-model mono bass synth (sixth native engine)](0032-torque-bass-synth.md) | Accepted; implemented |
| 0033 | [Drift: granular texture synth (seventh native engine)](0033-drift-granular-synth.md) | Accepted; implemented |
| 0034 | [Stepped automation for discrete and next-note synth parameters](0034-stepped-native-automation.md) | Accepted; implemented |
| 0035 | [Pedals in MIDI clips](0035-pedals-in-midi-clips.md) | Accepted and implemented |
| 0036 | [Concert: physically modelled grand piano (eighth native engine)](0036-concert-piano.md) | Superseded by ADR-0071 |
| 0037 | [Live synth telemetry for panels](0037-synth-telemetry.md) | Accepted; implemented |
| 0038 | [Live structural switching in native synths](0038-live-structural-switching.md) | Accepted; implemented |
| 0039 | [Background publication of routine editor changes](0039-background-editor-publication.md) | Accepted; implemented |
| 0040 | [A generic native effect platform](0040-native-effect-platform.md) | Accepted; implemented (integration completed under ADR-0043) |
| 0041 | [Pocket, a kick and bass masking resolver](0041-pocket-kick-bass.md) | Accepted; phase 1 built |
| 0042 | [A native utility suite](0042-native-utility-suite.md) | Accepted; all six built |
| 0043 | [One native insert path](0043-one-native-insert-path.md) | Superseded by ADR-0090 for export's link to native_inserts and by ADR-0092 for native_inserts' dependencies; otherwise implemented, classic kinds follow in ADR-0047 |
| 0044 | [Lumen: wavetable synth (third native engine)](0044-lumen-wavetable-synth.md) | Superseded by ADR-0057 |
| 0045 | [The development build](0045-development-build.md) | Accepted and implemented |
| 0046 | [The workspace redesign](0046-workspace-redesign.md) | Accepted and implemented |
| 0047 | [The classic processors become engines](0047-classic-processors-as-engines.md) | Accepted and implemented; convolution remains its own kind |
| 0048 | [Parallel real-time rendering](0048-parallel-audio-rendering.md) | Accepted and implemented |
| 0049 | [Live graph switching and latency handover](0049-live-graph-switching.md) | Accepted and implemented; every chain switches live (ADR-0050); held path removed (ADR-0094) |
| 0050 | [One chain per track](0050-one-chain-per-track.md) | Accepted and implemented; export has two VST3 renderers; superseded in part by ADR-0093 for MIDI while playing |
| 0051 | [Automation drawn on the arrangement](0051-arrangement-automation.md) | Accepted and implemented; inspector forms to retire |
| 0052 | [Constant-tempo and key analysis](0052-constant-tempo-and-key-analysis.md) | Accepted; implemented; downbeat rule in ADR-0053 |
| 0053 | [The clip strip](0053-clip-strip.md) | Accepted; strip, analysis, downbeat, zoom, bars before bar 1 and docked clip editor built; arrangement warp markers and tempo lane not built |
| 0054 | [Live controls, granular UI updates and meters on their own clock](0054-live-controls-and-granular-updates.md) | Accepted; implemented |
| 0055 | [Deck: a DJ mixer's three-band isolator](0055-deck-dj-isolator.md) | Accepted; implemented |
| 0056 | [Snarl: a growl and wobble synth](0056-snarl-growl-synth.md) | Superseded by ADR-0057 |
| 0057 | [Nova: the flagship wavetable synth](0057-nova-flagship-synth.md) | Accepted; platform, engine, panel, FX rack, arp and clip sequencer built; sample-based oscillators and table saving not built |
| 0058 | [Pump: a drawn volume shaper, and documents for effects](0058-pump-volume-shaper.md) | Accepted; platform, engine, MIDI trigger and panel built; superseded by ADR-0097 for how keys follow a mute lane |
| 0059 | [Play and sparkle: note effects, macros and freeze](0059-play-and-sparkle.md) | Superseded by ADR-0099 for what a bounce does with its source's mute lane, and by ADR-0102 for how a render keeps clip channel lanes; otherwise implemented |
| 0060 | [Anvil: a drawn kick synth](0060-anvil-kick-synth.md) | Accepted; engine, presets, preview and panel built |
| 0061 | [Performance tracing](0061-performance-tracing.md) | Accepted; implemented |
| 0062 | [Denormals flushed at every render entry](0062-denormals-flushed.md) | Accepted; implemented |
| 0063 | [Crate: a chop sampler (replaces the sampler)](0063-crate-chop-sampler.md) | Accepted; implemented |
| 0064 | [Temper: a resonance suppressor with dynamic EQ bands](0064-temper-resonance-suppressor.md) | Accepted; engine, presets and panel built |
| 0065 | [Nova FX rack: a signal-flow FX page, a parametric EQ and a shifter](0065-nova-fx-rack.md) | Accepted; implemented |
| 0066 | [Ember's circuit model: a vintage poly that misbehaves like one](0066-ember-circuit-model.md) | Accepted; implemented |
| 0067 | [Flyby: a sound passing the listener, properly](0067-flyby.md) | Accepted; implemented |
| 0068 | [Knock: physically modelled percussion](0068-knock-percussion.md) | Accepted; engine, modal library, kits and panel built |
| 0069 | [Dust: a bed of background texture, and age for what passes through](0069-dust-texture.md) | Accepted; engine, presets and panel built |
| 0071 | [Concert becomes a sampled grand: the Salamander Grand Piano, streamed from disk](0071-concert-sampled.md) | Accepted; implemented |
| 0072 | [Kick & Bass Lock: Anvil and Nova read each other's notes](0072-kick-bass-lock.md) | Accepted; implemented; Align not built |
| 0073 | [Nova Analogue: seeded voice cards, drift and Voice Lanes](0073-nova-analogue.md) | Accepted; implemented |
| 0074 | [Mix Check: offline checks, blame and one-step fixes](0074-mix-check.md) | Accepted; implemented |
| 0075 | [Chop: a tempo-locked gate and stutter](0075-chop.md) | Accepted; implemented |
| 0076 | [Nova panel: resize and reflow, a signal spine, a browser that lets you play](0076-nova-panel-redesign.md) | Accepted; implemented, including revision 2 |
| 0077 | [Nova Constellation: browse presets by sound, morph between them](0077-nova-constellation.md) | Accepted; implemented |
| 0078 | [Nova arp patterns: lanes of their own length, sixteen modes, a library and MUTATE](0078-nova-arp-patterns.md) | Accepted; implemented |
| 0079 | [Groove from a record: one band's feel from a record or clip, for the groove pool](0079-groove-from-a-record.md) | Accepted; implemented |
| 0080 | [Arrange from a loop: a whole track proposed from 8 or 16 bars, kept or binned in one step](0080-arrange-from-a-loop.md) | Accepted; implemented |
| 0081 | [Clay: note editing of audio — notes of partials plus the rest, edits rendered as original − old + new](0081-clay-note-editing.md) | Accepted; implemented |
| 0082 | [Strata: multiband dynamics — one curve per band, up and down, Linkwitz-Riley or linear-phase bands, OTT](0082-strata-multiband.md) | Accepted; implemented |
| 0083 | [Harmony: the Chord track, the Progression Lab and Comp](0083-harmony-chord-track-and-comp.md) | Superseded in part by ADR-0084 |
| 0084 | [Chords are notes in MIDI clips: the chord strip, held-note Comp with pushes seen ahead, Rhythm Apply](0084-chord-clips.md) | Accepted; implemented |
| 0085 | [One heavy synth's voices on the free audio helpers: nested worker batches, Nova voices rendered apart and summed in order](0085-nested-voice-rendering.md) | Accepted; implemented |
| 0086 | [MIDI recording from the computer keyboard where it was heard; a device-only metronome and count-in](0086-midi-recording-and-click.md) | Accepted; implemented |
| 0087 | [Nova voices on the GPU](0087-gpu-nova-voices.md) | Rejected |
| 0088 | [Fret: a sampled bass guitar with Feel, realtime auto-articulation](0088-fret-sampled-bass.md) | Accepted; implemented; not part of Kick & Bass Lock |
| 0089 | [Wren: a sampled concert flute with Breath, realtime phrasing](0089-wren-sampled-flute.md) | Accepted; implemented |
| 0090 | [Export renders a graph the render_graph builder prepares](0090-render-graph-builder.md) | Accepted; implemented for built-in chains |
| 0091 | [New projects are staged beside their folder and renamed into place](0091-staged-project-creation.md) | Accepted; implemented |
| 0092 | [Native inserts read which keys sound from routing](0092-native-inserts-read-key-audibility.md) | Superseded by ADR-0097 for how key notes follow a mute lane; otherwise implemented |
| 0093 | [Live MIDI edits and queued revisions](0093-live-instrument-edits-and-queued-revisions.md) | Accepted; implemented |
| 0094 | [Sources switch at a primed block; no held path](0094-primed-source-switching.md) | Accepted; implemented |
| 0095 | [Projects from every released schema open, through chained forward migrations](0095-schema-migration-policy.md) | Accepted; hook and golden-manifest test built, no schema released yet |
| 0096 | [MIDI clips keep notes trimmed off their left edge](0096-midi-clip-left-trim.md) | Accepted; implemented |
| 0097 | [A key source's mute lane gates its keys](0097-mute-lanes-gate-keys.md) | Superseded by ADR-0099 for what a bounce does with its source's mute lane; otherwise implemented |
| 0099 | [A bounce moves its source's mute lanes to the bounce](0099-bounce-moves-mute-lanes.md) | Superseded by ADR-0102 for where a clip's mute lane goes; otherwise implemented |
| 0102 | [A track render plays in slices that keep its clips' channel lanes](0102-renders-play-in-slices.md) | Accepted; implemented |
