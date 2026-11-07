# Reviewed source tempo state (schema 1.12)

`Snapshot::source_tempo_reviews` is essential project state, separate from
`cache/`. Each record refers to one managed source ID and carries the latest
detected BPM/confidence, generated source-frame beat suggestions, optional
user-corrected BPM, user source-frame beat markers and a truncation flag.
Generated and user markers remain separate so a view can label their origin.
At a shared frame, the user marker wins and the generated duplicate is removed.
Changing or removing a user marker does not silently restore an old suggestion;
the user can explicitly reanalyse to request a fresh generated set.

`ProjectSession::accept_source_tempo_analysis` checks the current analyzer
cache key, verified source rate/frame count and the project revision captured
when work started. The model also checks the immutable source digest and that
revision before it publishes the command. An edit, undo or redo changes the
revision and rejects a late completion. Reanalysis updates generated fields
only; the corrected BPM and user markers remain. A correction is a review of
source tempo, not an implicit conform action on the global tempo map.

The tempo semantic cache key includes the analyzer's onset cap and uses
algorithm version 2, so cache entries from the earlier beat-only option set
are rebuilt before a result can be reviewed.

User correction and user marker replacements are one undoable project command
each. The state is bounded to 20,000 markers of each provenance per source and
200,000 markers across the project, with finite BPM/confidence and strictly
ascending in-range frame positions. Removing a source record explicitly removes
its review; missing managed media retains the source record and its review.

The manifest reader accepts 1.0–1.11 by defaulting reviews to empty, rejects
review fields in older schemas, and validates exact fields and bounds in 1.12.
Neither cache eviction nor reanalysis changes manual choices.

Schema 1.29 adds onset review to the same record: `generated_onset_frames`
(the warp transient detector's result, replaced only by an accepted
reanalysis), `dismissed_onset_frames` (user corrections, never replaced) and
`onsets_truncated`. `ApplySourceOnsetAnalysis` uses the same captured
revision and source-identity checks as tempo analysis; a generated onset
within 64 source frames of a dismissal is dropped. Both lists share the
per-source and per-project marker bounds. Manifests 1.12–1.28 load with an
empty onset review and reject the 1.29 fields; 1.29 requires all three. The
Quick Warp editor uses this review for transient suggestions and the tempo
review for auto-warp and tempo conform (FS-05).
