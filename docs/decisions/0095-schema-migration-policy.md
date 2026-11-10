# ADR-0095 — Projects from every released schema open, through chained forward migrations

**Status:** Accepted and implemented. Released schemas: 1.47 (v0.3.0) and 1.48 (v0.4.0). **Date:** 8 November 2026; amended 10 November 2026.
**Supersedes in part:** [ADR-0013](0013-manifest-json-library.md), where it says the codec reads only the current schema.
**Requirements:** PRD §§8, 21–23; FS-03 PM-07; [ADR-0004](0004-atomic-project-save.md).

## Context

Schema changes before v0.3.0 dropped older layouts with no migration (the greenfield rule in AGENTS.md). Once people outside development save projects, a release that cannot open the projects of the release before it loses their work. PM-07 asks for versioned migrations of publicly released schemas, without saying which schemas count, where the code lives or how it is tested.

## Decision

1. **Released schemas.** A schema is released when a tagged release build writes it, from v0.3.0 (schema 1.47) on, pre-releases included. Schemas older than 1.47 are not released and stay unsupported. `detail::released_schemas` in `src/persistence/migration.hpp` lists the released schemas, oldest first.
2. **Every schema change ships its migration.** A change that moves the current schema from N to N+1 adds the step N → N+1 in the same pull request. Projects from every released schema then open in every later release.
3. **Migrations live in `persistence`.** `src/persistence/migration.cpp` holds the chain. `decode_manifest` reads the saved schema, passes the parsed manifest to `migrate_manifest` and decodes the result as a current-schema manifest. No other module sees an old layout.
4. **Each step is a pure function, and steps chain.** A `MigrationStep` maps a JSON manifest in one schema to the next schema, with no I/O, clock or global state. Loading applies the steps in order from the saved schema to the current one; the chain sets the manifest's `schema` field after each step. A step's output has exactly the top-level fields and layout of its target schema, since the decoder checks the chain's final result as strictly as a saved manifest; intermediate results are not checked. A step never skips a schema, so a project from any released schema takes the same path as one saved a release later. A gap in the chain fails the load as an internal error rather than decoding a stale layout.
5. **Golden manifests, opened and round-tripped by CI.** Each released schema has one golden manifest (`tests/persistence/golden/schema-<major>.<minor>.json`) saved by that release's build from the [demo song](../../tools/demo_song/README.md) and never edited afterwards. Beside it, `schema-<major>.<minor>.expected.json` records the golden's content when it is saved: each track's name and its numbers of audio clips, MIDI clips, notes, effects and automation lanes, and the number of markers. `persistence_schema_migration` opens every golden manifest through the chain, checks that content against the expected file, saves it in the current schema and checks that the saved bytes reopen to the same bytes. It fails when the golden set and `released_schemas` differ, and when the chain does not run contiguously from the oldest released schema to the current one.
6. **No downgrades.** Nothing converts a project to an older schema. An older release does not open a project saved by a newer one.
7. **A newer schema is refused.** A manifest whose schema is newer than the build's current schema fails as `unsupported`, with a message naming its schema and the newest one this build opens. A schema older than the oldest released one fails as `unsupported` too, with a message saying it predates the oldest supported release. Neither rewrites anything. When no generation of a project can be offered for that reason, `ProjectSession::inspect_recovery` fails with the same error, and opening the project shows it.
8. **Migration happens in memory.** Opening a migrated project never rewrites its generation. The next save writes a new generation in the current schema, so the pre-upgrade generation stays on disk as the backup and the rollback (ADR-0004). The first explicit save after a migration also records a named revision for the pre-upgrade generation, `Before upgrade from <schema> <first eight generation digits>`, so generation pruning keeps it. The store does this before it writes the new generation, and fails that save if it cannot; a revision that already names the generation is reused. A loaded generation reports the schema it was migrated from (`LoadedGeneration::migrated_from`).
9. **Each release.** A release whose schema differs from the last released one adds that schema to `released_schemas` and adds its golden manifest.

## Steps

- **1.47 → 1.48** ([ADR-0115](0115-cast-convolution-reverb.md)). Each `native_convolution` insert, on a track or the master, becomes a Cast effect (`native_effect`, engine `cast`, version 1) with the same slot id, bypass state and generation. Its managed IR moves to the document's `ir` media slot, `wet` becomes `cast.mix`, and `cast.normalise` is off, because the old core played the IR at its own level; Cast's other parameters keep their defaults. Cast plays any mono or stereo file up to 10 s, so every insert converts and none is dropped. Cast's mix is equal-power where the old one was linear, so a partial mix comes out somewhat louder. Nothing else differs between the schemas.

## Alternatives considered

- **Count only releases from the public beta on.** Projects saved by the 0.x releases people already use would stop opening at the next schema change.
- **Direct migrations from each released schema to the current one.** Each schema change would need a new migration for every older release. Chained steps keep each change to one step.
- **Migrate typed snapshots instead of JSON.** The model only has the current types, so an old layout would need its own frozen types. Steps on the parsed JSON need nothing besides the step.
- **Rewrite the generation on load.** That would lose the pre-upgrade copy that rollback depends on, and opening a project would write to it.

## Consequences

- A schema change costs a migration step, a golden manifest when it ships, and a test.
- AGENTS.md, the HLD and the persistence README describe the policy.
