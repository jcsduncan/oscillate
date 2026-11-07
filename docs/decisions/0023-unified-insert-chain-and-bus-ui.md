# ADR-0023 — Unified insert chain and visible buses

**Status:** Accepted; implemented through [ADR-0043](0043-one-native-insert-path.md) and [ADR-0050](0050-one-chain-per-track.md).

Schema 1.13, old-project migration, read-only Quick chain presentation, bounded
graph/export consumers, and atomic native add/reorder/bypass/removal commands
are integrated. Arbitrary ordered processing and drag/drop remain open.

**Requirements:** PRD §§6, 15–16; AM-05, AM-09, UQ-07.

## Context

The legacy 1.12 model stored VST3 inserts and four native master processors in
separate fields. Schema 1.13 replaces them with typed instrument sources and
one ordered effect vector per saved path. Group and return tracks already have
stable IDs; the master uses a reserved logical path identity. Engine node 1 is
still an implementation detail. Live and export render only their documented
bounded insert subset. Quick shows the saved chain and the grid in track rows;
editable cards must wait for the matching render and export contracts.

## Product direction

- Keep the beat/bar grid visible in every empty arrangement row. A clip may
  cover the grid inside its own bounds.
- Selecting an audio, instrument, group, return or master path opens one
  resizable chain surface at the right or bottom. Show order, bypass, identity
  and saved-versus-active status. Drag/drop and keyboard commands invoke the
  same project edit.
- Show group and return buses as real saved tracks with routing and status.
  Keep the master visible at the bottom; a visual-only master card does not
  satisfy this contract.
- Present native EQ, compressor, reverb and limiter as built-in chain items
  beside VST3 effects. Native DSP does not require a VST3 host round-trip.

The common contract is a processor/device slot, not a requirement that every
built-in processor be packaged as a VST3 binary. Native and VST3 adapters
must expose compatible processing, parameter identity and automation,
latency/tail, bypass, state, and fault-status contracts to the graph and UI.
Native processors remain linked to the application and run directly in the
prepared graph. Shipping them as standalone VST3 bundles for use in other
hosts would be a separate product and packaging decision.

## Proposed durable chain contract

Each signal path owns exactly one ordered vector of audio-effect insert slots. A tagged
`SignalPathId` identifies either a saved `TrackId` or the reserved logical
master path. The master tag must never be encoded as a fake track or inferred
from engine node 1. It remains stable across save/reopen and routing rebuilds.
Group and return paths use their existing `TrackId` identities.

An instrument is a source that generates audio before that vector, not an
effect insert that can be dragged after another processor. Keep the sampler
patch or a VST3 instrument source as a separately typed, durable track source.
Its saved `PluginInstanceId`, state and automation references remain stable.
The chain may contain VST3 audio effects after that source when the host and
graph support them, but moving an instrument source into the effect chain (or
an effect into the source position) is invalid. A group, return or master path
cannot own an instrument source.

Each slot has a globally unique, nonzero `InsertSlotId` that survives move,
bypass, settings edits, undo, save/reopen and compatible graph rebuilds. A
slot holds a discriminated native kind or VST3 identity, enabled/bypass state,
versioned settings or opaque plugin state, and generation information.
Changing order does not regenerate identities. Copy/paste creates a new slot
identity; its automation-copy and target-remapping policy needs a detailed
specification. Project state records user intent. Prepared runtime, active
revision, fault and compatibility status are derived control-side state,
never another persisted processing order.

VST3 slots keep `PluginInstanceId` as a separate identity.
`Vst3ParameterTarget{plugin, parameter_id}` continues to refer to that saved
plugin ID, so move or bypass does not retarget automation. The same target
continues to resolve when the ID belongs to a VST3 instrument source;
validation searches both source and effect identities. Native automation
needs a typed target using `InsertSlotId` and a stable parameter key; master
automation also needs a durable lane owner because the current snapshot has
none. Those additions must ship with validation and playback/export
evaluation, not as orphan target variants. Missing or unavailable VST3
plugins remain saved slots with their exact identity, state, parameters,
position and automation references. Missing is not user removal or bypass.

Validation rejects duplicate slot IDs, duplicate plugin instance IDs,
incompatible payloads, invalid path ownership, an effect in the instrument
source position, an instrument in an effect slot, and exceeded slot/state
limits before graph preparation. Unresolved legacy entries remain durable but
are explicitly unavailable to playback and export until resolved. Exact
bounds belong to the model/codec contract.

## Migration and single source of truth

The model/codec writer emits manifest schema 1.13 as the first chain
schema. Its writer encodes one slot vector
per track and one for the tagged master path. It does not also write legacy
`vst3_plugins` or four master effects as independent processing truths.
Readers of supported older schemas create the new in-memory form:

- On a legacy audio track, migrate the VST3 vector in order into effect slots,
  preserving each full saved payload and `PluginInstanceId`. Give each effect
  a stable derived slot ID.
- On a legacy instrument track with a sampler patch, preserve that patch as
  the source. Any saved VST3 entries require explicit role resolution before
  playback; do not silently interpret an unclassified entry as an effect.
- On a legacy instrument track without a sampler patch and with exactly one
  VST3 entry, migrate that entry as the instrument source, preserving its
  `PluginInstanceId`, state and automation references. It does not receive an
  effect `InsertSlotId` merely to fit the new chain representation.
- A legacy instrument track with multiple VST3 entries lacks saved role
  metadata. Preserve every entry, identity and order as unresolved legacy
  state, expose that status and require explicit role resolution before
  playback. Migration must not guess which entry generates audio or drop the
  project. A fixture must cover this schema-valid but previously unrenderable
  case.
- Place each configured legacy master effect in the previously audible
  EQ → compressor → reverb → limiter order. Empty EQ and absent optional
  effects do not become audible slots. Preserve settings and generations.
- Assign deterministic, namespaced slot IDs from saved project/path/legacy
  identity, using an algorithm fixed by migration fixtures and collision
  checks. Repeated reads before first upgraded save must yield the same IDs.
  A collision fails clearly instead of silently changing an ID.
- Preserve VST3 automation references by plugin ID. Do not invent native
  automation for legacy settings.

Migration requires old-schema read fixtures, deterministic ID and state
preservation checks, pre-upgrade backup, interrupted-write and rollback
tests, and save/reopen proof. Unsupported future schemas remain rejected.
An upgrade commits only with a complete atomic new generation; recovery must
still find the last valid one. Live and export consumers must consume the new
chain in saved order or explicitly reject it. Transitional adapters cannot
become a second persisted truth.

### Preparatory identity contract (implemented; no schema cutover yet)

*Later:* the derivation below served old-format migration and was removed with it (greenfield; ADR-0043, ADR-0047). Slot identities are command-allocated serials (`command_insert_slot_id`).

`InsertSlotId`, a tagged `SignalPathId`, and
`derive_legacy_insert_slot_ids` are available in the project model. The
derivation uses two fixed 64-bit byte streams over the literal
`oscillate-legacy-insert-v1`, project ID, path tag and track ID or master
tag, then VST3 plugin ID or native kind. Words are encoded low byte first;
the upper bit of the first output word reserves a migration namespace.
Input order does not affect identity. Golden fixtures pin the resulting IDs,
and duplicate or colliding IDs fail. This helper only accepts legacy VST3
track effects and native master effects. Instrument-source role resolution
must happen before calling it.

The preparatory 1.12 contract did not change snapshots or runtime. The
1.13 model/codec change replaces those fields rather than keeping a second
writable slot vector, once the graph, export and Quick consumers migrate.

### Read-only 1.12 projection

`project_legacy_insert_chain` validates a saved snapshot and derives a
control-side view of its track paths and reserved master path. It presents
VST3 effects in each legacy non-instrument track's saved vector order and
configured master processors in their fixed audible order. Instrument
sources are separately typed: sampler patch, sole VST3 instance or an
explicit unresolved legacy source that retains all plugin IDs in saved
order. A sampler plus any VST3 entry and multiple VST3 entries without a
sampler remain unresolved. Missing plugin status is carried without
removing the placeholder. Slot IDs come from the pinned derivation, and a
duplicate or collision fails rather than being renamed. This view cannot be
edited or saved independently; consumers still need the same retained
snapshot for full plugin state and native settings. An unresolved source or
missing plugin is not evidence that the path is ready for playback/export.
Only native master descriptors expose a settings generation. VST3 descriptors
have no synthetic generation: future runtime reuse must inspect the full
saved plugin identity/state and the host's compatibility contract.

The coordinated cutover proceeds in this order:

1. Replace the legacy mutable processing fields with a typed instrument
   source and ordered effect slots in `Snapshot`. Keep the existing public
   commands as adapters that edit that one vector, or replace their callers
   in the same change. Classify legacy instrument entries without guessing;
   retain multiple/unclassified entries as explicitly unavailable state.
2. Make every graph, export and Quick bridge reader either consume that
   source/vector or reject an unsupported shape before rendering or writing
   output. A read-only projection may expose the old fixed master order to
   bounded consumers, but cannot manufacture a second mutable order.
3. Change the codec reader/writer together: read 1.12 and earlier through
   the pinned migration, write only 1.13 slots and source, validate global
   slot/plugin identities and limits, and verify old-generation backup,
   interrupted save and rollback fixtures. Do not advance CURRENT on a
   failed upgrade.
4. Add slot reorder, bypass and removal commands with atomic undo and
   automation cleanup; then extend graph and export acceptance to arbitrary
   supported order before enabling editable chain controls in Quick.

### Implemented model/codec contract

`Track::instrument_source` is one tagged source: empty, sampler patch, VST3
instrument or unresolved legacy payload. Unresolved payload retains an
optional sampler patch and every full VST3 instance in saved order. Every
track, including group and return buses, owns `Track::effects`; the reserved
master owns `Snapshot::master_effects`. Each `EffectInsert` carries a globally
unique ID, enabled state and one native or VST3 payload. A native payload
stores settings and a nonzero generation. VST3 state, parameters, identity
and saved missing status stay in the full plugin payload. A fresh exact scan
may recover a saved missing plugin; unresolved roles remain unavailable.

Schema 1.13 writes only those fields and the monotonic
`next_insert_slot_serial` high-water mark. Command-created slots use a fixed
namespace distinct from the high-bit migration namespace. Undo, redo and
saved-state restore retain the highest serial already seen, so a removed
processor ID cannot be reused by a divergent history branch. The existing
master-setting commands preserve an existing slot ID and increment its
generation when settings change. They add new native slots in canonical
EQ → compressor → reverb → limiter order and allocate a fresh ID after
remove/readd. They do not expose arbitrary reorder or bypass editing yet.

The project model also accepts `AddNativeEffect`, `MoveEffectInsert`,
`SetEffectInsertEnabled` and `RemoveEffectInsert` on tagged track or master
paths. A move uses the destination index after removal. These commands reject
invalid paths, bounds and no-op moves/bypass changes; one accepted command
commits one undoable revision. Slot IDs survive moves, bypass and undo, and
explicit VST3 effect removal atomically removes its track and clip automation
references. The commands only record saved intent. Live and export still
reject shapes they cannot render, and Quick does not yet expose these edits.

Validation limits each path to 64 effect slots, the project to 4,096 slots,
each track to 32 VST3 instances and the project to 2,048 VST3 instances.
The existing state-byte limits remain. Global duplicate slot/plugin IDs,
invalid roles, zero native generations and allocator rollback are rejected.
The old-schema reader derives stable IDs from the pinned migration function,
preserves old master order and generations, and keeps ambiguous instrument
entries unresolved. Local tests cover the broad persistence contract, focused
1.13 round-trip and old-schema migration, atomic failed upgrade, model undo
and allocation identity.

## Commands, graph and failure semantics

Insert, move, remove and bypass are project transactions. One completed edit
creates one undo step; drag gestures coalesce, and undo restores slot order,
state and associated automation. Move and bypass preserve slot and plugin
IDs. Explicit removal atomically removes the slot and automation attached
to its native slot ID or VST3 plugin ID. If automation is nonempty, the UI
asks for confirmation before dispatch; undo restores both. Unavailable
plugins stay in the chain until explicitly removed. Commands validate the
whole candidate snapshot before commit.

The graph compiler places prepared slots in saved order on the selected path
before outgoing edges, including group and return buses and master. It
accounts for latency at each slot and recalculates routing PDC for accepted
topology or latency changes. Runtime ownership follows slot plus exact
processor identity/state compatibility, never vector index. One stateful
instance cannot be processed concurrently by two graphs. Off-callback
preparation, held-boundary publication, bounded tail handling, delay-line
priming and retirement follow existing engine contracts. A state/tail that
cannot be preserved within those bounds causes an explicit failed change
rather than a silent reset. The prior audible graph remains active, while
the saved/model revision and failed application status remain visible.
Missing processors cannot silently disappear from the audible route.
The saved `missing` flag records the last known compatibility status. A fresh,
exact scan and verified runtime binding may restore the processor without
changing its saved identity or chain position; an unbound slot still prevents
the route from becoming active. Export applies the same fresh-verification
rule before opening an output writer.

Playback and offline export evaluate the same slot order, bypass, automation
timing, routing and PDC semantics. Unsupported chains fail explicitly;
successful export cannot omit a saved processor or truncate a declared tail.

## Sequence and closure evidence

The arrangement grid fix may ship independently. A chain surface may show
truthful read-only saved items before editing. Drag/drop and keyboard chain
editing wait for one project model, codec migration, graph publication and
undo contract. Current bounded live VST3 and fixed native master paths do not
demonstrate arbitrary ordered chains.

The first implementation gate is a separately reviewed model/codec migration
with stable IDs, limits, old-schema fixtures and atomic undo commands.
Graph/runtime ownership and offline export then consume the unified model,
followed by editable Quick controls. Do not persist a half-populated slot
list beside legacy fields during this transition.

Closure requires native/VST3 order and bypass audio fixtures on track, bus
and master; processed master/stem and tail tests; PDC transitions and
old-graph preservation on failed publication; missing-plugin retention;
undo/reorder/save/reopen and migration recovery; keyboard parity; and
1080p/4K usability checks. No new third-party runtime is implied.
