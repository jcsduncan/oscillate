# ADR-0024 — Live routed source handoff contract

**Status:** Implemented for audio-only clip move, trim, fade, split, duplication and deletion in Quick; wider source edits remain open.

**Date:** 17 October 2026.  
**Requirements:** ER-01/08/09/11, TA-05, PM-06.  
**Affected interfaces:** `RoutedSourceFill`, `PrefetchRing`, `RoutedSourceSelection`, `RoutedPlaybackControl`; no project schema change.

## Decision

A clip edit commits to the project model first. If the active graph is
settled and the edit changes only audio clips on existing tracks, the control side
captures the accepted snapshot and source layout. A worker verifies managed
media and builds a replacement `RoutedAudioSource` while the old graph keeps
playing. Instrument-bearing projects and other source edits fail closed until
they have their own state-preserving transition contract.

The worker reads the completed transport position immediately before priming
the candidate rings, after media verification. It records the matching
transport generation. The control side checks the candidate's contiguous
first-block window before requesting a hold, then checks the **actual held
sample and generation** before staging the candidate and publishing the graph.
If either window is missed, the saved edit is reported as inactive and the old
graph resumes. No media I/O, worker wait, allocation or source destruction
occurs in the callback.

The graph publisher passes the revision to a source-fill callback. That
callback selects a preallocated source bank from one immutable
`RoutedSourceSelection` descriptor. A graph-only revision continues to use the
active source; the exact staged revision uses the candidate. The callback
holds a selection lease through mixing. A failed fill does not activate the
pending graph or consume the project cursor. Promotion and cancellation
retire old descriptors only after the callback epoch proves quiescence;
decoder workers join on an off-UI retirement task.

The graph reuses compatible VST3 and native processor owners, so source-only
edits do not reset their state. The UI's revision status distinguishes the
saved model from the active graph throughout preparation and after failure.
The selected source remains tied to the playback run across graph-only edits.

## Evidence and remaining gates

The routed publisher tests cover revision-specific source banks, failed
candidate fill, exact held publication and zero callback allocations. The
Quick stepped-output integration test moves a clip twice, trims it, changes its fade, duplicates it and deletes it while
playing, checks the new audible source without restarting output, verifies a
pause/seek during preparation, and confirms that failed media preparation
leaves the old graph audible with the saved revision marked failed.
Source-selection tests cover callback-epoch retirement and graph-only revision
fallback. ASan/UBSan covers the headless source and publisher contracts.

## No-hold source-only extension

The general source handoff still emits at least one silent device block while
it freezes the transport at a held graph boundary. The isolated move, trim and
exact split cases below avoid that hold at both 64 and 128 frames. The Quick ramp
fixture distinguishes the old and edited source positions and verifies exact
sample timing, uninterrupted transport, no callback allocation, and save/reopen.

For isolated moves, trims or exact splits of managed WAV clips on unchanged audio tracks,
prepare a replacement source bank on a worker as today. Keep the active
source and mixer rendering during preparation. A new publisher operation may
publish a **source-only plan** that shares the active mixer, gain evaluator,
processor owners and delay histories. It has the new project revision but no
new graph topology or DSP runtime. The candidate source selection is staged
before offering that plan. At the callback boundary, select and validate the
candidate's complete block of source frames *before* changing the active plan.
On success, render the new source through the same mixer in that block and
acknowledge the revision. There is no callback hold and no cursor replay.

If the candidate cannot fill that block, leave the old plan and source active,
render the old source at the current project sample, and keep the saved edit
pending. Candidate
source consumption during a failed attempt is confined to the candidate's
own ring. The old source's ring read cursor and its frame tags must remain
untouched until the fallback renders the old source in that *same callback*.
The graph revision and transport cursor likewise remain on the old path until
the candidate's full block passes source-fill and mixer-context validation.
Once published, a later processor fault follows the existing graph-fault
contract; the callback cannot safely roll back mutable DSP history. If even
the old source is unavailable, use the existing underrun contract. Publication
must never acknowledge a candidate before its source block and routing gain
spans are ready. All candidate preparation, memory
allocation, validation and retirement remain off the callback. Source
selection retirement continues to wait for callback quiescence.

The extension is bounded to four direct-to-master audio tracks, sixteen clips
and sixteen source records. A move or trim keeps each clip identity and source;
only placement or source range changes. One split may replace one elapsed-time
clip with exactly two adjacent partitions of its original project and source
frame intervals. The left clip keeps the old ID; the right clip has one new ID.
No other clip changes in that split revision. Both partitions retain the same
managed WAV source and processing settings; neither has fades or automation.
The original and both partitions must have one source frame per project frame.
Their managed WAV must match the project sample rate. Native EQ, compressor,
reverb and limiter chains on tracks and master may remain active and retain
their histories. VST3 effects, convolution, automation, sends, a custom main
route, processing changes, fade changes, musical-time split geometry, active
loops and other pending edits are excluded. Track and source-slot membership,
source identities, route order and processor identities remain unchanged.
Other clip forms and graph changes retain their existing reported handoff or
fail-closed behaviour. A
move or trim can change waveform value abruptly at its edit boundary, so this
contract promises no *inserted silent block*, not universal click-free sound.

The 64/128 ramp fixtures assert `frozen_silent_blocks == 0`, with the completed position increasing by
exactly 64 or 128 samples on *every* permitted callback. Every block must
contain the old ramp until a single exact new-revision block, with no
callback allocation, source underrun or stale sample. A
candidate-not-ready fixture proves old audio continues at the correct
position while the model revision stays pending. The candidate ring is primed
for its original prospective block; after a failed fill it cannot serve a
later block in the same transport generation. The pending edit therefore does
not activate on a later block. Stop and Play applies the saved edit through a
fresh source preparation. A bounded retarget/cancel protocol is future work;
until it exists, control must not mark this published candidate failed because
it could still activate. A native track and master EQ fixture with two tracks
and four clips verifies their filter histories and source-bank order survive
the source-only plan. The saved move/trim/split remains one undoable revision
and reopens at the intended placement or source partition.

Before treating this as the general live-source contract, add acceptance
tests for stale edit supersession, native/VST3 tail continuity, and a
sanitizer/concurrency run of the complete Quick adapter. Source media or track addition/removal, MIDI/sampler changes,
warp/pitch renders and topology changes need separate preparation rules. A
candidate that misses its prospective ring window is prepared again from the
current cursor up to three times, then fails visibly; a miss at the already
held boundary fails immediately to avoid a long silent hold. Playback and
export must continue to reject unsupported source changes rather than
silently rendering stale data.
