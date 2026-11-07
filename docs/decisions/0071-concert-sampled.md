# ADR-0071 — Concert becomes a sampled grand: the Salamander Grand Piano, streamed from disk

**Status:** Accepted; implemented (engine, library, install script and panel).
**Date:** 29 October 2026.
**Supersedes:** ADR-0036 (the physically modelled Concert). Keeps ADR-0035's pedal semantics and ADR-0037's telemetry channel.

## Why

Measured against recordings of a Steinway, the modelled Concert was badly wrong: an organ-like, fundamental-heavy bass; decays about twice too fast and without a two-stage decay; metallic undamped partials; a glassy treble. A freely licensed sampled grand was judged the faster route to a good piano than refitting the model; one good instrument is enough.

## Decisions

### One library: the Salamander Grand Piano V3

- **What:** a Yamaha C5 by Alexander Holm, CC-BY 3.0, in the sfzinstruments edition (48 kHz / 24-bit FLAC, commit `3382bf94`).
- **Contents** (641 files, 748 MB of FLAC):
  - 480 notes: 16 velocity layers, sampled every minor third from A0;
  - 88 key-release (hammer and key mechanism) noises, one per key;
  - 69 string-resonance releases in three layers (loud, soft and a quiet third), by minor third;
  - four sustain-pedal noises, two down and two up.
- **Attribution:** "Salamander Grand Piano by Alexander Holm, CC-BY 3.0". It appears in the panel (library plate and About), in `ATTRIBUTION.txt` beside the library, and in the manifest. The licence and the README are copied in with it.
- **One piano only;** no other libraries.

### Memory: the starts in RAM, the rest streamed

The numbers decide it:

| Content | Audio | int16 stereo | float stereo |
| --- | ---: | ---: | ---: |
| Note samples | 6,595 s | 1,208 MiB | 2,415 MiB |
| Release, resonance and pedal samples | 213 s | 39 MiB | 78 MiB |
| **All** | 6,809 s | **1,247 MiB** | 2,493 MiB |

- **Compact RAM does not fit.** Even 16-bit is 1.25 GB, twice the ~600 MB ceiling. The samples are already trimmed to their noise floor (C4's last frame above −80 dBFS is within 0.2 s of its end), so there is no tail to cut.
- **So Concert streams.**
  - The first 16,384 frames (0.34 s) of every note stay in memory (31 MiB).
  - The release, resonance and pedal samples stay in memory whole (39 MiB).
  - **About 70 MiB is resident per process**, shared by every Concert.
  - Each kernel adds 160 stream rings of 16,384 frames (10 MiB).
- **Streaming design:**
  - **Rings.** Every note voice owns a stream slot. A process-wide streamer thread fills the slot's ring with the rest of the sample, in 4,096-frame positioned reads (`pread`), up to 0.34 s ahead of the voice. It looks every 2 ms.
  - **No waits on the callback.** The callback never locks, allocates or touches the disk. A slot's request, progress and consumption are each one atomic word tagged with a generation, so a voice never reads data meant for an earlier note in its slot.
  - **Late data.** A frame that has not arrived plays as silence and counts as an underrun. Telemetry reports underruns, and the panel shows them.
- **Offline renders never outrun the disk.**
  - The platform gains `SynthKernel::await_content()`, reached through `NativeSynthRuntime::await_content()` and `NativeSynthPlayer::await_content()`.
  - Offline export (`offline_master.cpp`, `vst3_instrument_offline.cpp`) and the test render helper call it between blocks. Concert then services its own streams inline, so an offline render is deterministic and as fast as the disk allows.
  - The realtime callback never calls it.
- **Libraries are never freed.** A loaded library stays in memory until the process exits, because a sounding kernel may still read it. A library missing at start can be installed and loaded later; a ready one is not replaced. A process can load at most 255 libraries, a limit only tests approach.

### The installed format and the install step

- **Format.** `tools/concert/install_salamander.py` (Python standard library, git and ffmpeg) writes:
  - `concert.pack`: interleaved little-endian int16 stereo at 48 kHz, each sample aligned to 4 KiB;
  - `concert.manifest`: one text line per sample, giving kind, key range, root, velocity range, pack offset, frame count, scale, recorded peak, the SFZ's level, velocity tracking and per-second release decay.
- **Why convert.** The pack needs no decoder: the Debug and Quick builds have no FLAC codec (ADR-0016 keeps libsndfile optional and worker-only), and a raw pack streams with plain positioned reads.
- **The conversion:**
  - the silence before the attack is trimmed to 2 ms (at −40 dB re the sample's peak) and faded in, so all layers start together and play promptly;
  - each sample is peak-normalised to −0.2 dBFS and TPDF-dithered to 16 bits. The error against the 24-bit source measures −84 dB re the signal, below the recordings' own noise floor (−67 to −83 dBFS in the tails measured).
- **Integrity.** The script fetches the pinned commit with git. Git hashes every object, so the pinned commit and tree ids are the integrity check. The script also verifies the 641-file set and the SFZ mapping against the files.
- **Install location.** The library is built beside its destination and replaces an old one only once complete. The destination is `$OSCILLATE_CONCERT_LIBRARY`, else `$XDG_DATA_HOME/oscillate/instruments/concert/salamander-grand-v3` (`~/.local/share` by default). That follows the presets, racks and grooves convention, and Concert looks in the same place (`concert::library::default_directory`).
- **Loading.**
  - The first Concert kernel loads the library on the control thread (manifest checks, then about 70 MiB of resident reads).
  - The manifest and pack are untrusted. Every range is bounded, the pack's size must match, and every key must have a note.
  - A missing or invalid library is looked for again when the panel asks or a Concert starts, if the directory has changed.
- **Why a script, not an in-app download.** An in-app "Download piano" action is nicer, but needs a network client and the conversion inside the app. The script is reproducible and keeps both out of the app. The panel shows the exact command, with a copy button and Look Again.

### Playback

- **Mapping.**
  - Each key plays its region's sample, resampled up to one semitone from the root, plus Tune (±50 cents).
  - The resampler is a 32-tap Kaiser-windowed sinc (β = 8) with 256 phases, linearly interpolated.
  - Rising notes, and sessions below 48 kHz, use a table cut below the band the steepest step leaves.
  - Notes at their root at 48 kHz are copied exactly.
- **Velocity.**
  - Touch shapes velocity (a power curve).
  - Every blow then sounds the two layers whose velocity centres surround it, with linear (equal-gain) weights. The layers' attacks are aligned and largely correlated, so the level rises smoothly with no step at a layer boundary.
  - Dynamics scales each layer's level relative to the key's loudest: 0 % evens them, 100 % is as recorded, 150 % widens the range.
- **Dampers** (ADR-0035 semantics):
  - **Lift.** A held key, a sostenuto-captured key or the sustain pedal lifts a key's damper. Half-pedal lifts it along a smoothstep from 20 % to 80 % of the pedal's travel. Dampers clear in 5 ms and land in 20 ms. Keys above the library's `last_damped_key` (E6 for the Salamander) have no damper.
  - **Decay.** A damped note decays on top of its recording with a register-dependent T60: 1.0 s at A0, 0.4 s at C4 and 0.15 s at E6, scaled by Dampers. Half-pedal damps proportionally.
  - **Repeated keys.** A key struck again fades its earlier sound over 100 ms.
- **The mechanism's recordings.**
  - **Key release.** The key's release noise sounds when the key rises, whatever the pedals do, because it is the key and hammer settling.
  - **String release.** The string-resonance release sounds when a damper lands on a sounding string: at key-up without the pedal, or at pedal-up.
  - **Level.** Both use the SFZ's level, velocity tracking and per-second decay of the time the note was held.
  - **Sustain pedal.** It plays its down and up noises in turn.
  - **Controls.** Key Release, String Release and Pedal set their levels; 50 % is the SFZ's own voicing.
- **Una corda.** The soft pedal moves a blow down by 12 velocity steps (lower layers), 1.5 dB quieter and up to 6 dB darker above 1.2 kHz, scaled by the Una Corda control.
- **Sympathetic resonance.** The library has no pedal-down resonance recordings (its "harm" samples are the release resonances above), so Concert adds a simple model:
  - **The loops.** One tuned string loop per key: a fractional delay with a one-pole lowpass in the loop. Each loop's input is normalised so its peak gain is one.
  - **Excitation and damping.** Everything that sounds excites the loops. A loop runs freely while its key's damper is up (T60 8 s in the bass to 1.5 s in the treble) and is damped to 80 ms when the damper is down.
  - **What you hear.** Pressing the pedal makes the instrument bloom, and silently held keys ring in sympathy.
  - **Placement and level.** The loops are spread from bass left to treble right. Resonance sets their level: 100 % is −12 dB, and the default is 30 %.
  - **Cost.** Sleeping loops cost nothing.
- **Output.** Tone is a first-order tilt about 700 Hz (±6 dB). Width scales the side signal. Perspective mirrors the stereo image through a 20 ms crossfade. Level is applied after a fixed −9 dB headroom (`concert::headroom_db`), then a safety knee above −1 dBFS.

### Parameters and presets

- **Thirteen parameters** in five groups:
  - Voicing: Tone, Touch, Dynamics;
  - Dampers & Pedals: Dampers, Una Corda, Resonance;
  - Mechanics: Key Release, String Release, Pedal;
  - Stereo: Width, Perspective;
  - Output: Tune, Level.
- **Next-note controls.** Touch and Una Corda choose a blow's layers, so they apply from the next note and are not automatable. Everything else is live and automatable.
- **Presets:** Concert Grand (default), Bright, Soft, Close, Felt, Audience and Dry.
- **No schema change.** Synth values persist as sparse named values per engine (ADR-0027), so the project schema is unchanged. The engine keeps id `concert` and table version 1 (greenfield):
  - a saved Concert patch with only default values loads as the new Concert Grand;
  - one with the old modelled ids is refused as an unknown parameter.

### Telemetry

- **Layout** (`concert::telemetry`):
  - the three pedals;
  - each key's energy (its voices plus its resonance loop, log10 relative to a mezzo-forte note) and damper contact;
  - the library's state;
  - the count of underruns.
- **Panel.** The panel's strings view shows these live. When nothing is playing, it shows each key's sample length.

### What was removed

`piano_dsp.{hpp,cpp}`, its contract (`native_piano_dsp_contract`), the partial and sustain sketches (`concert_partials`, `concert_sustain`, `ConcertPartialView.qml`) and the instrument-type selector. The modal library (`modal_dsp`) stays, for Knock; its contact-law test now checks the Stulov formula directly.

## Tests

- **`native_concert_contract`** runs on a synthetic library in the installed format (`tests/native_dsp/concert_test_library.hpp`): decaying harmonic tones at each region's root in three velocity layers, noise clicks, resonance tones and pedal thumps. It covers:
  - a missing library: silent, says why, bounded tail;
  - malformed libraries refused: format, truncated pack, out-of-pack sample, a key without a note, sample rate, a pack path outside the library;
  - the registry and presets;
  - tuning within half a cent up and down a semitone and with Tune;
  - the resampled waveform against the source read at the new rate, with less than −70 dB error;
  - velocity 1–127 rising monotonically with no step over 0.6 dB;
  - streaming bit-exact across the resident/streamed join;
  - the pedals: sustain, half-pedal, sostenuto, key release at the key and string release at pedal-up;
  - una corda quieter and darker;
  - block splits identical;
  - no allocation on the callback;
  - the tail bound;
  - sane presets;
  - telemetry;
  - goldens.
- **Other tests.** The application export-parity and routed-synth tests, the Quick integration test and the panel test install the synthetic library with notes short enough to stay resident, so live rendering in tests never depends on disk timing.

## Not verified

The engine has not been built or listened to. Still open:
- the default voicing figures (headroom, resonance level and loop T60s, damper T60s, soft-pedal shift) against reference recordings;
- the stereo orientation of the Salamander's AB pair, which is assumed to be the player's perspective;
- CPU cost and underruns under load;
- the goldens, which need a first `CONCERT_REBASELINE=1` run.
