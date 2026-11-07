# Spec 09 — Crate, the chop sampler

Decision record: [ADR-0063](../decisions/0063-crate-chop-sampler.md). Panel: [crate-panel-mockup.html](../ui/crate-panel-mockup.html).

## 1. Contracts shared by every part

These are frozen before the parts are built in parallel. Change them only here, first.

### 1.1 Model (schema 1.44)

```cpp
// include/oscillate/project/model.hpp
inline constexpr std::size_t max_native_regions = 128;      // per document
inline constexpr std::size_t max_native_region_name = 64;   // UTF-8 bytes
inline constexpr std::size_t max_native_region_tag = 16;
struct NativeRegion {
  std::uint32_t slot = 0;          // stable id, unique in the document
  std::string media;               // a NativeMediaSlot id in this document
  std::uint64_t start_frame = 0;   // in the media's source frames
  std::string tag;                 // engine vocabulary; Crate: kick|snare|hat|other
  std::string name;                // shown on the pad; may be empty
  bool operator==(const NativeRegion&) const = default;
};
// NativeSynthDocument gains: std::vector<NativeRegion> regions;  (empty() includes it)
```

Model validation (`validate_native_document`): at most `max_native_regions`; slots unique; `media` names a media slot of the document; within one media, `start_frame` strictly increasing in vector order; name and tag within their limits. The engine checks the rest (Crate: slot < 32, at most 32 regions, all in media `sample`, start inside the decoded sample, tag in its vocabulary).

Codec: `"regions": [{"slot", "media", "start_frame", "tag", "name"}]` in the document object, omitted when empty. Schema 1.44 also removes `SamplerPatch` (`"kind": "sampler"`).

### 1.2 Session

```cpp
// One undo step: the patch's flat values and document together (re-chop).
Result<void> set_native_instrument_state(project::TrackId track,
    std::vector<project::NativeInstrumentParameter> values,
    project::NativeSynthDocument document, std::optional<GestureId> gesture = {});
// One undo step: a MIDI clip with its notes; returns the new clip's id.
Result<project::ClipId> add_midi_clip_with_notes(project::TrackId track,
    timeline::TickPosition start, timeline::TickPosition length,
    std::vector<project::MidiNote> notes);
```

Media import gains `begin_native_synth_media_import(track, slot, fd)` → a task run on a worker, then `finish_native_synth_media_import(prepared, regions = nullopt)` on control (one undo step, as `import_native_synth_media` today), taking WAV, and MP3/FLAC in codec builds. Given `regions`, they replace the document's regions in that slot in the same step (a new sample and its first chops together); given `NativeSynthMediaState{values, labels}`, the patch's sparse values and the document's labels are replaced in that step too (the new sample's root, reset chop parameters, `crate.bpm` and `crate.sample`); the synchronous `import_native_synth_media(track, slot, fd, regions)` runs all three for short or bundled sounds.

`source_prep::load_sampler_source` / `SamplerSource` are renamed `load_managed_audio` / `DecodedAudio`; synth media depends on them.

### 1.3 Band-limited reader (public)

`include/oscillate/warp_dsp/bandlimited.hpp` promotes the warp core's kernel: a kernel per quality tier, a read plan for any speed (cutoff scaled by 1/speed above 1), and a read from planar or interleaved stereo at a fractional position. `noexcept`, allocation-free, callback-safe. At whole positions and speed 1 the read is the identity (bit-exact oracle).

## 2. The engine (`crate`, version 1)

### 2.1 Global parameters

| id | range | default | scale / unit | live? |
|---|---|---|---|---|
| `crate.layout` | pads, keys | pads | choice | yes, not automatable |
| `crate.keys_chop` | 0–31 | 0 | step (slot) | yes, not automatable |
| `crate.root` | 0–127 | 60 | step (MIDI key of the sample) | yes, not automatable |
| `crate.time` | repitch, follow | repitch | choice | next note |
| `crate.era` | clean, sp12, mpc60, s950, tape | clean | choice | yes, not automatable |
| `crate.bits` | 4–24 | 24 | step, bits | yes |
| `crate.rate` | 4000–48000 | 48000 | log, Hz | yes |
| `crate.drive` | 0–1 | 0 | linear | yes |
| `crate.vinyl` | 0–1 | 0 | linear | yes |
| `crate.wow` | 0–1 | 0 | linear | yes |
| `crate.output` | −24–+12 | 0 | dB | yes |
| `crate.swing` | 50–75 | 50 | % | yes |
| `crate.velocity` | 0–1 | 0.7 | linear (velocity sensitivity) | yes |
| `crate.voices` | 1–32 | 16 | step | next note |

Choosing an era sets `bits`, `rate`, `drive` and the era's fixed stages (§2.4); the knobs stay editable after.

Document labels: `crate.bpm` (the sample's tempo, a decimal BPM in 20–999) and `crate.sample` (the loaded file's name, non-empty, for the panel); each at most once.

### 2.2 Per-chop parameters (`crate.chop.NN.*`, NN = slot 00–31)

| control | range | default | scale / unit | automatable |
|---|---|---|---|---|
| `pitch` | −48–+48 | 0 | semitones | yes |
| `fine` | −100–+100 | 0 | cents | yes |
| `gain` | −48–+12 | 0 | dB | yes |
| `pan` | −1–+1 | 0 | linear | yes |
| `start` | 0–1 | 0 | share of the chop | yes |
| `length` | 0.01–1 | 1 | share of the chop | yes |
| `attack` | 0–2000 | 1 | ms, log | yes |
| `hold` | 0–5000 | 0 | ms, log | yes |
| `decay` | 1–5000 | 5000 | ms, log | yes |
| `sustain` | 0–1 | 1 | linear | yes |
| `release` | 1–5000 | 30 | ms, log | yes |
| `cutoff` | 20–20000 | 20000 | Hz, log | yes |
| `resonance` | 0–1 | 0 | linear | yes |
| `reverse` | off, on | off | choice | no |
| `mode` | oneshot, gate, loop | oneshot | choice | no |
| `filter` | off, lp, hp, bp, notch | off | choice | no |
| `choke` | 0–8 | 0 | step (0 = none) | no |

### 2.3 Playing

- **Pads layout.** Region *i* in time order is pad *i*; key 36 + *i* plays it (bank A: 36–51, bank B: 52–67). Other keys are silent.
- **Keys layout.** Every key plays the region in slot `crate.keys_chop`, repitched from `crate.root`.
- **Modes.** One-shot plays to the chop's end whatever the note-off (release starts at the end). Gate releases at note-off. Loop repeats start–end until note-off, with a 2 ms crossfade at the loop point.
- **Reverse** reads from the end of the chop to its start.
- **Choke.** A note-on in group *g* > 0 releases every voice of group *g* with a 5 ms fade.
- **Envelope** AHDSR on each voice; velocity scales gain by `crate.velocity`.
- **Filter** per voice: the platform's state-variable filter in the chosen mode.
- **Voices** up to `crate.voices`, oldest released stolen first. Retriggering a sounding chop in one-shot or gate mode restarts it (with a 2 ms fade on the old voice).
- **Swing** delays every second sixteenth by (swing − 50)/50 of a sixteenth, from the transport; note-offs follow their note-ons.
- **Time.** Repitch reads at the pitch ratio × source rate / project rate. Follow Tempo keeps pitch from `pitch`/`fine` and stretches by project tempo / the sample's tempo (from the source's reviewed BPM, carried in the document's label `crate.bpm`), with a grain overlap-add that restarts at the chop start.

### 2.4 Eras

| era | bits | rate | read | anti-alias | output |
|---|---|---|---|---|---|
| clean | 24 | 48000 (bypassed) | band-limited | — | clean |
| sp12 | 12 | 26040 | nearest sample at the era clock | none | hard-ish clip |
| mpc60 | 12 | 40000 | linear at the era clock | gentle low-pass | soft saturation |
| s950 | 12 | 31250 | linear at the era clock | resonant low-pass tracking the rate | soft |
| tape | 24 | 48000 (bypassed) | band-limited | high-shelf roll-off | tape saturation, wow and flutter |

`vinyl` adds seeded crackle and hiss; `wow` adds slow pitch drift. Starting figures above are placeholders until measured (`tools/crate_reference`, ADR-0063 §9).

### 2.5 Presets

Init, and the four starter sounds (Kick, Snare, Hat, Pluck: one chop each, bundled samples), plus era showcases on a bundled break once sourced under a bundling licence.

## 3. Chopping (application)

`include/oscillate/application/crate_chops.hpp`, pure and deterministic over a decoded sample:

- `chop_transients(audio, sensitivity 0–1)`; `chop_grid(audio, bpm, division 4|8|16, first_beat_frame)`; `chop_equal(audio, n)`; manual edits add, move and remove starts.
- Results are at most 32 starts, strictly increasing, the first at frame 0 (a sample always plays from its beginning on pad 1). Starts closer than 10 ms merge.
- `classify(audio, start, end)` → kick (low band dominant, fast decay), snare (mid band and noise), hat (high-frequency share), other.
- `detect_root(audio)` → median voiced pitch, else 60.
- Re-chopping keeps the slots of regions whose start is unchanged and assigns free slots to new ones; a dropped slot's parameters reset.

## 4. Chop to MIDI

One clip on the Crate track at the playhead: region *i* as a note on key 36 + *i*, at its start converted to ticks at the sample's tempo, lasting to the next region's start, velocity 100. Length rounds up to whole bars.

## 5. Panel

To the approved mockup: header (sample, tempo, key, era, time), coloured waveform with chop markers (drag to move, double-click to remove, click empty space in Manual to add), chop-mode bar with Chop to MIDI, pads (bank A/B), the selected chop's controls, and the character section. Dropping an audio file on the panel loads it.

### 5.1 Panel contract

`CratePanel.qml` (objectName `cratePanel`, in `qml/crate/`) reads the generic `NativeSynthDeviceController` for every parameter (`crate.*`, and the selected chop's `crate.chop.NN.*` through `chop_parameter`) and `controller.crate`, a `CrateController` that exists only when `engine_id` is `crate` (null otherwise). Everything `CrateController` shows derives from the document, the values and the decoded sample; it holds no truth of its own beyond the panel-only state marked *(panel)*.

Properties (all notify `changed`):

| property | meaning |
|---|---|
| `has_sample` | the `sample` slot holds managed or built-in audio that decoded |
| `sample_name` | the source's file name (the document's `crate.sample` label, written on load), or the built-in name |
| `sample_seconds`, `sample_rate` | length and source rate |
| `bpm` | the `crate.bpm` label, else 0 |
| `key_name` | the detected root as a note name ("F2"), or empty |
| `chops` | regions in time order: `[{index, slot, start, end (0..1 of the sample), start_seconds, length_seconds, tag, name, key, key_name, bank}]` |
| `selected` | index into `chops` *(panel)*; writable; clamps |
| `bank` | 0 (A) or 1 (B) *(panel)*; writable |
| `chop_mode` | "transient", "grid", "equal" or "manual" *(panel)*; writable, does not re-chop by itself |
| `sensitivity` (0..1), `grid_division` (4, 8, 16), `equal_count` (1..32) | the chop settings *(panel)* |
| `busy`, `message` | an import or decode in flight; the last refusal or progress text |

Invokables:

- `peaks(columns)` → `[max |sample|]` per column over the whole sample, peak 1 normalised to the loudest column; empty without a sample.
- `chop_parameter(index, control)` → the id `crate.chop.NN.<control>` of chop `index`'s slot (empty when out of range).
- `chop()` re-chops with `chop_mode` and its setting (manual keeps the starts); `chop_transients(sensitivity)`, `chop_grid(division)`, `chop_equal(count)` set the mode and re-chop. Each re-chop is one undo step (values and document, §1.2), keeps slots per §3 and keeps `selected` on the same start when it survives.
- `add_chop(fraction)`, `remove_chop(index)` (index 0 refuses), `rename_chop(index, name)` — one undo step each.
- `move_chop(index, fraction, phase)` with phase 0 begin, 1 update, 2 commit, 3 cancel: a marker drag is one undo step heard as it moves.
- `set_era(index)` writes `crate.era` and the era's `bits`, `rate` and `drive` (`era_settings`) as one edit.
- `reverse_all(on)` sets every chop's `reverse` as one edit.
- `load_sample(path_or_url)` imports a file into `sample` asynchronously (`busy` while it runs), then chops it with transients in the same undo step as the import, writes `crate.bpm` from the source's tempo (a reviewed tempo of the same content in the project, else the import worker's own analysis at confidence ≥ 0.5), writes `crate.sample` (the file's name), sets `crate.root` from `detect_root` and resets the old and new chops' parameters. False when the file is refused at once. `choose_sample()` asks for a file, then loads it.
- `chop_to_midi(start_bar)` writes §4's clip at zero-based bar `start_bar` (the panel passes the playhead's bar) as one undo step; false without a sample, regions or tempo.
- `pad_key(index)` → the MIDI key that plays chop `index` in the current layout (pads: 36 + index; keys: -1).

The panel auditions pads through the editor's bridge (`play_note(track, key, velocity)` / `release_note(key)`), lights pads from telemetry (`playheads`, `last_slot`), and accepts audio files dropped on it (`load_sample`). An audio file dropped on the arrangement always makes an audio track; a sample reaches Crate through its panel. `ProjectBridge::nativeSynthTrackId` names the track the open panel plays.

## 6. Tests (each part)

- Model and codec: regions round trip, validation, schema 1.44, sampler gone.
- Engine: identity read at unity, reverse, one-shot/gate/loop, choke, AHDSR, aliasing at +24 st in Clean below −80 dB, era rate and bits measurable, no allocation on the callback, block-size invariance, export equals live.
- Chopping: transient positions on synthetic clicks, grid and equal arithmetic, classification on rendered kicks, snares and hats, slot preservation on re-chop.
- Session: re-chop and Chop to MIDI are one undo step each; import is asynchronous.
- Panel: loads, edits a chop, bank switch, drag-drop.
