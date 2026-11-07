# Returning to a saved project snapshot

**Requirements:** PRD §§8, 18; FS-03 PM-03/05.

`ProjectModel::restore_snapshot` publishes a validated snapshot of the same project as one new model revision and one undo step. The saved generation's old revision number is not reused: the new revision is greater than the currently active model revision, so async work from either prior state cannot install against it. Undo returns to the displaced state and redo reapplies the selected state, each with a new revision. A restore clears any previous redo branch and ends gesture coalescing.

The method rejects a different project ID, zero saved revision, invalid snapshot or exhausted current revision without changing model state or history. If the sample rate changes, it acquires the stopped-transport lease before publication and returns that lease to the caller; a denied or empty lease leaves the model unchanged. The broad change set includes IDs from both states so graph and UI consumers can invalidate their projections.

This is a model command only. The caller must select and validate a named generation through `ProjectStore`, handle missing-media diagnostics, coordinate any active playback, then save the accepted revision through the normal atomic project-save path. Neither the named pointer nor `CURRENT` changes merely because this method is called. The ProjectSession and Quick recovery workflow remain integration work.

The focused project contract uses two distinct track states and an older saved revision as an independent oracle for restore, undo, redo and monotonic revision behaviour. It also tests foreign/invalid snapshots, redo invalidation, revision exhaustion, a running transport and a held rate-change lease. GCC and Clang run with warnings as errors; the focused ASan/UBSan test, changed-file clang-tidy and clang-format checks pass. The isolated project-contract coverage run executes every line of `restore_snapshot`. Its 74% whole-file result omits the other project-model consumers included in the aggregate coverage configuration; the combined gate must be checked after integration.
