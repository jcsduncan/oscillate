# Verified VST3 project insertion

**Scope:** PI-02/04 control-side insertion seam. There is no Quick browser or
audio callback work in this slice.

`insert_verified_vst3_effect` accepts a selected catalogue entry, an effect
class ID, a track and a chain index. It requires a successful scanner receipt,
then rescans the bundle through the out-of-process helper. The canonical bundle
and binary paths, byte size and SHA-256 must match. The chosen processor class
must still have the same identity and `Fx` subcategory. A missing, changed or
unavailable binary, ambiguous class, and an invalid insertion position fail
before any project mutation.

After verification, `ProjectSession` allocates a stable plugin instance ID,
applies one `InsertVst3Plugin` model command and saves the project. The new
instance records class, vendor, name, version, canonical bundle path and binary
hash. Its component state and parameter references are empty, and `missing` is
false. Undo/redo retains the same ID and chain position; reopening retains the
saved intent. If saving fails after the model accepts the command, the session
retains that edit for an explicit save retry and reports the failure.

This insertion service does not load or activate the plugin, install its scan
receipt in the routed playback provider, or claim offline VST3 export support.
Those are separate control-side actions with their own revision and failure
status. The existing routed path remains fail-closed when its receipt is absent.
The bundle can change after insertion, so the later live binding step must
recheck its hash against the saved identity before any in-process load.
