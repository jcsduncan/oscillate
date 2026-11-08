# ADR-0091 — New projects are staged beside their folder and renamed into place

**Status:** Accepted; implemented in `src/persistence/project_creation.cpp`. **Date:** 7 November 2026.
**Amends:** [ADR-0004](0004-atomic-project-save.md), which covers saves into an existing project but not the creation of one.
**Requirements:** FS-03 PM-04.

## Context

Creating a project opened a writer on the final folder, then made the first explicit save there. The save writes a generation before it publishes `state/CURRENT`. A process killed between those two steps left a folder holding `state/WRITER.lock` and a generation, but no `CURRENT`. That folder could not be opened ("project file missing"). It could not be created again ("folder must be empty"). The project library also listed it. The name stayed blocked until the user deleted the folder by hand ([#201](https://github.com/jcsduncan/oscillate/issues/201)).

## Decision

`persistence::ProjectCreation` builds a new project in a hidden sibling stage, `.<name>.creating-<random>`. The name part is shortened to fit `NAME_MAX`, and `<random>` is 32 hex digits. The final name must be absent or an empty folder. Anything else is refused with a conflict. A project name that matches the stage pattern is refused as invalid.

1. The stage is created and immediately locked with `flock` on its directory; the lock is held until creation ends. The sole writer then flushes the stage's entry in the parent and lays out `assets/`, `state/` and `cache/`.
2. The first explicit save commits a generation and `CURRENT` inside the stage, using the ADR-0004 protocol.
3. The stage's own `CURRENT` is validated, the stage is flushed, then it is renamed to the final name. If the final name was absent, the rename uses `RENAME_NOREPLACE`, so a folder that appeared in the meantime is never replaced. If it was an empty folder, a plain rename replaces it, and that rename fails if the folder has gained contents. The writer's directory descriptors stay valid across the rename.
4. The parent is flushed. If this flush fails, the project still exists at its name: creation succeeds, and `publish` returns `uncertain` so the caller can report that its durability is in doubt.

If a process is killed before the rename, the final name holds what it held before: nothing, or the empty folder. If it is killed after the rename, the final name holds a project with a committed `CURRENT`. When creation fails without being killed, the stage is removed on a best-effort basis; a stage that cannot be removed is left for cleanup.

Each creation, and each start of the desktop project library, removes abandoned stages in that folder. It removes only directories that:

- match the stage pattern;
- contain only the `assets/`, `state/` and `cache/` folders a new project's writer lays out, with `assets/` empty or absent;
- were last modified before the current process started;
- hold no `state/CURRENT`;
- are not locked by a live creation.

A stage that holds a committed `CURRENT` is left in place: it is a complete project that was never renamed, and its owner can recover it by hand. Cleanup is best effort. A stage that cannot be removed is skipped and blocks neither other stages nor the new creation. A scan stops after 8,192 directory entries; stages past that bound are not removed. The library never lists a stage.

## Alternatives considered

- Treat an uncommitted store at the final name as an interrupted creation, letting create and the library recognise it. This keeps a visible half project at the final name, and every reader of a project folder would need the same rule.
- Remove every stage without checking its age or lock. That could delete a stage that a concurrent process is still building.

## Consequences and evidence

A stage killed after its first save but before the rename stays hidden on disk until someone removes it. The process-kill suite (`tests/persistence/process_kill_tests.cpp`) kills a creator at six points: before staging, in the empty stage, after a generation, after `CURRENT`, after the rename and after completion. A freshly executed process then has to open the project, or create it again under the same name. Only the project may be listed, and no uncommitted stage may remain. The same suite checks that cleanup keeps a stage newer than the process and a stage a live creation has locked, that it skips a stage it cannot remove, and that it keeps a stage-shaped folder holding anything else. `quick_project_library_recovery_contract` checks cleanup and hiding at library start.
