# Writer-lock replacement during a save

**Requirements:** PRD §§8, 18, 21; FS-03 PM-04/PM-08.

`flock` protects the inode opened by the writer, not the `WRITER.lock` filename. If the lock file is unlinked and replaced, another writer can obtain an exclusive lock on the new inode while the first writer still holds its lock. Before this change, both could save to one project.

The store now checks that `WRITER.lock` still names the held regular-file inode immediately after locking, at save entry, and just before the `CURRENT`/`AUTOSAVE` rename. An observed replacement returns `conflict` and does not claim the attempted save committed. A filesystem fault fixture replaces the filename after the new pointer file is written, verifies the first writer fails while `CURRENT` stays at the previous generation, then proves a second writer can lock the replacement and save a generation that reopens normally.

This check addresses observed lock replacement at those boundaries. It does not make a directory controlled by another process atomic: the lock path can still change between the last check and pointer rename. Project-directory ownership and hostile namespace concurrency remain release qualification concerns. There is no schema, API, dependency, or audio-thread change.

Focused GCC, Clang and ASan/UBSan persistence fixtures pass; formatting and targeted tidy pass. With the encoded-source adapter enabled, the two persistence tests cover 1,644 of 1,893 executable persistence lines (86.8%; store 83%, codec 90%). This remains below the 95% risk-based persistence target. Uncovered paths include syscall failures and deeper malformed-input cases, which need independent fault seams or filesystem qualification rather than coverage-only fixtures.
