# Project save process-kill qualification

**Requirements:** PM-04/05; ADR-0004 generation publication and recovery.

`persistence_process_kill` opens a real project on the local filesystem, writes
an explicit revision and an autosave, then forks a fresh writer. Its save hook
terminates that writer with `SIGKILL` after one selected completed save stage.
The parent opens a new writer, so abandoned-stage cleanup and validation run in
a new process. The test uses only the public store API to check both pointers
and recovery candidates.

The matrix covers explicit save and autosave, each killed after manifest write,
generation flush, generation rename, generations-directory flush, temporary
pointer write, pointer rename, and the pre-final-flush checkpoint. Before
pointer rename, the corresponding old pointer stays valid. After pointer
rename, it refers to the new complete generation in this running filesystem.
After generation rename, an unpointed new generation remains a recovery
candidate. The last durable explicit revision remains recoverable in every
case. The test confirms actual `SIGKILL` termination rather than accepting a
normal fault-hook return.

On 18 October 2026, the focused test passed all 14 scenarios in 15.38 seconds
with GCC 15.2.0, C++20, and the local development build under
`build/dev`. It did not run the full test suite.

This is a process crash test, not a power-loss or cross-filesystem durability
proof. `SIGKILL` leaves Linux page cache and mounted filesystem state intact;
the after-rename outcome at the unflushed checkpoint may differ after power
loss. Asset-stage kills, migration rollback, launch-filesystem qualification,
and a bounded malformed manifest/media fuzz corpus remain separate work.
