# Routing intent

`Track::main_output` is absent for the master bus or identifies a group bus.
Audio and instrument tracks, group buses and return buses all have a main
output. A `SendRoute` has a stable project ID and finite linear gain from 0
through 4. Audible sends target group or return buses. A sidechain send
names one built-in or VST3 effect slot on its destination; a VST3 target also
needs a verified auxiliary-bus receipt before a graph can activate it. Sends and main outputs form one
acyclic dependency graph. The model rejects missing destinations, wrong track
kinds, duplicate send IDs, cycles and the documented count limits before
publishing a revision. Removing a referenced bus requires rerouting its
sources in the same or an earlier atomic command. Undo and redo restore the
whole routing intent. Each send's purpose and target slot are saved with the project.

This is **saved project intent**, not an active audio graph. `ChangeSet.routing`
marks a revision that needs a graph rebuild. Application code shows the saved
revision separately from the active graph revision, including pending or
failed application status. A live or offline consumer must reject any route it
cannot render, with an actionable result, before claiming the revision is
active or publishing output.
