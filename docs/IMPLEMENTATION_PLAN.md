# Oscillate — Implementation status and open work

**Inputs:** [PRD](PRD.md), [HLD](HLD.md), [functional specs](specs/README.md),
[decisions](decisions/README.md). This page lists what is built and what is
still open. It is not a schedule. When it disagrees with the code, the code
wins and this page needs fixing.

## 1. How work proceeds

- Build vertical slices that render, save and reopen a real project before
  broadening a feature. Every slice ends with a working application, focused
  tests and a known-limits note.
- A change that crosses a module boundary, a persisted format or a real-time
  contract needs an ADR in [decisions](decisions/README.md) and updates to the
  affected spec and tests.
- DSP starts with a bounded, allocation-free callback design and an
  independently checked reference result; optimisation follows measurement
  ([contributor guidance](../AGENTS.md#dsp-performance-policy)).
- The project format accepts only the current schema (1.47) until the first
  public beta. After that, every schema change needs a migration with
  forward, rollback and interrupted-write fixtures.

## 2. What is built

| Area | State |
| --- | --- |
| Core | C++20 modules for timing, project commands and undo, crash-safe generation saves and recovery, graph publication, PipeWire and JACK output, device-loss handling. |
| Arrangement | Audio import with managed media, MIDI clips with velocity and pedal lanes, chords as notes in MIDI clips ([ADR-0084](decisions/0084-chord-clips.md)), Standard MIDI File import, markers, tempo and meter maps, copy/paste, clip strip and docked clip editor ([ADR-0053](decisions/0053-clip-strip.md)). |
| Recording | MIDI recording from the computer keyboard with overdub, metronome and count-in ([ADR-0086](decisions/0086-midi-recording-and-click.md)). |
| Instruments | Thirteen native engines: Prism, Ember, Nova, Pulse, Halo, Torque, Drift, Anvil, Knock, Crate, and the sampled Concert, Fret and Wren with separately installed libraries. |
| Effects | Twenty-three native effect and utility engines on one insert path shared by live playback and export ([ADR-0043](decisions/0043-one-native-insert-path.md), [ADR-0050](decisions/0050-one-chain-per-track.md)); optional convolution reverb. |
| Mixing | Buses, sends, returns, sidechains, PDC, macros, racks, freeze, note effects ([ADR-0059](decisions/0059-play-and-sparkle.md)), live control edits ([ADR-0054](decisions/0054-live-controls-and-granular-updates.md)), parallel rendering on audio helpers ([ADR-0048](decisions/0048-parallel-audio-rendering.md), [ADR-0085](decisions/0085-nested-voice-rendering.md)). |
| Audio processing | Tempo and key analysis, the in-house warp and pitch engine ([ADR-0026](decisions/0026-continuous-warp-stream.md)), Clay note editing ([ADR-0081](decisions/0081-clay-note-editing.md)), groove extraction ([ADR-0079](decisions/0079-groove-from-a-record.md)), Arrange from a loop ([ADR-0080](decisions/0080-arrange-from-a-loop.md)). |
| Automation and export | Track, clip, send and device automation drawn on the arrangement ([ADR-0051](decisions/0051-arrangement-automation.md)); master and processed-stem export to WAV, and FLAC/MP3 in codec builds; Mix Check ([ADR-0074](decisions/0074-mix-check.md)). |
| Plugins | Out-of-process VST3 scanning, in-process hosting, saved state, placeholders for missing plugins, X11 editor embedding. |
| Diagnostics | Performance tracing ([ADR-0061](decisions/0061-performance-tracing.md)) and the trace report tool. |

## 3. Design and qualification gates

These gates block only the work named in the last column.

| Gate | State | Open condition | Blocks |
| --- | --- | --- | --- |
| G1 timing ([ADR-0007](decisions/0007-timing-representation.md)) | Closed | — | — |
| G2 model ([ADR-0008](decisions/0008-project-command-revisions.md)) | Closed | — | — |
| G3 processor state ([ADR-0001](decisions/0001-processor-runtime-state.md)) | Closed | — | — |
| G4 storage ([ADR-0004](decisions/0004-atomic-project-save.md)) | Closed for the current schema | Migration policy before the first public beta | Public beta |
| G5 media ([ADR-0010](decisions/0010-derived-media-identity.md)) | Mostly closed | Shared source-analysis cache budget integration | Cache UI |
| G6 revision status | Closed | — | — |
| G7 performance and PDC ([ADR-0005](decisions/0005-delay-compensation-transitions.md)) | Open | Reference machine and plugin set; physical-device 32-track run at 64 and 128 frames; final PDC budget (the 8,192-frame cap is provisional); changed-latency VST3 transitions | Performance release gate |
| G8 dependencies | Open | Qt module and package review, MP3 encoder redistribution, AppImage and `.deb` notice and SBOM closure | Distribution |
| G9 publication ([ADR-0002](decisions/0002-graph-publication.md)) | Closed | — | — |

## 4. Open engineering work

1. **Qualification.** Physical-device 32-track workload with backend underrun
   and CPU headroom evidence ([runbook](performance/g7-physical-device-runbook.md));
   recorded-material warp and pitch listening
   ([review](performance/warp-recorded-review.md)); a wider installed-plugin
   compatibility matrix; usability and accessibility testing on realistic
   projects. Prove performance changes with whole-process CPU under light and
   heavy projects, not single-thread benchmarks alone.
2. **Export consolidation.** Bring the remaining VST3 offline renderer
   variants onto the one-chain-per-track builder, with master/stem, tail and
   timing parity fixtures ([ADR-0050](decisions/0050-one-chain-per-track.md)).
3. **Live edits.** Wider source edits through the live source handoff
   ([ADR-0024](decisions/0024-live-source-handoff-contract.md)); VST3
   replacements whose latency changes
   ([ADR-0005](decisions/0005-delay-compensation-transitions.md)).
4. **Feature follow-ups.** Warp markers on the arrangement and the tempo lane
   ([ADR-0053](decisions/0053-clip-strip.md)); retiring the automation
   inspector forms ([ADR-0051](decisions/0051-arrangement-automation.md)); Nova's
   later phases ([spec 08 §13](specs/08-nova-synth.md)); Kick & Bass Lock
   Align ([ADR-0072](decisions/0072-kick-bass-lock.md)) and its Fret follower
   ([ADR-0088](decisions/0088-fret-sampled-bass.md)); Pocket's later phases
   ([ADR-0041](decisions/0041-pocket-kick-bass.md)); convolution graph and
   release integration ([ADR-0020](decisions/0020-convolution-reverb.md));
   Wayland-native VST3 editors.
5. **Code health.** Reduce the large session, model, playback-control and
   bridge files along cohesive command and read-model boundaries
   ([code health](quality/code-health.md)). Do not move audio-thread
   ownership casually.
6. **Release preparation.** Dependency inventory, package/runtime closure,
   notices, diagnostics bundle, support documents and migration policy before
   the first public beta.

## 5. Checks

During feature work, build the changed targets and run the tests for the
affected behaviour. Before a merge that touches several areas, and before any
release, run the broad sweep: GCC and Clang builds, the fast and slow tiers,
clang-format, clang-tidy, ASan/UBSan, TSan where concurrency changed, and
coverage. Commands are in [AGENTS.md](../AGENTS.md#commands). A release needs
evidence for each of the PRD's release gates (PRD §23); coverage percentage
does not waive a blocker.
