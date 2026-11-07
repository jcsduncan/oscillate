# FS-05 — Analysis, warping and processing jobs

**Status:** Draft. **Dependencies:** media identity, tempo map, scheduler, cache store. **PRD:** §§10–12.

## Functional contract

- **PA-01** Import schedules waveform, BPM/confidence, key/compatible-key, onset/transient, beat-grid and warp-marker suggestions in the background. Editing remains available while work is queued or running. Waveform detail increases as the user zooms in; the visible source interval is projected to display columns rather than retaining a fixed number of points for the whole recording. For a seven-minute 48 kHz source, the waveform summary uses at most 128 source frames per base bucket within its bounded 250,000-bucket budget, so a three-bar view retains transient detail. A clip action can explicitly rebuild the waveform from verified source audio, replacing its disposable shared cache entry without altering source media, edits or reviewed analysis corrections.
- **PA-02** Generated suggestions are visually distinct from manual edits. Reanalysis may replace generated results but never manual corrections, applied warp markers, chosen stretch mode or pitch settings.
- Per-source reviewed tempo state stores detected BPM/confidence, an optional user correction, and separate generated/user source-frame beat markers in the project manifest. The correction does not change the global tempo map. A late worker result captured before any intervening project revision is rejected; an accepted reanalysis replaces only generated values and gives a user marker precedence at the same source frame. This state survives cache deletion and reopen. Schema 1.29 adds generated and user-dismissed transient (onset) frames to the same record, under the same capture rule; see the Warp editor below.
- Per-source reviewed key state stores optional generated tonic/mode, confidence and compatible keys separately from optional user correction in schema 1.14. An unknown suggestion is explicit. `ProjectSession` checks the result's semantic cache key, nonzero generation, source rate and frame count, then applies the command against the captured project revision and source SHA-256. Reanalysis preserves the correction, including when it changes the suggestion to unknown. The application maps the worker's C=0..B=11 major/minor key and relative/dominant/subdominant suggestions to the project's independent types. This adapter does not validate estimator quality or imply import scheduling is complete.
- The Quick inspector shows a selected source's saved suggestion, confidence, compatible-key relations and manual correction. Existing review loads without rerunning analysis; users explicitly request reanalysis. A worker result may update the project only while its exact clip/source selection, generation and captured project revision still match. Changing selection or project revision cancels publication. The panel offers keyboard-accessible correction and clear controls, and save/reopen retains the review.
- **PA-03** Users review and atomically apply warp markers. Marker changes preserve other anchors and source order. One application or gesture is one undo step; the source file remains unchanged.
- Schema 1.24 stores per-clip pitch semitones, formant-preservation choice and ordered reviewed warp markers separately from disposable analysis. Pitch intent is finite and currently bounded to -12..12 semitones by the candidate processor. Each marker binds an absolute source frame to a clip-local offset in the clip's placement timebase; endpoints remain implicit. One `SetClipProcessing` command updates these settings and one generation, with undo/redo. Invalid source/target order, bounds, timebase or sample-rounded collisions reject before publication. A trim or placement edit that would invalidate reviewed markers rejects instead of dropping them. Changing nominal project rate rescales elapsed marker offsets alongside elapsed clip placement, preserving time; musical offsets are unchanged. Musical offsets resolve through the shared tempo map at the current rate. This is durable model intent only; UI review and production DSP activation remain open.
- Schema 1.28 adds a per-clip `warp` record (ADR-0026 point 4):
  - an explicit stretch mode (`repitch`, `beats`, `tones`, `texture`, `complex`, `complex_pro`) and a manual-override flag;
  - remembered parameters for Beats (loop off/forward/back-and-forth, envelope 0..100), Tones (grain 5..200 ms), Texture (grain 10..500 ms, flux 0..100) and Complex Pro (formants 0..100, envelope 8..256).
  - **Range and migration.** Pitch widens to −48..48 semitones and renders as `round(100 × semitones)` cents, with half-cent ties away from zero. Clips from 1.24–1.27 reopen with their pitch, formant choice and markers unchanged, mode `complex`, not overridden, and default parameters.
  - **Re-Pitch** is only ever an explicit choice. It ignores transpose, and the UI labels transpose inactive.
  - **Formant preservation** is applied by Complex Pro only, as its formant amount, or 0 when the intent is off. Elsewhere it is labelled inactive.
  - **Quality tier** is not saved. Export and cache renders use the highest tier; live step-down is reported.
  - **Rejection.** Missing, extra, non-finite or out-of-range fields in a 1.28 manifest reject before publication.
  - **Commands.** Mode and parameter edits travel in the same `SetClipProcessing` command: one undo step and one new settings generation.
- A clip renders through the warp engine when its pitch is active, it has reviewed markers, or its placement duration differs from its rate-converted source duration. The warp map is derived from the shared clip frame plan:
  - **Output frames** are clip-local project-rate frames.
  - **Source positions** are Q48.16 positions in a project-rate view of the source. Plan anchors are scaled by the exact rate ratio in integer arithmetic.
- A managed source at another supported rate is presented to the stream through an in-house, stateless, exact-rational polyphase sinc converter. An unprocessed-equivalent clip at any 44.1/48/88.2/96 kHz source/project pair therefore keeps its elapsed duration and pitch in every mode, without depending on the optional libsamplerate build. Externally linked sources are refused for processing with an actionable status until linked-source worker verification exists.
- The session exposes that command as one undoable clip edit.
- **Split, slip and trim of processed clips** (warped, stretched or pitched; `application::rewindow_processed_clip`). The clip's audio is its warp map: clip start, reviewed markers and clip end pin source frames to positions on the clip's own timeline (ticks for musical clips, samples for elapsed), linear in project samples between them and extended beyond the clip by its first and last segment's slope. Each edit re-windows that map as one undoable revision; markers stay glued to their audio, as in Ableton Live, and mode, transpose, formants and remembered settings are kept.
  - **Split** gives each marker to the half that contains it, rebased to that half's start. A cut exactly on a marker uses it as both halves' shared edge, so the map is unchanged; a cut between markers takes the map's source frame there, rounded to the nearest frame.
  - **Trim** moves an edge along the map (outward by the edge segment's slope), so the remaining audio keeps its timing. **Slip** keeps the clip on the timeline and moves its audio and markers together by the timeline span the source delta covers at the clip start.
  - A trim or slip that would leave a reviewed marker outside the clip, or the window outside the source, rejects without changing the project; markers are never silently dropped. A processed clip whose source is missing cannot be re-windowed.
  - Playback and export read the same frame plan, so a split half plays and exports the same frames as the whole: Re-Pitch exactly, Beats except from the cut to the next preserve point plus one 2 ms slice fade (the cut starts a new slice). Phase-vocoder and granular modes restart their stream at the cut, as any clip start does.
- **Slicing** a sample into chops and Chop to MIDI belong to Crate ([spec 09](09-crate-sampler.md) §§3–4); slicing an arrangement audio clip into zones is not implemented.

Routed live playback renders saved processed clips in all six modes (ADR-0026 point 8):
- It prefers a validated Q0 cache render and otherwise renders live on the prefetch worker.
- Seeks and planned loop wraps are primed by generation.
- Underruns are reported, quality step-downs counted, and per-clip tier is reported for the prepared source's own revision.

The Quick Inspector's Warp editor edits the selected audio clip's saved processing; every change is one `SetClipProcessing` undo step that saves and reopens:
- **Mode:** all six modes, with the manual-override flag.
- **Transpose:** semitones plus cents, kept but labelled inactive in Re-Pitch.
- **Mode settings:** each mode's settings, remembered across mode switches.
- **Formants:** Complex Pro formant preservation, amount and envelope, labelled inactive elsewhere.

Its marker lane shows reviewed warp markers over the warp-mapped waveform and the quarter-note grid:
- **Adding:** a marker pins the source frame already playing at its position, so the sound does not change.
- **Moving:** dragging or Alt+arrow keys move a marker between its neighbours; a drag saves once, on release.
- **Deleting:** Delete removes the selected marker.

Onset suggestions are durable review state (schema 1.29, on the source's tempo review):
- A cancellable worker runs the warp transient detector on the whole verified source. Its result is stored as the source's generated onsets (bounded to 20,000, with a truncation flag) only if the project revision it captured is still current; otherwise it is reported stale and nothing is stored. Storing is one undoable revision.
- The clip shows the generated onsets inside its trimmed range, minus any within 30 ms of a reviewed marker, as hollow suggestions.
- **Apply** adds them as markers in one atomic edit without moving audio. **Dismiss** (one, or all shown) saves the frames as user corrections: a generated onset within 64 source frames of a dismissal is removed, and reanalysis never brings it back. Clearing a dismissal does not restore an old suggestion; reanalyse to refresh.
- Reanalysis replaces generated onsets only. It never changes dismissals, applied markers, the stretch mode or pitch settings (PA-02).

Auto-warp and tempo conform are reviewed proposals for musical clips, from the source's tempo review (the user's BPM correction, else the detection; user and generated beats). If the source has no stored review, the tempo panel's verified analysis is stored first as its own revision. Nothing is stored for a clip that cannot be proposed (an elapsed clip).
- **Auto-warp** puts each reviewed beat inside the clip on the project's quarter-note grid: the first beat snaps to the nearest grid beat from where it plays now (the next one if that is the clip start), each later beat one quarter note after the previous. The clip end follows the last beat interval. Reviewed markers are fixed anchors: they are never moved or removed, a beat within 30 ms of one or whose grid position would cross one is skipped (and counted), and the clip length is kept when a reviewed marker follows the last beat.
- **Conform** stretches the clip uniformly so its reviewed BPM plays at the project tempo map: the clip keeps its start, its length becomes the source range's beats at that BPM, and reviewed marker offsets scale with it.
- The lane previews the proposal's markers; **Apply** is one undoable edit of placement and processing, refused if the clip's settings generation or the project revision changed since the proposal. Neither changes the project tempo map.

In the arrangement, a processed clip's waveform is drawn as it plays: the visible part of the clip is split into equal timeline columns, each converted through the meter and tempo maps and the clip frame plan to the source interval it plays, and the shared waveform summary is read at the level matching the average column width. Markers, stretch and tempo changes therefore show at any zoom, with work bounded by the column count (at most 4,096) and the visible source buckets. Unprocessed clips keep the linear source-range view.

While playing, the editor states:
- whether the saved revision is audible, pending (naming the revision still heard) or failed;
- the selected clip's cache or live backing, its tier, and how many times it stepped down.

Saved warp edits take the routed source handoff. A project whose only processed content is warp or pitch starts routed playback; the legacy path never plays it raw. If the legacy path is already playing, the editor says the edit is not audible until Stop and Play.

Loop playback (toolbar, whole bars) is routed-path only:
- The control thread arms `RoutedAudioSource::set_loop` and services it on every poll (`RoutedPlayback::service_loop`).
- It schedules `TransportCursor::request_loop_wrap_at(end, start)` only when `loop_status()` reports the next generation primed. The transport splits the device block at the loop end, so the wrap is sample-exact at any bar and device period.
- Changing the range or disabling the loop withdraws a queued wrap (`cancel_loop_wrap`). Frames already queued for that pass are discarded by an in-place transport restart (a short priming hold at the current position). A range armed after its clips were streamed past its end is re-primed the same way while the playhead is before the end.
- A wrap queued after the in-flight block already passed the loop end applies at the next block start; it is counted and shown as a late wrap.
- If playback still crosses the loop end unprimed, the loop restarts by seek, and the status says so.
- Arming a loop the playhead is already past does not jump; playback continues.

Offline export also renders saved processed clips in all six modes, at Q0 from the verified managed source:
- master WAV/encoded export;
- processed stems;
- the VST3 effect and mixed instrument variants, within each variant's existing source, track and plugin limits.

Fades, routing, sends, PDC and effect tails apply as for raw clips. A changed or missing source fails without publishing.

The legacy direct clip and instrument playback paths still reject unrendered pitch/marker intent rather than silently using original audio.
- **PA-04** Rhythmic/melodic/complex classification chooses a default stretch mode with confidence. A new clip uses a verified classification when available; otherwise it starts at Complex. A later classification may automatically change a non-overridden mode only through a visible, undoable command bound to the exact source identity, clip settings generation and project revision. An overridden mode never changes on reanalysis. Vocal or other formant-sensitive classification selects Complex Pro without overwriting the clip's formant choice. Users can override the mode per clip. Pitch and stretch settings belong to clip instances; extreme ratios/shifts warn about likely quality degradation without forbidding the operation.
- **PA-05** Vocal/formant-sensitive material defaults to formant preservation with a per-clip control. Combined stretch/pitch processing is preferred where supported; a sequential fallback is labelled. Cache-miss adaptive playback processing runs in a media prefetch worker and fills the same ring as ordinary playback, never the callback. It steps down to a documented quality mode when behind; if that cannot meet demand, ER-11's reported silence/underrun policy applies. Real-time and offline paths preserve requested duration and pitch semantics.
- **PA-06** Queue priority is approaching playback, visible/edited material, then speculative work. Latest settings supersede old generations; stale results never install. Users can cancel jobs and pause/resume non-essential work.
- **PA-07** Cache keys include source version, project nominal sample rate, instance settings, algorithm/version, quality and dependent tempo state only when each input affects the artifact. Source-only waveform, tempo, key and onset summaries use a shared per-user content-addressed cache; clip-instance processing remains project-local. A project verifies its managed or linked source before using a shared hit. The manifest persists accepted generated reviews and manual corrections independently, so another project's analysis cache cannot silently replace user choices. Processed clip renders are keyed as ADR-0026 point 7 defines: source identity, project rate, clip and settings generation, canonical map, resolved mode and applied parameters, tier, per-mode algorithm version and rate-adapter version. The LDW2 payload header is validated before any seek read. Missing/invalid cache is regenerated; priority prefetch processing can supply playback while it rebuilds. A project-rate change invalidates rate-dependent cache and prefetch generations. Stale worker output is generation-checked before entering the ring. Eviction removes only reconstructable files. Project state, managed media, links and user renders survive clear-cache. The final global byte budget counts the shared store once plus reachable registered project caches; moved or unreachable projects are omitted without deleting their data. Equal-recency eviction uses stable registry order, then entry name.
- **PA-08** Per-clip status and a detailed panel distinguish queued, running, completed, cancelled, failed, stale and unavailable, with reasons for throttling and failures. No completion-time promise is required.

## Component contract

Jobs have an immutable input descriptor, generation ID, cancellation token and result validation step. Worker concurrency is capped by user limit and a non-RT headroom signal; the callback never asks the scheduler to wait. Cache writes are temporary then atomically installed after hash and generation validation. A cache index is reconstructable by scanning validated entries. Analysis confidence and provenance are serialized separately from manual choices.

Long derived audio can be opened through a worker-only validated cache read handle. It checks the complete entry header, exact length and SHA-256 before exposing payload bytes; callers then read bounded chunks at payload offsets. The handle pins the immutable inode through managed clear, eviction and replacement without holding cache locks while workers read. Source identity verification and prefetch generation validation remain caller responsibilities. Cache files modified directly outside the store are unsupported, and this API is never used by the audio callback.

The algorithm-selection decision is a licence and quality gate before D1 begins, covering permissive, reciprocal, commercial and justified in-house paths. It uses measured quality fixtures: percussive transients, sustained tones, vocals, mixed music, extreme ratios and combined shifts. Compare CPU at 64/128 buffers and offline output, plus licensing and packaging obligations. No algorithm is accepted on subjective preference alone.

## Acceptance evidence

| ID | Evidence |
| --- | --- |
| PA-01/02 | Import fixtures with known tempo/key/onsets, low-confidence cases, reanalysis after manual correction and save/reopen; schema 1.29 onset-review round-trip, 1.28 migration and field rejection, stale-capture rejection, dismissals surviving reanalysis. Quality tolerances are approved with the fixture set. |
| PA-03–05 | Schema 1.28 round-trip and 1.24–1.27 migration with undo/reopen equality; every source/project rate pair keeping identity duration and pitch per mode; converter passband/stopband/seek oracles; warp/pitch duration and pitch measurements, anchor invariants, undo/redo, extreme-ratio warnings and listening review with recorded outcomes. |
| PA-06/07 | Deterministic fake-worker scheduling tests, rapid setting changes, cancellation races, interrupted writes, LRU eviction, cache deletion/reopen, source-media preservation, quality step-down and ring underrun reporting. Two projects containing identical media must share a source-analysis hit while retaining separate corrections; changed bytes, algorithm versions and corrupt entries must miss. A missing or damaged source must not be masked by a shared hit. |
| PA-08 | UI state tests for every job state and accessible non-colour indicators. |

Benchmark tests use stable fixtures and report distributions. Do not use long sleeps or wall-clock timing as the primary oracle for scheduler unit tests.
