# ADR-0013 — Maintained JSON codec for project manifests

**Status:** Accepted; implemented. The codec reads and writes only the current schema (superseded by [ADR-0095](0095-schema-migration-policy.md) from the first public beta: released schemas open through forward migrations); see the schema table in [`src/persistence/README.md`](../../src/persistence/README.md). The older schema examples below record this decision's original context, not current compatibility.
**Date:** 14 October 2026
**Requirements:** PRD §§8, 21–23; FS-03 PM-03/PM-08; ADR-0004.

## Context

The first schema 1.0/1.1 manifest codec used a fixed-order custom JSON cursor and hand-built output. It was small enough for M1, but project state will gain MIDI, routing, plugin state and automation. Extending a custom JSON syntax implementation would increase parser risk and currently rejects otherwise valid field ordering and escaped strings. A maintained JSON library is preferred over extending the custom parser; nlohmann/json was chosen.

## Decision

Use the Ubuntu-packaged `nlohmann_json` CMake target, version 3.11.3 or later, in `persistence` only. It is a build-time header dependency; no network fetch occurs at CMake configure and no copy is vendored into the repository. The [upstream library](https://github.com/nlohmann/json) is MIT licensed. Record the exact package/header version and notice in the release dependency inventory.

The library handles JSON syntax, UTF-8 strings and serialization. Project code still owns the strict versioned manifest schema: required and allowed fields, types, ranges, entity IDs, count/byte limits, path containment and semantic validation. Reject duplicate and unknown object fields rather than accepting ambiguous state. Bound raw manifest size before parsing and validate array/string/blob counts before installing a `project::Snapshot`. Parsing remains off the audio callback.

The writer produces deterministic, readable JSON for a given snapshot and version. Its whitespace and field byte order may change from the old custom writer; generation and pointer checksums are computed over the actual new bytes. Existing schema 1.0 and 1.1 generations must reopen unchanged. [ADR-0014](0014-m3-midi-event-contract.md) separately approves schema 1.2 for instrument and MIDI state, and new writes now use 1.2. Old generations are never rewritten merely by loading them.

## Alternatives considered

- Extend the fixed-order custom cursor: it would duplicate maintained JSON syntax code as the schema grows.
- Vendor a JSON header or fetch it during configure: both obscure dependency provenance and make local reproducibility harder than an explicit system package.
- Relax schema validation because the JSON parser is mature: syntax correctness does not prove a safe project model.

## Consequences and evidence

The persistence target gains an MIT-licensed build dependency. The replacement merged with golden old-generation reopen, round-trip, reordered fields, escapes, duplicate/unknown fields, malformed numbers, oversized input, interrupted saves and migration rollback fixtures passing locally. Exact byte golden tests assert only the fields and format that the project commits to; checksum tests compare against bytes actually written. Distribution still requires the dependency inventory and notice review.
