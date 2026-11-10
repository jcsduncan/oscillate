# Persistence dependency

This module links OpenSSL 3 `libcrypto` through CMake's `OpenSSL::Crypto` target for SHA-256 manifest checksums. OpenSSL 3 is licensed under Apache-2.0. The dependency and its transitive package closure belong in the release software bill of materials and Ubuntu/AppImage redistribution review. The checksum detects accidental or malicious changes to a generation; it is not an authenticity signature because the pointer and manifest live together.

The bounded nlohmann/json codec writes the current schema, 1.49, with strict duplicate-field and size checks; a missing or unknown field is rejected. `migration.cpp` brings a manifest from an older released schema to the current one before decoding, through one pure step per schema change, and refuses newer and pre-release schemas ([ADR-0095](../../docs/decisions/0095-schema-migration-policy.md)). Released schemas are those written by a tagged release from v0.3.0 on: 1.47 and 1.48. 1.47 opens through the 1.47 → 1.48 step, which turns each `native_convolution` insert into a Cast effect playing the same IR, and 1.48 through the 1.48 → 1.49 step, which adds the master rebalance, off, and gives no track a chosen or heard role; schemas before 1.47 do not open. The first explicit save after a migration names the pre-upgrade generation as a revision, so pruning keeps it. Each released schema has a golden manifest in `tests/persistence/golden`. The [project-media specification](../../docs/specs/03-project-media.md) and the specs it cites hold the detailed field contracts.

| Minor | Adds |
| --- | --- |
| 1.0 | Initial layout. No source registry: clips reopen as unresolved source references. |
| 1.1 | The source registry. |
| 1.2 | Instrument and MIDI clip state. |
| 1.3–1.5 | Optional master EQ, compressor and reverb controls. |
| 1.6 | AIFF, FLAC and MP3 managed sources. |
| 1.7 | Saved VST3 instances (bounded placeholders). |
| 1.8 | Routing intent: group and return tracks, sends. |
| 1.9 | Track and clip automation lanes. |
| 1.10 | Optional master limiter controls. |
| 1.11 | Send-gain automation targets ([ADR-0021](../../docs/decisions/0021-send-gain-automation.md)). |
| 1.12 | Source tempo reviews. |
| 1.13 | The unified insert chain: `master_effects` and monotonic insert slot identities ([ADR-0023](../../docs/decisions/0023-unified-insert-chain-and-bus-ui.md)). |
| 1.14 | Source key reviews. |
| 1.15 | A typed source `location`: managed, or external with its last known path. |
| 1.16 | Track `muted`. |
| 1.17 | Audio clip `fades`. |
| 1.18 | Track `soloed`. |
| 1.19 | Track `level` and `pan`. |
| 1.20 | `master_level`. |
| 1.21 | Channel-level automation targets. |
| 1.22 | Native compressor sidechain high-pass frequency. |
| 1.23 | Track-to-native-compressor sidechain sends. |
| 1.24 | Per-clip `processing`: pitch, formant choice and reviewed warp markers. |
| 1.25 | `native_convolution` inserts. |
| 1.26–1.27 | Sampler envelope and low-pass cutoff (the sampler was removed in 1.44). |
| 1.28 | Per-clip `warp` mode and parameters ([ADR-0026](../../docs/decisions/0026-continuous-warp-stream.md)). |
| 1.29 | Onset review on source tempo reviews. |
| 1.30 | Built-in synth patches (`native_instrument`) and native synth parameter automation. |
| 1.31 | VST3 controller state (absent, present-empty or non-empty). |
| 1.32 | MIDI clip pedal lanes ([ADR-0035](../../docs/decisions/0035-pedals-in-midi-clips.md)). |
| 1.33 | Built-in effect slots (`native_effect`) and their parameter lanes (`native_effect_parameter`) ([ADR-0043](../../docs/decisions/0043-one-native-insert-path.md)). |
| 1.34 | Channel pan and mute automation (`track_control` keys `channel_pan`, `channel_mute`); a mute lane overrides the mute switch. |
| 1.35 | Presentation: track `name` and `color`, audio and MIDI clip `color` (null or 0xRRGGBB), and project `markers` (`id`, `position` in ticks, `name`; sorted, one per position). None affects audio. |
| 1.36 | Project `key`: null, or `{tonic, mode}` (tonic 0–11 from C, mode `major` or `minor`), set by the musician. It never affects audio. |
| 1.37 | The classic effect kinds `native_eq`, `native_compressor`, `native_reverb` and `native_limiter` are removed (ADR-0047); built-in effects are `native_effect` patches (Shape, Press, Ceiling, Expanse). |
| 1.38 | Audio-clip Follow/Own tempo and source downbeat review, plus pre-roll lead bars (ADR-0053). |
| 1.39 | Automation domains persist their linear, logarithmic or decibel scale; segment evaluation follows that scale. |
| 1.40 | Built-in synth documents (`NativeSynthDocument`: mod routes, curves, FX order, media references) for Nova; Lumen and Snarl removed ([ADR-0057](../../docs/decisions/0057-nova-flagship-synth.md)). |
| 1.41 | Built-in effect `document` ([ADR-0058](../../docs/decisions/0058-pump-volume-shaper.md)). |
| 1.42 | Built-in effect `link` ([ADR-0058](../../docs/decisions/0058-pump-volume-shaper.md)). |
| 1.43 | Track `note_effects`, `macros` and `frozen` ([ADR-0059](../../docs/decisions/0059-play-and-sparkle.md)). |
| 1.44 | Built-in synth document `regions` (`slot`, `media`, `start_frame`, `tag`, `name`; omitted when empty) for Crate ([ADR-0063](../../docs/decisions/0063-crate-chop-sampler.md)); the sampler instrument source (`"kind": "sampler"`) and the `sampler_level` control key are removed. |
| 1.45 | Audio clip `clay` take ([ADR-0081](../../docs/decisions/0081-clay-note-editing.md)). |
| 1.46 | Project `chords` (the Chord track, [ADR-0083](../../docs/decisions/0083-harmony-chord-track-and-comp.md)). |
| 1.47 | `chords` removed: chords are notes in MIDI clips ([ADR-0084](../../docs/decisions/0084-chord-clips.md)). |
| 1.48 | `native_convolution` inserts removed; a built-in effect `document` may hold managed `media` slots (Cast's impulse responses, [ADR-0115](../../docs/decisions/0115-cast-convolution-reverb.md)). |
| 1.49 | The project's master `rebalance` and each track's `mix_role` and `heard_role` ([ADR-0123](../../docs/decisions/0123-master-rebalance.md)). |

Schemas before 1.47 are development formats that do not open; 1.47 onwards
are released. Check every active branch before claiming the next minor.

The store stages WAV through a held `assets/` directory descriptor. Staging copies a pinned input fd to a temporary file, verifies the WAV metadata and SHA-256, flushes and publishes the digest-named asset without replacing an existing file, then flushes the directory. A move-only receipt authorizes a new source record in a save from the same writer. A previously committed identical record needs no new receipt. Saves check the managed file type and length; `open_managed_source` additionally checks its full hash and metadata before returning a caller-owned pinned fd. The reader reports unresolved clip sources, missing assets and obviously damaged assets alongside a valid project generation. Each committed save prunes older generations of the active project, keeping the 10 most recent valid ones plus any referenced by the current pointer, autosave or named revisions. It never deletes an incomplete or unverifiable generation (`prune_generations_unlocked`). Named revisions (`list_named_revisions`, `load_named_revision`) and collect-and-save (`src/application/project_collection.cpp`) are implemented. An explicit full integrity audit and separate plugin-state blobs still need their own contracts. The checksum file permits validation of otherwise unpointed complete generations; it is not an authenticity signature. Filesystem and power-loss qualification remains a release gate beyond the deterministic interruption fixtures.

Recovery enumeration inspects at most 1,024 directory entries, retains at most 256 valid candidates, reads at most 64 MiB of candidate manifests, and caps a conservative estimate of retained decoded state at 64 MiB in one call. The decoded-state estimate charges tempo events for their expanded timing pieces. An exceeded limit is reported as `resource_exhausted`; the store does not silently return a partial candidate list. Each manifest remains capped at 16 MiB. Ordinary `load_current` and `load_autosave` do not enumerate generations.
