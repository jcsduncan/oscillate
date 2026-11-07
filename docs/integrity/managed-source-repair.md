# Managed-source repair from an exact candidate

**Requirements:** PRD §§8, 21; FS-03 PM-06/PM-08/PM-09.

`SourceRecord` persists a stable source ID, encoding, SHA-256, byte length and decoded metadata. It does not yet persist an external-link location. The new store API repairs a **missing managed asset** for a source present in a selected, validated generation. It does not change the manifest, source ID, clips, or project revision. External linked-media relinking and collect-and-save still require a project-model/schema contract and UI work.

The writer accepts a caller-owned pinned regular-file descriptor, rejects a candidate above the 64 GiB restore limit or with a different byte length before copying, then uses the existing cancellable staging pipeline synchronously on the caller's worker thread. Its calculated hash, encoding, rate, channels and frame count must all match the persisted `SourceRecord`. The selected generation is revalidated before publication. Publication uses the existing no-replace digest path and directory flush; an existing file is never overwritten, including if it is corrupt or a symlink. An error before linking fails; an after-link durability error reports `uncertain` only when the visible asset passes full hash and metadata verification. The caller must keep project-writer use serialized and invoke this potentially long operation off the UI and audio threads.

Filesystem fixtures cover missing-source diagnostics, same-length wrong bytes, exact WAV and FLAC recovery, stable manifest/source identity on reopen, pre-link interruption, after-link uncertain outcome, generation-directory replacement immediately before publication, unregistered/malicious source identity, invalid generation paths, FIFO input and symlinked managed paths. Cache files and user renders are not involved.

The 64 GiB cap is a product limit of this repair path, not a limit on import or existing projects. A selected generation must still exist at publication. Filesystem power-loss qualification remains open; a visible asset after an uncertain result can disappear after an actual crash. The Qt Quick managed-media panel now offers a candidate picker, worker-backed verification, cancellation and result status for this exact managed-asset repair. It does not implement external linked-media relinking.

## Local verification

The focused persistence suites passed under GCC, Clang 21 with warnings as errors, and GCC AddressSanitizer/UndefinedBehaviorSanitizer. `clang-format` and targeted `clang-tidy` passed. The same two suites under GCC coverage exercised 1,877 of 2,182 persistence source lines (86.0%); this remains below the provisional 95% component goal. Most uncovered paths are defensive filesystem failures, malformed legacy/schema branches, and error returns outside this repair slice. No synthetic tests or exclusions were added to inflate the figure.
