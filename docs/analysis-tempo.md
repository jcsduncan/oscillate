# Tempo and beat-grid estimate (PA-01 worker slice)

`analyze_tempo` reads a verified mono or stereo `AudioReader` on a worker. It
computes a bounded 5 ms energy envelope, finds positive-energy onsets, and
estimates a constant BPM from repeated adjacent onset intervals. A 120,000-bin
default cap coarsens the envelope for long sources; a 20,000-marker default cap
truncates suggested beat positions and records that fact. Both caps have hard
upper limits. Decoder scratch is 4,096 stereo frames. The scan and period
search have bounded work and perform no callback-side I/O or allocation.

The result carries source key, job generation, BPM, confidence in `[0, 1]`,
source-frame beat suggestions and separately bounded source-frame onset
suggestions. Onsets remain available when there is no reliable BPM. A caller must compare key and generation again
before installation. This API does not change the project tempo map, warp
markers or user corrections. Suggestions are generated data. A failed read or
cancellation yields no partial result. `conflicts_with_project_tempo` is only
a prompt predicate: it requires confidence at least 0.5 and BPM separation
greater than 3%; conform remains an explicit user action using `timeline`.

This estimator is intentionally conservative: fewer than five onsets, silence,
and inconsistent intervals return no estimate or low confidence. Dense
syncopation, rubato, tempo changes, half/double-time ambiguity and harmonic
rhythms need a richer reviewed estimator before the full PA-01 quality gate.
The fixture tests cover 90, 93 and 120 BPM clicks, phase at the first source
frame, silence, short/irregular material, conflicts, cancellation, changed
source failure, transient positions and separate output caps. No new dependency
or licence obligation is introduced. Tempo cache payload v2 persists generated
onsets, and Quick renders selected-clip transient ticks distinct from the beat
grid. Import scheduling, manual warp correction and a full quality review remain
integration work.
