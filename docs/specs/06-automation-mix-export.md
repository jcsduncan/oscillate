# FS-06 — Automation, mixing and export

**Status:** Draft. **Dependencies:** timeline evaluator, engine graph/PDC, processor API, media writers. **PRD:** §§14–17.

## Functional contract

- **AM-01** Track automation and clip envelopes can target supported mixer, native processor and plugin parameters. Lanes are nested, resizable, collapsible, searchable and visibly show the owning parameter and timebase. Switching a lane between musical and elapsed time preserves each breakpoint's current absolute sample position, stable ID, value and curve settings. Clip-owned points retain clip-local coordinates, with the clip start as the conversion anchor. The edit is one undo step and persists across save/reopen; unrepresentable exact conversions and target-coordinate collisions reject without changing the lane. Interpolation follows the chosen timebase between breakpoints, and later tempo edits follow that timebase's anchor rules.
- **AM-02** Curves support step/hold, linear, quadratic, exponential, ease-in/out and custom Bézier segments, with valid parameter-domain mapping. Discrete parameters use step/hold. Numeric edits, snap toggle, multi-point selection, move/scale and copy/paste retain shape where valid and warn on semantic incompatibility.

The project session can replace a saved segment's shape and Bézier handles by stable lane ID and starting point ID. The command applies to track and clip lanes, preserves unrelated points and lane identity, and publishes one undoable revision. A final or missing point, invalid shape or handles, or a non-hold shape on a discrete domain rejects without changing the project. Repeating an identical curve is a no-op. Handles remain stored when a segment is temporarily switched to another shape, so returning to Bézier restores them. The Quick curve controls are a separate UI task.
- **AM-03** The active clip envelope owns a parameter while its clip is active. At release, the parameter resumes the track automation curve's value at that time; de-click smoothing may affect the transition but not event timing. There is no implicit sum or blend.
- **AM-04** Automation edits and linked clip edits are undoable. By default clip envelopes travel with a moved clip and track automation stays fixed. A move that crosses track automation points or regions and has multiple plausible outcomes prompts for a choice; the user may remember that choice. Collapsing a lane cannot delete its data.
- **AM-05** Mixer supports channel levels, pan, inserts, groups, buses, sends, returns and sidechains. Signal-flow UI shows the accepted project revision and active routing revision separately, including pending or failed changes. Peak/RMS, clipping, spectrum, EBU R128 / ITU-R BS.1770 integrated, short-term and momentary loudness, and true peak are available without blocking audio.

The current Quick master-output spectrum path copies bounded 2,048-frame stereo windows after the final master gain into an SPSC tap on the callback. Its worker computes a spectrum and the master EQ panel projects 48 logarithmic bands for visual feedback. Seek, loop, stop and restart invalidate older windows by transport generation and epoch. Full tap queues drop a window without delaying or altering audio. The plot is a current-output visual aid, not a persistent analysis result or a calibrated mastering meter. Broader AM-05 loudness, true-peak and spectrum qualification remain open.

The native compressor's linked detector can use a first-order high-pass from
20–2,000 Hz; 0 bypasses the filter. It filters the detector input, not the
audible signal, and preserves the filter state across callback blocks and
compatible graph edits. The frequency is saved per insert in schema 1.22;
older projects load with the filter bypassed. Schema 1.23 adds a saved
track-to-native-compressor sidechain send. Its detector has independent gain
and PDC-aligned input; live and offline master processing use the same native
compressor path. A live target-slot change needs playback restart, and the
previous active graph remains visible if the new graph cannot be applied.
Processed stems with native external detectors preserve the detector signal
while excluding its audible contribution from the target stem; focused tests
cover direct and bus detector routes, detector latency, target tails, repeat
renders and interrupted publication. A bounded VST3 effect path now accepts
one direct audio-track detector send when the saved target slot matches an
effect with an activated stereo auxiliary input. Its offline master and
selected wet stem preserve the detector input; the wet stem sends the detector
track's audible output to a private muted sink. Missing or incompatible
auxiliary buses fail before replacing the export target. Broader VST3
sidechain layouts remain unsupported.

The pinned master path has a persistent output level. Its gain follows all master inserts, including the limiter, and precedes output metering and final render writes. Master and processed-stem renders use the same saved value. A level edit is one undoable routing revision; UI status must distinguish its saved revision from the active live graph until publication completes.

Solo is a saved track control. With no solo active, only explicit mute silences a path. With one or more solos active, rendering retains each soloed track, its upstream contributors and downstream route to the master, including sends and returns; other track paths are silent. An explicit mute still silences its track even when soloed. Live graph publication and offline master/stem export use the same effective-mute projection. A failed live graph edit must continue to expose the saved-versus-active revision status.

A channel-mute automation lane drives its track's mute in place of the saved mute switch. A track's sidechain keys sound only where the track can be heard ([ADR-0092](../decisions/0092-native-inserts-read-key-audibility.md), [ADR-0097](../decisions/0097-mute-lanes-gate-keys.md)). A solo that keeps the keyed track keeps its key. Where a mute lane mutes a key source, its key notes are dropped from the lane's step and its audio key fades with the channel's mute glide, delayed as the key's audio is. Live playback and export apply the same lane.

Track `level` is a linear post-insert gain in 0..4. Track `pan` is stereo balance in -1..1: center retains unity on both channels, positive balance attenuates left by `1-pan`, and negative balance attenuates right by `1+pan`. Both apply to the track's post-insert output before main and send routing, and explicit mute still takes precedence. A stopped export uses the saved value throughout. A qualified, revision-independent, same-topology live edit can publish its complete mute/solo/level/pan control projection at a callback boundary without holding playback; other edits retain the held-graph or reported inactive path. The no-hold projection refuses a changed node with channel-level automation until its precedence is qualified. The Quick live mixer ramps a changed static node gain to its saved value over 128 samples; a later graph switch starts from the last audible gain, while an unrelated switch continues the current ramp. The UI commits one static edit on release or keyboard action. A channel-level automation lane evaluates one value per sample in the shared live/offline route-gain preparation and takes precedence over static level while active. Pan balance and mute still apply. Lanes are edited on the arrangement ([ADR-0051](../decisions/0051-arrangement-automation.md)); the project model bounds each lane at 4,096 points. Continuous fader recording and a transition ramp when enabling or removing a lane remain later AM-01/AM-05 work.
An audio clip can own a channel-level envelope with clip-local musical or elapsed coordinates. New points are placed at a fractional position inside the clip in Arrangement View, which supports clips shorter than one bar. A timebase switch preserves their absolute sample positions. The active clip envelope takes precedence over the track lane, then releases back to the track curve. The Quick arrangement has an explicit level-edit mode for placing and dragging points, with one undo step per completed gesture and stable point identities. Enter/Space places a point at the clip centre; focused points support arrow-key position/level changes and Delete. The envelope's tools take the selected point's exact level in dB, switch the timebase and clear the envelope, each one undo step.

Splitting an audio clip splits each clip-owned lane in the same atomic project revision. The left half retains its lane and existing point identities; the right half gets fresh IDs and rebased local coordinates. Hold and linear segments crossing the cut preserve their value at the boundary. Quadratic and ease-in/out segments are represented as mathematically equivalent Bézier subcurves, and custom Bézier segments use de Casteljau subdivision. Both halves follow the lane's timebase even when it differs from the clip placement. Undo/save/reopen restore both halves. A cut inside a changing exponential segment rejects before mutation: its fixed-curvature representation cannot encode the two restricted curves exactly. Constant exponential segments can split as holds. Supporting a changing exponential cut requires a separately reviewed, persisted curvature parameter; split must not approximate or discard automation.

A bounded VST3 effect parameter path prepares continuous normalized track
lanes and clip envelopes through the shared evaluator. The active clip overrides
its track lane, then the track value resumes at release. Instrument sources
also accept continuous normalized parameter lanes beside MIDI events. Host,
effect and instrument runtime capacities come from one preparation budget:
automated lanes × maximum block frames + one manual-event slot, capped at 4,096
events per process call. An over-budget project fails preparation explicitly.
Synthetic instrument and effect fixtures verify eight fully changing lanes at
64 and 128 frames, exact offsets, manual headroom and zero callback allocations.
Unsupported, read-only
and discrete parameters fail before graph publication or export output
publication. Curve and clip-placement semantics contribute to the runtime
compatibility key. A changed lane during playback preserves the old render
graph and reports the saved revision as unapplied until restart; a routing-only
edit can reuse the plugin and unchanged curve. Bounded VST3 audio-effect master
and processed-stem export use the same evaluator. Active curve replacement and
broader plugin-lane editing remain open AM-01/AM-03 work; synthetic capacity
tests do not qualify third-party plugins.

A project with VST3 instruments or effects exports through the native offline
render with the plugins as chain stages, so it gets every native output format,
rate, loudness report and stem rule
([VST3 offline export](../export/vst3-offline-export.md),
[VST3 output formats](../export/vst3-output-formats.md)). Encoded VST3 stems and
real-time VST3 export remain open.

- **AM-06** Offline and real-time export produce a master in WAV 16/24-bit PCM or 32-bit float, FLAC or MP3, at selected sample rate. Output-rate conversion runs in the export worker, outside the audio callback; original media is unchanged. Integer bit-depth reduction uses TPDF dither by default. Processed stems include inserts, routing effects and tails; exact summation to master is not promised.
- **AM-07** Playback and export use identical tempo, meter, curve, routing and PDC semantics at the same project revision. Offline quality may be higher; any persistent processing limitation is reported. An export of a project revision whose live graph failed must explicitly build that revision or fail with its diagnostic; it cannot silently render the older active graph. Any xrun during real-time export fails the render and records the event. Export failure never replaces an existing valid file.
- **AM-08** Export records target, format, sample rate, bit depth, revision, effect-tail policy, processing mode and warnings. Repeat renders meet a defined tolerance for deterministic processors; nondeterministic plugins are identified in reports.
- **AM-09** Audio, instrument, group, return and master paths present one ordered audio-effect insert-chain workflow. An instrument generator is a typed source before that chain and cannot be reordered among effects. A chain item has stable identity, kind, enabled/bypass state, parameter/opaque state and automation references. Native processors appear as built-in chain items alongside VST3 effects; they are not required to be VST3 binaries. Insert, remove, reorder, bypass and copy/paste are atomic undoable edits, preserve item identity where appropriate, and survive save/reopen. Routing/PDC and export follow the saved order or explicitly reject an unsupported chain without claiming it is active.
- **AM-10** Every audio, instrument, group and return track has an undoable saved mute state. Mute silences the track's post-insert output into all main and send destinations, including when a send gain is automated; it does not erase clips, routes or processor state. The logical master is always present and cannot be deleted. Deleting a track is one undoable edit. A referenced bus cannot be deleted until its incoming routes are removed or reassigned; the UI reports the reason. A saved mute or deletion that cannot apply to an active graph is visibly pending until playback restarts.

## Component contract

Automation evaluator takes immutable curve, parameter descriptor, timebase and sample interval; returns timestamped values in the parameter's domain. Conversion and interpolation have one implementation for engine and offline render. Parameter-specific smoothing is a separate stage. Copy/paste converts through normalized and physical ranges only when semantic compatibility is known; otherwise it requires confirmation or rejects.

Persisted lane identity, descriptor snapshots, clip-local coordinates, bounds and schema
migration follow [ADR-0019](../decisions/0019-persisted-automation-contract.md). A clip
envelope stores its points relative to its clip and travels on a move; the control
thread prepares absolute coordinates for the shared evaluator. Trimming retains
out-of-range points for later extension. An unresolved plugin or parameter keeps its
lane and values, with an explicit inactive status. Live playback and export may not
silently omit a saved lane; they must render the requested revision with the same
evaluator or fail/report that revision as not applied. The current curve core alone
does not satisfy AM-01–04.

The proposed [unified insert contract](../decisions/0023-unified-insert-chain-and-bus-ui.md)
gives each track, group, return and logical master path one saved slot order.
Move and bypass preserve slot and plugin identities and their automation
targets. Explicit removal deletes the slot and attached automation in one
undoable transaction, after confirmation when automation is nonempty; a
missing processor retains both. The chain UI must distinguish saved intent
from the active graph revision and show the reason for a failed publication.
Schema migration, native parameter targets and master-owned automation need
their own acceptance fixtures before editable chain controls are enabled.

Export renders to a temporary output and publishes by atomic rename after writer close and validation. Tail policy has a maximum duration and silence threshold fixed in the export contract before implementation. Stems are generated from explicit graph tap points, documented in the UI. Format encoders and their licenses are approved before packaging.

An opt-in offline integrated-loudness report may tap only the final emitted sample stream for its stated format, rate and channel layout. A failed export cannot publish a report; a successful report records the number of measured output frames and uses an empty value for insufficient duration or gated silence. A report derived before encoding, dither or rate conversion must not be labelled as a measurement of the published file. The current [float32 WAV master/stem slice](../export/loudness-report.md) supports all four project output rates but remains narrower than the final AM-05 and AM-08 reporting requirements.

The first offline WAV master slice accepts one of two explicit control-side authorities: an active live graph status matching the requested snapshot revision, or an exclusive stopped-transport lease acquired on the project control sequencer and held through the entire render. The stopped path builds and validates the requested snapshot independently; it does not assert that a live graph is active. The same source, routing, format and atomic-publication checks apply to both paths. The transport gate must prevent playback start while the lease exists. The desktop session's current always-stopped gate is valid only while desktop playback is unavailable; desktop playback must use a real gate before it can call this path. A stale lease or a failed offline preflight cannot replace an existing master.

### Mix Check

Mix Check ([ADR-0074](../decisions/0074-mix-check.md)) renders a pinned snapshot's master and each audible track offline with the export renderers, on a worker, while the user keeps working; it never changes the project by itself. Its master render takes a lease from a gate of its own, like a track render, because it never claims a live graph and publishes nothing outside a private scratch directory, which is removed afterwards. Each finding names the track(s) to blame, its band, the measured amount and one fix. Applying a fix is one undo step and is refused once the mix differs from the measured one (solo changes excepted). A re-check renders only tracks whose render inputs changed and reports both the target's change and any band elsewhere that moved by more than 0.1 dB. Each check's summary is kept in the project's disposable `cache/mix-check/` directory, never in saved state, and the next check lists what is fixed, better, worse, new or the same.

## Acceptance evidence

| ID | Evidence |
| --- | --- |
| AM-01–04 | Curve boundary fixtures, parameter-range and discrete tests, copy/paste semantics, clip/track handoff sample positions, exact schema 1.9 and 1.8 migration fixtures, missing-plugin retention, clip-local move/trim, undo/redo and save/reopen; UI lane and accessibility tests. |
| AM-05 | Synthetic impulse and metering fixtures; graph/UI revision display tests; mixer workflow usability test. |
| AM-06–08 | Decode master and processed stems to verify routing, format, rate, bit depth, duration, TPDF dither, effect inclusion and complete tails; compare playback/offline event traces, repeat renders within approved tolerances, reject silent export of an older active graph, and inject a real-time xrun to prove failed-render reporting and old-file preservation. |
| AM-09 | Chain order/bypass impulse fixtures for native and VST3 items on track, bus and master; stable slot and VST3 automation IDs across move, undo and save/reopen; old-schema deterministic migration and interrupted-upgrade recovery; removal with attached automation and missing-processor retention; drag/drop and keyboard parity; latency/tail continuity and old-graph preservation on failed publication. |
| Mix Check | `analysis_mix_check` (each measurement on planted signals with hand-derived values), `application_mix_check_*` (rumble, click, mono and ringing found, blamed and fixed in one undo step with a re-check; stable ids; history and targets) and `quick_mix_check_controller_contract`. |
| AM-10 | Mute and deletion command undo, schema 1.16 round-trip and 1.15 migration, routed callback allocation, automated-send and bus mute output, processed export sample oracles, missing/referenced track rejection, Quick keyboard controls, and visible saved-versus-active status. |

The first AM-10 development microbenchmark ran on Intel Core i7-10700K with GCC 15.2.0 Release at 48 kHz. With 32 unmuted sources and 20,000 measured blocks after warm-up, the routed mixer averaged 16.48 µs per 64-frame block and 32.47 µs per 128-frame block, with no synthetic deadline misses. Other builds were active on the host, and this unmuted-path measurement neither isolates mute overhead nor qualifies physical-device xruns. The muted callback allocation fixture observed zero allocations.

Do not assert bit-identical output for external plugins unless the plugin is proven deterministic. Use controlled fixture processors for exact semantic tests.
