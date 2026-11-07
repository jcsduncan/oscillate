# Nova performance

`oscillate_nova_bench` (bench/native_dsp/nova_bench.cpp, built with `-DOSCILLATE_BUILD_BENCHES=ON`) times Nova on the scenes that matter and checks every change against saved renders:
- the Analog Pad's overlapping four-note chords;
- a Pluck sixteenth line;
- a four-note chord on every preset;
- a five-note chord (held 4 s, then released) on every Pad preset, against Supersaw Lead and Pluck: the `chord5` scenes.

```sh
cmake --preset release -DOSCILLATE_BUILD_BENCHES=ON
scripts/build.sh build/release --target oscillate_nova_bench
build/release/bench/native_dsp/oscillate_nova_bench                    # timing, 128 and 512 frames
build/release/bench/native_dsp/oscillate_nova_bench --timing chord5     # only the scenes named chord5...
build/release/bench/native_dsp/oscillate_nova_bench --render DIR       # save renders
build/release/bench/native_dsp/oscillate_nova_bench --compare DIR      # check against them
NOVA_TIMELINE=1 build/release/bench/native_dsp/oscillate_nova_bench --scene "preset Wobble Growl"
NOVA_TRACE=t.json build/release/bench/native_dsp/oscillate_nova_bench --scene "Analog Pad chords"
```

## The 26 October 2026 pass

A performance trace (ADR-0061) of a demo project showed an Analog Pad track taking 11 ms of every 21 ms block. Profiling the bench (gprof, with temporary trace spans per voice stage) found these causes:

| Cause | Fix | Output |
|---|---|---|
| Each oscillator ran all 16 unison slots on every sample. Silent slots still advanced phase and ramps through a chain of dependent conversions, and they outnumbered sounding ones about 4 to 1. | Silent slots hold their state. A slot that becomes audible mid-note (a unison count raised during a held note) starts from where it stopped rather than from a phase that kept advancing. Both are arbitrary, and it fades in over 5 ms. | Identical |
| A switched-off oscillator, and a silent sub or noise generator, still rendered every sample. | They hold their state while silent and no warp reads them. | Identical |
| The oscillators were interleaved sample by sample even when none reads another (no FM, PM, AM or RM). | Each oscillator renders a chunk, with unison voices outside the sample loop. Per sample the sums add in the same voice order, so results match bit for bit. Crossed patches keep the per-sample path. | Bit-identical |
| Warps that are off were still called four times per unison sample. | Skipped when off; off returns its input. | Bit-identical |
| The ladder filters ran left and right one after the other. | Both channels go through one SSE2 ladder, with the same operations per lane. | Bit-identical |
| No thread flushed denormals. A decaying tail (Wobble Growl's FX after release) cost ten times what the playing notes did. | ADR-0062: every render entry flushes denormals. | Values below 1e-308 become 0 (−757 dBFS) |

Results on an Intel i7-10700K, GCC 15, quick-opt (Release). Figures are each scene's mean share of one core at 128 frames, with p99 in brackets. The DAW was open while measuring, so the numbers are indicative:

| Scene | Before | After |
|---|---|---|
| Analog Pad chords | 46.1% (73%) | 32.3% (52%) |
| Pluck sixteenths | 14.5% (19%) | 8.5% (14%) |
| Wobble Growl | 79.9% (178%) | 12.7% (23%) |
| Analog Pad | 28.9% (33%) | 22.0% (29%) |
| Pluck | 16.9% (27%) | 5.9% (10%) |
| FM Bell | 17.9% (21%) | 11.7% (20%) |
| Supersaw Lead | 22.3% (29%) | 15.8% (29%) |
| Hoover | 14.7% (20%) | 10.0% (14%) |
| Init | 11.4% (28%) | 4.2% (9%) |
| Deep Sub | 4.8% (9%) | 1.2% (3%) |

What remains on the Analog Pad scene, roughly:
- the oscillators: 36%;
- mixing, filters and amp: 26%;
- the FX rack (chorus and reverb, with its resampler): 23%;
- control ticks: 11%.

[ADR-0085](../decisions/0085-nested-voice-rendering.md) spreads a track's voices across the audio workers, so one heavy Nova track does not set the block's critical path.

## The 1 November 2026 pass: pads as chords

Pads play four- to six-note chords from MIDI clips, and the pad presets were the heaviest Nova tracks. perf is not permitted on this machine (`perf_event_paranoid` 4), so the profile came from a small `SIGPROF` sampler preloaded into the bench, attributing each sample (and each libm sample to its caller) to a function.

| Cause | Fix | Output |
|---|---|---|
| A filter whose settings hold still still ramped by zero steps every control tick and re-derived its coefficients every other oversampled sample. Formant and Vowel cost ten powers and five tangents a derive. | A still ramp keeps the ramp's refresh timing (so later ramps refresh on the same samples) but derives once. The drive curve is re-derived only when the drive moves. | Bit-identical |
| Formant, Vowel and Phaser re-derived every band at 48 kHz while slow LFOs moved FAT or cutoff (Whispered Ahh: a third of its cost). | During a gentle ramp (each of cutoff, resonance and FAT moving at most 1/384 octave or unit in a tick, and staying between the same two vowels) they derive exactly at the tick's ends and move linearly between in their coefficients. Faster ramps, and ramps crossing a vowel, derive at every refresh as before. `VoiceFilter::set_exact` keeps the reference path. | Within (ln 2 / 384)² / 8 of a coefficient for one tick: Whispered Ahh differs by at most −144 dBFS; a slow formant sweep in isolation by −125 dB of its peak. The growls' fast square-LFO formants (Wobble Growl, Neuro Bass) by −110 dBFS (100 dB below their peaks) |
| Each oscillator copied all 16 unison slots, each with a kilobyte BLEP ring, into a local and back on every control tick, silent slots included. | The BLEP oscillators live apart from the slot's motion; silent slots are skipped before the copy. | Bit-identical |
| Wavetable unison voices ran one at a time through a scalar cubic read. | With both warps off and no level fade, voices go through it two at a time in SSE2 lanes with the scalar path's exact operations, adding to the sums in voice order. | Bit-identical |
| The noise tilt evaluated `exp` every sample; every unison slot's pan took a cosine and a sine every tick, silent ones included; the detune curve's `pow` ran per slot per tick; the voice mixed silent sources. | Hoisted, skipped for zero gain, memoised on count and curve, skipped. | Bit-identical |
| Expanse (every pad's reverb) took 32 sines a frame for its line wander and re-derived 32 early-reflection read weights a frame. | Sines exact every 32 frames and at every control, rotated by each line's per-frame turn between; reflection weights kept while pre-delay and size hold still. | Wander within a few ulps of the sines: at most −150 dBFS on any Nova preset; Expanse's golden fingerprints unchanged |

`--compare` against renders saved before the pass: 40 of 75 scenes bit-identical, the rest within the bounds above. `native_nova_render_contract` holds every Pad preset's five-note chord to its pre-pass fingerprint (per half-second window: each side's RMS and a pseudo-random projection, to 1e-6 of the window's level).

Results on the i7-10700K, GCC 15, quick-opt, five-note chords, mean (p99) share of one core; the DAW was open, so each figure is the better of two interleaved runs:

| Scene | 128 frames before | after | 512 frames before | after |
|---|---|---|---|---|
| Analog Pad | 22.4% (28.4%) | 16.0% (20.8%) | 22.4% (27.7%) | 15.9% (19.7%) |
| Glass Pad | 20.2% (24.6%) | 13.2% (16.6%) | 20.1% (23.7%) | 13.1% (15.4%) |
| Dark Strings | 16.1% (22.0%) | 12.7% (17.1%) | 16.1% (20.6%) | 12.6% (16.1%) |
| Spectral Drift | 16.7% (19.1%) | 11.2% (13.3%) | 16.6% (18.1%) | 11.1% (12.7%) |
| Choir Pad | 21.8% (28.5%) | 12.9% (16.8%) | 21.7% (26.1%) | 12.8% (15.8%) |
| Warm Pad | 15.3% (20.7%) | 10.9% (14.4%) | 15.2% (19.4%) | 10.8% (14.1%) |
| Sweep Pad | 14.8% (19.1%) | 9.7% (12.4%) | 14.6% (17.3%) | 9.7% (11.9%) |
| Warehouse Poly Pad | 16.5% (22.2%) | 12.9% (17.2%) | 16.5% (20.9%) | 12.9% (16.2%) |
| Whispered Ahh | 33.0% (39.7%) | 17.9% (21.9%) | 32.9% (37.9%) | 17.8% (20.0%) |
| Supersaw Lead | 17.7% (27.1%) | 13.6% (21.2%) | 17.5% (25.5%) | 13.4% (20.5%) |
| Pluck | 7.3% (12.9%) | 6.1% (11.0%) | 7.3% (12.7%) | 6.1% (10.7%) |

What remains (sampled shares of the five-note chord's cost):

| Scene | Oscillators | Voice filters | Expanse reverb | Chorus, rack, resampler | Control ticks | Voice mix |
|---|---|---|---|---|---|---|
| Analog Pad | 31% | 22% | 24% | 10% | 7% | 4% |
| Warm Pad | 24% | 10% | 35% | 15% | 9% | 5% |
| Whispered Ahh | 25% | 27% | 22% | 8% | 9% | 7% |

- The oscillators are arithmetic: Classic (BLEP) unison stacks still run a voice at a time, and warped wavetables take the scalar path.
- The ladder filters are a serial chain of saturating stages with divisions, already paired across channels.
- Expanse's late network (16 lines, each three fractional allpass reads and two shelves a frame) is a fixed cost per instance, a fifth to a third of a pad; packing its lines into SIMD lanes is the next step there.
- Inactive unison slots still count toward an oscillator's wavetable band-limit (`fastest` in `Oscillator::set`): with fewer voices than 16, the extrapolated outer slots ask for a duller mip level than the sounding voices need. Fixing it brightens wavetable unison slightly, so it was left for a decision rather than folded into a same-output pass.

## The 2 November 2026 pass: a heavy project at a 1024-frame buffer

Nova still glitched in a heavy project (four Nova tracks, Knock drums, Anvil kicks, about 30 effect inserts). A copy was timed through `RoutedPlayback::prepare_and_start` with a timing output. The output rendered 1024-frame blocks (the project's device buffer, a 21.3 ms deadline) at device pace, with 7 helpers, for the whole 34 s song. The per-node breakdown came from the ADR-0061 trace (`tools/trace_report`), and each Nova track was also timed alone, from its saved patch and notes. Six-note chords held on a pad with a 3.5 s release mean about eleven voices at a time.

What was hot:
- **The pad track `aaff`.** Classic osc A with 5-voice unison, noise into a Formant filter in parallel with a Ladder, Analogue on, and chorus and reverb in its rack. It took 7.1 ms of the average block and 16.6 ms of the worst, one task on one thread while the other sources had long finished. Alone, its time went to the filters (about 28%), the oscillators (19%), Expanse (13%), the voice mix (8%) and noise (5.5%).
- **The other Nova tracks.** The bass took 2.4 ms per block and the pluck 1.6 ms (half of it Expanse in its rack). A Nova with no notes and no rack took 0.2 ms.

| Cause | Fix | Output |
|---|---|---|
| One synth's voices ran on one thread while the other cores waited (ADR-0048's "next lever"). | ADR-0085: nested worker batches. Nova renders each sounding voice on a free helper, into its own buffer started at −0, and adds the buffers in voice order. | Bit-identical |
| Formant and Vowel ran their bands one channel at a time. | Both channels through each band in one SSE2 pair, with the coefficients' division once per band (`VoiceFilter::Impl::character_pair`). | Bit-identical |
| A silent sub ran sample by sample beside sounding noise, or the reverse. Noise found its sample index with two integer divisions per sample. | Each of them holds its state alone when silent, as both already did together. The index uses a compare. | Bit-identical (a source silent for a whole tick resumes from where it stopped) |
| Expanse re-derived 48 powers, 40 shelves and 32 sines and cosines every control tick, with the settings unchanged. Its 16 lines and 8 diffusers ran one at a time, and its phase wrap called libm `floor`. | Derived only when the settings or the space change. Lines and channels run in SSE2 pairs (readers, allpasses, shelves). An inline floor gives the same value for every input (`synth::floor_inline`), and every effect's `dsp::wrap` uses it. | Bit-identical |

`--compare` against renders saved before the pass: all 75 scenes are bit-identical. The full 34 s mix of the project is bit-identical. `native_nova_render_contract.voices_on_helpers` checks that a chord renders to the bit with its voices on three helpers.

The project at 1024 frames, i7-10700K, GCC 15, quick-opt, DAW closed:

| | Before | After |
|---|---|---|
| Mean block | 14.6 ms (69% of the deadline) | 10.0 ms (47%) |
| p99 | 20.7–21.6 ms | 12.5 ms |
| Worst | 25.8–28.0 ms (121–131%) | 15.8–17.6 ms (74–82%) |
| Blocks over the deadline | 11–19 of 1593 | 0 |
| `aaff` source, mean (worst) | 7.1 ms (16.6 ms) | 3.6 ms (6.2 ms) |

Each Nova track alone on one thread (`track_bench`, best mean of three runs, share of one core). Only 1024 and 128 frames were measured before; 64 frames is after only.

| Track | 1024 before | 1024 after | 128 before | 128 after | 64 after |
|---|---|---|---|---|---|
| aaff (pad) | 30.8% | 25.7% | 31.2% | 25.8% | 26.5% |
| Base (bass) | 9.2% | 8.6% | 9.2% | 8.7% | 8.9% |
| Pluck | 6.6% | 5.5% | | 5.5% | 5.6% |
| Empty Nova | 0.8% | 0.8% | | 0.9% | 0.9% |

What is left:
- **The worst blocks** are now set by the bass track's chain: its Nova source, then a Forge insert of about 5 ms. That is outside Nova.
- **The pad voice.** On one thread it is filters (Formant plus a serial Ladder of four saturating stages, a quarter of the cost), Classic BLEP unison one voice at a time, and Expanse.
- **Cross-voice SIMD.** Now that voices render apart, the ladder could run across voices in SIMD lanes. That needs voice-major restructuring.
- **Other engines** with many voices could use `run_nested` the same way.
