# ADR-0026 — Continuous warp stream and musical-time warp model

**Status:** Accepted for the schema 1.28 production contract (points 4–8), which is implemented: per-clip warp mode and parameters, source-rate conversion and the LDW2 cache, served by the in-house `warp_dsp` engine. Per-mode quality and CPU evidence is recorded in the implementation history. **Date:** 18 October 2026 (status reviewed 20 October 2026).
**Requirements:** PRD §11–12; FS-01 TA-03/TA-05; FS-02 ER-05/ER-11; FS-05 PA-01–PA-08. Amends ADR-0006 (ring tag semantics) and supersedes the Signalsmith candidate decisions in `docs/performance/warp-continuous-decision.md`.

## Context

The experimental warp renderer ran an external stretcher once per marker segment and cropped its latency afterwards. The recorded evidence shows why that cannot ship:
- seam jumps of 0.35 against a 0.004 source jump;
- markers 704 frames late, or −457 to +70 frames with latency scheduling;
- final-frame attacks moved by about 20 frames;
- short segments rejected;
- a usable range of only 0.5–3×.

The product requires Ableton Live–class warping: Beats, Tones, Texture, Re-Pitch, Complex and Complex Pro modes, warp markers, auto-warp, transpose with formants, and slicing. It also requires the same marker, duration and pitch semantics in live playback and export.

## Decision

1. **In-house engines in `warp_dsp`.**
   - Every mode is implemented from published methods in a new `oscillate_warp_dsp` library.
   - External stretchers are used only as black-box binaries in an offline comparison screen. They are never linked into a shipped target. Rubber Band (GPL) and Bungee (MPL) sources are not read.
2. **Integer time map.**
   - A `WarpTimeMap` is a contiguous list of output-frame segments, each mapping linearly to source positions in Q48.16 fixed point.
   - Evaluation is exact integer arithmetic: floor of `(t−t0)·ΔS/ΔT`, computed as quotient plus remainder so it cannot overflow. The result does not depend on block partitioning.
   - A segment may start with a splice, a source discontinuity used for loop joins and slices. Within a segment, source never runs backwards.
   - The control side builds the map from clip warp markers through the shared tempo-map evaluator (TA-03).
3. **Continuous stream with an absolute output clock.**
   - A `WarpStream` is created with all of its memory. `start(map, source, output_frame)` seeks, and `render()` returns exactly the frames from `next_output_frame()` onwards without allocating.
   - The stream has no output latency to crop. Any analysis lookahead is internal pre-roll and worker lead time.
   - Processor state is never reset at a marker.
   - A continuous run is independent of render partitioning. Live playback and export can be compared bit-for-bit only with the same source, map, settings, quality tier, dispatch level and start frame. Different starts may produce different phase/grain detail.
   - Stateless modes (Re-Pitch, Beats) are also bit-identical after any seek.
   - Tones is bit-identical after an arbitrary seek when its map-specific grain checkpoint index has been prepared off-thread. The legacy unindexed worker path remains exact but can take time proportional to a long run.
   - Phase-vocoder modes reset phase at deterministic epochs: clip start, end zone, onsets and loop splices. A seek pre-rolls from the latest epoch, bounded at about 0.7 s, so it is bit-identical when an epoch is that close. Otherwise it converges to the same timing and level but not the same phase detail. Forced periodic phase resets were rejected because they would be audible in normal playback.
4. **Persisted clip intent (production contract, schema 1.28).**
   - The schema 1.24 `ClipProcessing` shape stays the base: pitch, formant intent and reviewed markers in the clip's placement timebase. The musical-tick-only marker model, warp on/off flag and beat-region fields proposed in the first draft of this ADR are **withdrawn** for this stage. Placement already supplies the musical or elapsed timebase, and normal clip-edge trim keeps its meaning.
   - Schema 1.28 adds a `warp` record to `processing`:
     - `mode`: one of `repitch`, `beats`, `tones`, `texture`, `complex`, `complex_pro`. It is always stored explicitly, so a saved project never changes sound because a classifier or default changed.
     - `mode_overridden`: true once the user picks a mode. A verified classifier result may automatically select a mode for a non-overridden clip through one visible, undoable project command, provided the captured source identity, clip settings generation and project revision still match. The UI distinguishes the saved choice from the active render until publication. Reanalysis never changes an overridden mode.
     - Per-mode parameters, all remembered while another mode is selected:
       - `beats`: `loop` (`off`, `forward`, `back_and_forth`) and `envelope` 0..100;
       - `tones`: `grain_ms` 5..200;
       - `texture`: `grain_ms` 10..500 and `flux` 0..100;
       - `complex_pro`: `formants` 0..100 and `envelope` 8..256.
   - `pitch_semitones` stays the one transpose value. Its bound widens to −48..48. The engine renders `round(100 × pitch_semitones)` cents, with exact half-cent ties away from zero; the stored value is unchanged. Thus 1.24–1.27 values reopen exactly and the audible quantisation is at most half a cent.
   - **Default mode.** A new clip uses an existing verified source classification when available; otherwise it starts at `complex`, not overridden. Every clip migrated from 1.24–1.27 starts at `complex`, not overridden. Default parameters are Beats forward/100, Tones 40, Texture 100/0 and Complex Pro 100/128. A subsequent verified classification may select a better mode under the guarded command above; vocal or other formant-sensitive material selects Complex Pro while retaining the saved formant choice. Re-Pitch is never an automatic default; it is the explicit varispeed choice.
   - **Formant intent.** `preserve_formants` remains the user-facing intent. It is applied by Complex Pro only: the stream's formant amount is `complex_pro.formants` when the intent is set and 0 otherwise. Other modes do not apply it, and the UI labels it inactive there rather than implying it is audible. Re-Pitch has no independent transpose: `pitch_semitones` is kept but inactive and labelled so.
   - **Quality tier is runtime policy, not project state.** Export and cache renders use Q0. Live playback starts at Q0 and may step down to Q1, then Q2, at a recorded hop boundary when a worker falls behind; every step-down is reported (PA-05/PA-08).
   - **When processing applies.** A clip is rendered through a warp stream when its pitch is non-zero (outside Re-Pitch), it has reviewed markers, or its placement duration differs from its rate-converted source duration. Otherwise the ordinary source path plays it and the mode is irrelevant to the sound.
   - `SetClipProcessing` stays the single command: one validated, atomic, undoable revision that bumps the clip's settings generation. A project-rate change rescales nothing new, because every new field is rate-independent.
5. **Map and output-frame coordinates.**
   - The only timing evaluator is `processing::prepare_clip_frame_plan`, which resolves placement and markers through the shared tempo map at the project rate. The warp map is derived from that plan, never re-evaluated.
   - **Output frames** are clip-local, project-rate frames: 0 is the rendered clip start and the map ends at the plan's `output_frames()`.
   - **Source positions** are Q48.16 positions in the *project-rate view* of the source (point 6), absolute from the source file's first frame. A plan anchor at source-rate frame `s` maps to `round(s·P·65536 / S)` with exact, overflow-checked quotient-plus-remainder integer arithmetic, where `S` is the source rate and `P` the project rate. Endpoints use the same rule, so the source span read by the stream matches `source_rate_frame_count` of the range endpoints, and an identity clip covers exactly the frames the unprocessed path plays.
   - Ring tags for processed clips are clip-local output frames, as in the first draft. Unprocessed clips keep their current absolute project-rate source tags. The consumer knows which rule a clip uses from its prepared slot.
6. **Source-rate conversion for processed clips.**
   - A warp stream reads a `SourceView` at the project rate. When the verified managed source has a different rate, a worker-side rate-adapting view presents it at the project rate, so a 44.1 kHz source in a 96 kHz project keeps its pitch and elapsed duration in every mode. Folding the rate ratio into the map instead would be correct only for Re-Pitch: the pitch-preserving modes would hold source pitch in *samples* and play a 44.1 kHz source a semitone and a half sharp in a 48 kHz project.
   - The converter is in-house: an exact-rational polyphase windowed-sinc filter built from the existing `warp_dsp` Kaiser kernel design. It has `P/gcd(S, P)` phases (at most 320 for 44.1/48/88.2/96 kHz pairs; any ratio up to the existing 4,096-phase bound) and a cutoff at the lower Nyquist with a transition band below it.
     - It is stateless per output frame, so reads are exact after any seek and independent of partitioning.
     - It allocates only at creation.
     - Same-rate sources bypass it bit-exactly.
     - It does not depend on libsamplerate or `OSCILLATE_BUILD_SOURCE_RESAMPLER`. That optional reader continues to serve unprocessed clips unchanged in this stage, so the processed and unprocessed paths may differ in filter detail but never in timing or pitch.
   - The adapter's identity (algorithm and version) is part of the cache key.
   - External links stay rejected for processing with an actionable status ("collect the source into the project to warp it") until linked-source verification is wired to worker views.
7. **Render cache contract.**
   - Processed clip audio is disposable, project-local cache data keyed by `CacheArtifact::clip_processed` and:
     - source identity (SHA-256, byte length, rate, channels, frames);
     - project rate;
     - clip ID and settings generation;
     - the canonical map (every segment's output range, source range and splice flag);
     - the resolved mode and its *applied* parameters (formant amount after intent, transpose in cents, Texture seed);
     - quality tier;
     - algorithm ID `oscillate.warp_dsp.<mode>` with a per-mode algorithm version;
     - the rate-adapter version.
   - The payload format moves to LDW2: the header repeats the key digest, mode, tier, frame count and channel count, and the reader validates all of them before any seek read.
   - LDW1 Re-Pitch entries become misses and are regenerated; they are reconstructable, so nothing user-visible is lost.
   - Changing any engine's output bumps that mode's algorithm version. Re-Pitch is at 2 (LDW1 key change); Complex Pro is at 2 (level-neutral formant shaping, which keeps each shaped frame's energy). Clearing or corrupting the cache never alters source media, project state or user renders.
8. **Live path.**
   - The routed source's prefetch worker owns each processed clip's `WarpLiveReader`. The reader is either a validated Q0 LDW2 cache render or Q0/Q1/Q2 streams built at preparation from the verified project-rate source. It fills the clip's ADR-0006 ring, tagged by clip-local output frame.
   - The callback consumes processed and unprocessed clips identically. It never analyses, renders, reads files, allocates or waits.
   - **Seeks** hold on the new generation until the worker has primed it.
   - **Loop wraps** are primed ahead. An armed loop stops each overlapping clip at the loop end in the current generation and queues the next generation from the loop start, so the planned wrap block plays without a hold. Each set or clear publishes a new loop configuration version, from one control thread. Readiness and misses are bound to that version. A planned wrap is accepted only when the configuration, its ready record and the wrap position agree, and the worker adopts queued frames only for that explicit acceptance. A loop armed after the worker has streamed past its end reports missed. So does a rearm or clear after next-generation frames were queued for an earlier loop: those frames cannot be withdrawn from the rings, so the caller restarts the generation (in place or by seek) to discard them. A wrap the source holds anyway replays as a seek, with fresh processor history.
   - **Saved warp/pitch edits** are audio clip edits. They prepare a replacement source off the callback, and the old graph stays audible and reported active until that source's revision is accepted.
   - **Falling behind:** a callback underrun is reported silence. After one, the worker resumes ahead of the consumer by an explicit ring forward skip, never by playing stale frames, and a live-rendered clip steps down one quality tier. A clip whose lead falls below a quarter of its ring also steps down. Steps are counted and per-clip tiers are reported; tiers never step back up within one prepared source.
   - **Scheduled loop wrap.** A control thread polling far coarser than the device period cannot request a wrap at the loop end. `TransportCursor::request_loop_wrap_at(boundary, start)` queues the planned wrap at the loop end, and `render_transport_block` renders the device block containing the boundary as two parts (two source render calls, each with its own block context). The last frame before the wrap is `boundary − 1` and the next is `start`, at any sample position and device period, with no silent remainder.
     - The request slot is one ticketed atomic (even empty, odd pending, incremented on every queue, apply or withdraw). The callback applies a request only by a compare-exchange from the value it read; `cancel_loop_wrap()` is the same exchange from the control side, so a queued wrap is applied exactly once or withdrawn, never both. No lock, allocation or wait is added to the callback.
     - A request that arrives after the in-flight block already passed the boundary applies at the next block start and is counted late (`loop_wrap_counts()`), never reported exact.
     - Changing or clearing the loop withdraws its queued wrap. Frames already queued for the withdrawn pass are discarded by `request_restart_in_place()`: a new source and processor generation at the current position, so a short priming hold instead of a seek at the old loop end. The same restart re-primes a range armed after its clips were streamed past its end, while the playhead is before that end.
   - **Export** (master, processed stems, and the VST3 effect and mixed instrument variants) uses one shared decision, `processing::processed_clip_request`, and one seam, `exporting::prepare_processed_export_clip`.
     - A raw clip keeps its existing source reader.
     - A processed clip renders through `processing::OfflineWarpClip`: the same map, analysis and stream factory as live and cache renders, at Q0. It reads from the store's verified managed descriptor (`ProjectRateSourceView::from_descriptor`), never a path reopened later, and never depends on the render cache.

## Alternatives considered

- **Keep per-segment external stretching.** Rejected on the measured seam, marker and endpoint failures above.
- **Adopt Bungee, Rubber Band or Signalsmith continuously.** Upstream controls marker timing and seam behaviour, and there are licence gates (MPL with unapproved transitive terms; GPL). In-house engines were chosen to own marker timing and seam behaviour and to avoid those licence gates.
- **Persist an `automatic` mode resolved at render time.** Rejected: a later classifier or default change would silently change saved projects. The resolved mode is always stored.
- **Musical-tick-only markers with a warp on/off flag and beat regions.** Deferred: it needs a lossy conversion of elapsed-placement markers and redefines trim. The 1.24 placement-timebase markers already give one evaluator.
- **Fold the source-rate ratio into the time map.** Rejected for the pitch-preserving modes (point 6). It is correct only for Re-Pitch, and one rule for all modes keeps one cache and seek contract.
- **Reuse the libsamplerate `SourceRateReader` for processed clips.** Rejected as the processed-path dependency: it is an optional, off-by-default build at medium quality, and its state makes seek exactness depend on its internal history.
- **Floating-point time maps.** Rejected: they accumulate drift and make output depend on block size.

## Consequences, risks and tests

The engine needs its own oracles. Acceptance covers:
- exact duration;
- marker and transient placement within 1–2 frames;
- pre-echo, seam and pitch/formant error;
- partition invariance, plus mode-specific seek accuracy and bounded seek cost;
- zero allocation in `start`/`render`;
- comparison with black-box references;
- a recorded listening review.

Performance evidence follows the AGENTS.md DSP policy.

Risks are Complex Pro quality parity, onset-anchoring misfires and seek pre-roll cost. They are tracked in `docs/performance/warp-engine.md`.

Contract acceptance additionally covers:
- 1.24–1.27 migration that preserves pitch, formant intent and markers exactly, with undo/reopen equality;
- source-rate fixtures for every 44.1/48/88.2/96 kHz source × project pair: an identity clip keeps duration (`source_rate_frame_count`) and pitch (≤ 1 cent) in every mode;
- converter passband, stopband and seek-exactness oracles against a direct double-precision sinc;
- cache-key sensitivity to every keyed input, and LDW1 rejection.
