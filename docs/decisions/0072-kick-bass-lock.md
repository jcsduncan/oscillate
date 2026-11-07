# ADR-0072 — Kick & Bass Lock: Anvil and Nova read each other's notes

**Status:** Accepted; implemented. Align is deferred (see below). **Date:** 29 October 2026.
**Builds on:** ADR-0057 (Nova), ADR-0058 (effects keyed by notes, read ahead), ADR-0060 (Anvil), ADR-0062 (denormals flushed).
**Contract:** `include/oscillate/native_dsp/synth/lock.hpp` (the pair, `LockNote`, `LockState`), `SynthEngine::lock_follows` / `lock_enabled` / `lock_envelope` and `SynthKernel::locate` / `adopt_lock` in `runtime.hpp`, `NativeSynthPlayer::refresh_lock` in `player.hpp`. Parameters: `anvil.lock` (Anvil's table) and `nova.lock.on`, `nova.lock.duck`, `nova.lock.crossover`, `nova.lock.mono` (spec 08a §2.12a). Design: `docs/ui/kick-bass-lock-mockup.html`.

## Context

Below about 120 Hz a kick's tail and a bassline are two near-sines in the same space. When their pitches disagree they beat; when their phases disagree they cancel, so the low end goes thin, changes from bar to bar and falls apart in mono. Producers fix this by hand: they tune the kick to the song, sidechain the bass to the kick with a compressor, and put a mono-maker on the bass. Each tool works from audio, after the fact. A sidechain compressor needs a detector, attack and release, and either lookahead latency or a late duck. It also only knows how loud the kick is, not what it is.

Both instruments are ours. The project holds every note either plays, and Anvil can compute the exact amplitude of each of its hits from its own drawn curve.

## Decision

1. **Notes ahead, not audio.** An instrument whose engine sets `lock_follows` reads the notes of every unmuted track playing that engine, compiled from the project (`engine::compile_midi_events`) on the control thread into an immutable `LockState`, the way Pump reads its keying notes (ADR-0058).
   - The runtime takes it through a mailbox (`NativeSynthRuntime::offer_lock`), retires old states only after the callback has adopted a later one, and adopts at a block start. Nothing allocates or waits on the callback.
   - `SynthKernel::locate(project_frame)` runs before every note and render segment, so a kernel knows where it is in the followed notes for any block split.
   - The same `NativeSynthPlayer` compiles it for live playback and for export, so they agree exactly.
   - **Why not audio.** An audio sidechain cannot know a kick before it sounds without latency, has a detector to tune, and gets nothing from a bass about which note it is playing. Read ahead, the duck starts on the hit's first frame and Anvil knows the bass note before its own hit starts.
2. **Pairing.** Anvil follows Nova (`lock_bass_engine`); Nova follows Anvil (`lock_kick_engine`). A pair forms only when Lock is on at both ends: a track compiles another's notes only if its own Lock and the other's are on. A muted track is not followed. Several kicks or basses all take part.
3. **Anvil: the tail follows the bass (`anvil.lock`, Off / Bass note / Keep tone).**
   - **Keep tone** (added 30 October 2026, for Mix Check, ADR-0074): a retuned tail changes the kick's character, so locking without retuning is offered. Any choice but Off locks Anvil, so the pair forms and Nova ducks and monos its lows under each hit; only Bass note also retunes. The choice was appended, so saved Off (0) and Bass note (1) keep their meaning. `lock_kick_bass_note` and `lock_kick_keep_tone` in `lock.hpp` name them.
   - **Which note.** The bass note under the hit: the latest one still sounding at the hit's frame. If none sounds, one starting within a sixteenth after the hit (`lock_lookahead_beats`; 0.125 s with no tempo) counts, since a bass often lands just after the kick.
   - **The tune rule.** The tail moves to the root or the fifth of that note, in whichever octave lies nearest the drawn tail (after Tune), choosing whichever of the two is nearer in log frequency. A fifth is consonant with the root, and allowing it keeps the tail within half an octave of where it was drawn, so the kick's character survives. The whole drawn sweep transposes with it, so its shape is kept.
   - **No bass under the hit:** the drawn tail.
4. **Nova: duck and mono below a crossover (`nova.lock.*`).**
   - **The split.** A fourth-order Linkwitz-Riley pair at the crossover (60–250 Hz, default 120). Low and high bands are in phase and sum to an allpass, so with nothing ducked the level is flat at every frequency.
     - A "remainder" high band (input minus low) was rejected: the low-pass's phase shift leaves most of the low band in the remainder. Folding the low side to mono then barely removes it, and a duck boosts the lows just under the crossover.
     - The price of the true pair is the allpass's phase rotation around the crossover while Lock is on. It is inaudible on a bass, and Lock off runs nothing, so off is bit-exact.
   - **Duck.** The low band's gain follows `1 − Duck × (the loudest followed hit's amplitude)`. Each amplitude is Anvil's own body envelope for that hit's velocity (`SynthEngine::lock_envelope`, one value every `lock_envelope_frames`). It is smoothed only to avoid a click (0.5 ms down, 2 ms up), so the dip starts on the hit's frame and ends with it. There is no detector, threshold, attack, release or lookahead to set. The mids and top of the bass keep playing.
   - **Mono Lows.** The low band folds to mono, so the club system sums what the producer heard. Above the crossover the stereo image is untouched.
   - **Switching.** Lock fades in and out over 20 ms. Duck and Crossover are global modulation destinations.
5. **One click.** `ProjectSession::lock_kick_and_bass(bass, on)` switches the Nova bass's Lock and every unmuted Anvil track's Lock together as one model `apply`: one revision and one undo step. A kick it turns on takes Keep tone; a kick already on Bass note stays there, and either reads locked. `kick_bass_lock_state` drives the track menu. "Lock kick & bass" / "Unlock kick & bass" shows on Nova tracks while the project has an Anvil track. Both panels carry the switch: Anvil's Master card and Nova's KICK LOCK card on the GLOBAL page.
6. **Live.** After every published snapshot (a rebuilt graph, or a synth's live value change being acknowledged), every live synth player re-offers its `LockState` (`RoutedAudioSource::refresh_synth_locks`). A bass edit or the other track's Lock switch then reaches a kept player without a rebuild. The compile runs on the control thread and only allocates there.

## Deferred: Align

The design also proposed phase alignment: the later sound starts in phase with the one already playing. It is not built.
- Alignment derived from notes would assume the bass's waveform phase at a note start is known. Nova's own FX chain (filters, drive, the rack's EQ and phasers) shifts that phase by an amount that depends on its settings, its modulation and the note. So the alignment would be approximate exactly where it matters.
- Doing it properly needs the bass's rendered phase near the crossover at the kick's start: an analysis of the bass's output ahead of the kick, which live playback cannot have without latency.
- Revisit with a measured approach, for example an offline analysis pass whose result the kick reads, if Tune and Duck leave audible cancellation in real mixes.

## Consequences

- Two engines know each other's ids through `lock.hpp`. Further pairs (a Pulse kit kick, another bass engine) would need `lock_follows` to name more than one engine.
- `LockState` holds every followed note and one envelope per distinct velocity. That is small, but it is recompiled for each locked player on every published snapshot.
- Tests:
  - `native_lock_contract`: Nova off is bit-exact, levels are kept, Mono Lows works, the duck has no latency, and block splits render alike. Anvil's tail lands on the root or fifth, and block splits render alike. Keep tone locks without retuning (`anvil_keep_tone_locks_without_retuning`).
  - `application_kick_bass_lock`: one undo step, the menu state, a kept player following a bass edit, and a locked pair's export nulling against live playback.
