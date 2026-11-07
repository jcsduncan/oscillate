# ADR-0077 — Nova Constellation: browse presets by sound, morph between them

**Status:** Accepted; implemented. **Date:** 31 October 2026.
**Builds on:** ADR-0057 (Nova), ADR-0058 (user presets), ADR-0062 (denormals flushed), ADR-0073 (Nova Analogue).
**Panel design:** the Constellation part of [ui/nova-redesign-mockup.html](../ui/nova-redesign-mockup.html). ADR-0076 is the panel redesign around it.

## Context

A list of sixty preset names says little about how they sound. The design puts every preset in a night sky, dark to bright across and short to long up, and lets the user click or drag between stars: what plays is a blend of the three nearest, every knob in between, heard live while a chord is held. KEEP MINE holds the user's FX, envelopes or routes while the rest morphs; MUTATE scatters six variations around the sound; KEEP THIS saves the blend as a user preset, which becomes a star.

The design placed stars by hand-written numbers. The real one has to place them by how each preset actually sounds, and a morph has to blend a real Nova patch: about a thousand parameters and a structured document (routes, curves, effect chains, media, labels, voice lanes).

## Decision

1. **Listening (`application::sound_map`).** Each sound is rendered offline through the same `NativeSynthRuntime` playback uses, at 48 kHz in 128-frame blocks at 120 BPM, with denormals flushed. The phrase depends on the preset's category: a bass holds C2; everything else holds a C3 + C4 dyad. Both play at velocity 100, held 1 s, then released, 2.5 s in all. Sounds whose documents hold imported audio are not rendered (there is no project to read the audio from), so they have no star.
2. **Measurements (`measure`, a pure function of the frames).**
   - **Brightness:** the magnitude-weighted log2-frequency centroid of the mid signal over 2048-sample Hann frames (hop 1024), 20 Hz–20 kHz, bins more than 80 dB under the frame's peak ignored. It is averaged over frames by frame energy and scaled 0 (20 Hz) to 1 (20 kHz).
   - **Length:** 0.35 × the attack (time to 90 % of the peak 10 ms RMS envelope, over the held second) plus 0.65 × the time the envelope stays within 30 dB of its peak, over the whole render. A pluck that decays while held and a pad with a long release land at opposite ends.
   - **Movement:** the mean L1 flux between successive frames' band shapes (24 log bands, 50 Hz–16 kHz, each frame normalised to sum 1). It covers the held part after the attack, skipping frames 40 dB under the loudest, and is halved into 0..1.
   - **Width:** side energy over mid plus side energy.
   - A render under −100 dBFS RMS is inaudible and has no star.
3. **Placement.** x is brightness and y length, each scaled between the library's 5th and 95th percentiles of audible sounds and clamped, so one outlier does not crowd the rest into a corner. Movement and width scale the same way and drive each star's twinkle depth and size. Variations are placed against the library's percentiles.
4. **Cache.** Measurements live in `$XDG_CACHE_HOME/oscillate/constellation/nova.json` (else `~/.cache`), keyed by 64-bit FNV-1a over the analysis version, the phrase and the sound's canonical preset text (engine, version, sparse values, document). Opening again reads the file and renders nothing. A saved, renamed or edited user preset gets a new key, so only it is listened to again. A completed listen forgets keys the library no longer holds. An unreadable, oversized (> 4 MiB) or other-version file reads as empty. Bumping `analysis_version` redoes everything. The cache is never the truth; deleting it only costs a re-listen.
5. **Threads.** Listening runs on a worker that renders on up to four threads (half the cores), so the GUI thread only places stars as results land and shows "LISTENING TO PRESETS… n/total". A new listen or the view going away stops the old one between blocks.
6. **Morph (`application::nova_morph::morph`).** Up to three weighted sounds (the view weights the three nearest stars by inverse square distance in pixels, plus one pixel) blend into one:
   - continuous parameters in normalised knob space, so log scales and skews blend as the knobs travel;
   - an effect slot's `nova.fx.<uid>.*` settings only over the parts holding the same module in that slot as the heaviest part; otherwise the heaviest's;
   - discrete parameters and switches from the heaviest;
   - the document from the heaviest, except that each of its routes' amounts is the weighted sum over the parts' routes with the same source and destination, whatever their slot; a part without that route counts 0;
   - the performance controls (`nova.perf.*`) stay as they are; they are the player's, not a preset's.

   **KEEP MINE** keeps the current sound's part: FX (every `nova.fx.*` value, the chains and the effects' impulse media), ENVELOPES (`nova.env.*`) and MOD ROUTES (the routes and every route amount).
7. **Live and undo.** A drag is one undo step: `begin_state_gesture`, updates, `end_state_gesture` on `NativeSynthDeviceController`, over the adapter's gesture mechanism. Values take the live path (`submit` with `update`), so held notes glide as for a knob drag. The document is submitted (`submit_state` under the same gesture, with the same values) only when it changes, that is when the heaviest star or a lock changes, so the runtime rebuilds the document rarely. The commit writes the final values under the gesture, and the project model coalesces every step into one undo step. A click without a drag is one edit (`apply_state`). Clicking a star itself loads it exactly: a factory or user preset through the existing paths, a variation as one state edit.
8. **Mutate.** Six variations, each moving every mutable continuous parameter by a uniform draw of up to 0.08 of its knob travel from splitmix64 seeded by the sound's key plus a press count. The same sound and press give the same variations. The level, tuning (`*.fine`, `*.pitch`), FX bus returns, performance controls, global plumbing and Kick Lock never move; discrete parameters and the document never move; locks hold as for a morph. Variations are rendered and measured like presets and appear as temporary stars until the next MUTATE.
9. **Keep this** saves the sound now through `save_user_preset` as "Blend N" (the first free N). The saved preset is listened to like any other and becomes a star.
10. **Qt.** `NovaConstellation` is a child of the controller, exposed as the `constellation` property (null for every engine but Nova). It starts listening only when the view first shows. `NovaConstellationView.qml` draws the sky on a Canvas, with a frame timer running only while visible. It shows axis labels, the cursor with lines to its three stars (thickness by weight), the MORPHING readout, KEEP MINE toggles, MUTATE, KEEP THIS, and the listening progress. The panel hosts it in a Loader over the page area.

## Consequences

- The first open listens to every preset once: about 60 renders of 2.5 s. At Nova's measured cost of up to ~20 % of a core per second of a heavy patch, that is about 15 s of CPU, a few seconds on four threads. After that, opening is instant.
- User presets with imported audio, and presets a newer Nova cannot read, have no star.
- A user preset has no category, so it plays the dyad and draws in the neutral colour.
- Measuring is deterministic and pure, so tests check it against synthetic signals with known answers (a sine's centroid is its frequency; mono has no width) and check real renders twice for bit-identical results.

## Tests

- `application_sound_map_features`: centroids of 200 Hz and 4 kHz sines within 0.02 of their own frequency, white noise brighter than filtered noise, pluck < released note < slow swell, mono 0 / independent noise ½ / opposite channels 1 wide, a sweep moving and a steady sine still, silence inaudible, deterministic.
- `application_sound_map_render` (slow): Deep Sub darker than Supersaw Lead, Pluck shorter than Warm Pad, the same preset measuring the same, a user preset's text reading back as its sound, imported media refused.
- `application_sound_map_morph`: knob-space blending, discrete and document from the heaviest, performance controls kept, each lock, route amounts by source and destination with absent as 0, effect slots only over the same module, refusals.
- `application_sound_map_mutate`: seed determinism, modest moves, fixed parameters fixed, locks.
- `application_sound_map_cache`: round trip, retain, corrupt and other-version files, keys following the sound (an edit misses only itself), robust placement.
- `quick_nova_constellation_panel_contract`: the view over a primed cache draws and labels every star; clicking a star loads it; a drag is begin, updates, commit; a lock holds; KEEP THIS adds a star; MUTATE's variations land and load as one edit.
