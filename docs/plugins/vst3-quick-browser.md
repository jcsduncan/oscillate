# Qt Quick VST3 effect browser slice

The same drawer now has a bounded instrument path for selected instrument
tracks; see [the instrument slice](vst3-quick-instrument.md). The effect
constraints below continue to apply to audio-track insertion.

The Quick drawer searches VST3 effect classes by name, vendor and bundle path.
It shows scan diagnostics, quarantined bundles and an explicit Retry action. A
user can add an absolute bundle or directory path; those paths precede the
standard Linux roots for the current app run. The scan is bounded to 128
bundles, 20,000 visited entries and 32 search paths. The drawer reports when
that limit is reached. This is a narrow discovery and insertion workflow, not
a compatibility certification or the full plugin browser in PRD §16.

Scan and fresh pre-insert verification run on a background worker through the
helper process. The UI thread only applies `ProjectSession::insert_vst3_plugin`
and its save. A verified result carries the exact selected class and binary
identity. If the project changes while verification runs, the result is
rejected. The project command validates the track and chain position; a failed
save reports that the model revision remains applied and undoable. The later
live bind rescans the binary and fails closed if it changed after insertion.

The routed playback slice accepts up to two verified serial effects on one
audio track. Other chain shapes remain unsupported by
this path. The host forwards stepped project tempo and musical position in
each VST3 processing block. A saved missing insert stays in the model and prevents silent
replacement; the browser shows its saved identity. The scanner-off build shows
an unavailable state and refuses insertion. Saved inserts are labelled as saved
model entries; current playback status is shown separately. A plugin scan or
load never runs in the audio callback.

The helper must sit beside `oscillate`. The local VST3 build copies it to
that location after building the app. The executable no longer falls back to
an absolute build-tree helper, so a package that omits the helper visibly
fails its scan or playback preparation. Distribution packaging must include
that sibling helper and complete its permission and sandbox review.

The offscreen Quick contract uses the synthetic Loaded Gain fixture to cover
search, stale revision rejection, verified insertion, undo/redo and
save/reopen. The catalogue contract covers quarantine and Retry; the service
contract covers
changed/missing binary and class identity refusal. Tests do not establish
third-party plugin GUI or broad VST3 compatibility.

## Saved-plugin recovery

The selected track's insert chain exposes a recovery panel for saved VST3
effects and instrument sources. It shows the saved class, vendor, version,
bundle path, component-state size and parameter-reference count. A missing or
unverified entry remains in its original slot; scanning never rewrites it.
The user may add an absolute search path, scan, and choose a candidate only
when the class metadata and SHA-256 binary identity exactly match the saved
entry. The worker rescans that candidate before applying one undoable relink
command. The command changes the bundle path and missing flag while retaining
the plugin ID, chain slot, bypass state, component state and parameter refs.
Recovery is stopped-only, and a relinked project revision is not described as
audible until the playback graph passes preflight. A save failure leaves the
accepted model edit undoable and explicitly marked unsaved.

This slice cannot safely relink a legacy placeholder whose original binary
SHA-256 identity was never recorded, or substitute a newer plugin build with
the same class ID. Those cases require a separate user-approved migration
contract. The focused Quick test covers exact match, incompatible scanned
class refusal, stopped-only behavior, QML candidate presentation, state
retention, undo and reopen. It does not certify plugin compatibility.
