# Save publication after state-directory replacement

**Requirements:** PRD §§8, 18, 21; FS-03 PM-03–PM-08.

A writer pins `state/` and `state/generations/` with directory file descriptors. If another process renames a pinned directory and puts a new directory at its original path, the descriptors remain valid, but a successful pointer rename could publish into the displaced tree. The save would report success even though a fresh open of the project cannot see that revision.

The store now checks that the pinned `state/` and `state/generations/` descriptors still name the entries reachable from the held project root before starting a save and again immediately before replacing `CURRENT` or `AUTOSAVE`. It also checks the held `assets/` descriptor at save entry. A replacement observed at either point fails with `conflict` (or an open error for an inaccessible entry); the save does not claim commitment. A deterministic filesystem fixture replaces `state/` after the new pointer file is written, then verifies that save fails and a fresh reader still opens the prior generation.

This narrows an externally induced directory-swap window; it does not make a hostile concurrent filesystem namespace immutable. The root path itself is not pinned through its parent, and a swap after the final identity check remains a race. Controlled project-directory ownership and filesystem/power-loss qualification remain release gates. No manifest or public API changed, and no real-time path is involved.

Focused GCC, Clang and ASan/UBSan persistence fixtures pass. With the optional encoded-source adapter enabled, the two persistence tests cover 1,628 of 1,871 executable persistence lines (87.0%; the store 83%, `codec.cpp` 90%). This remains below the 95% risk-based persistence target. Uncovered branches include syscall and allocator failures that require dedicated fault seams or process/filesystem qualification; this fixture only claims coverage of the observed directory replacement. The ordinary codec-disabled persistence test covers 1,587 of 1,841 lines (86%).
