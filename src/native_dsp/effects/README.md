# Effects

The native effects. Each one is a parameter table and a DSP kernel run by the
native effect platform ([ADR-0040](../../../docs/decisions/0040-native-effect-platform.md)),
with its own Qt Quick panel in `apps/daw_quick/qml/<engine>/`. The screenshots
come from the panel tests in `tests/ui/quick/`, with telemetry from the real
effect or a test frame.

## Filter, drive and modulation

### Contour

Multimode filter from the synths' filter models (SVF, ladder, OTA, SEM, comb),
with a tempo-synced LFO and an envelope follower on the input or the sidechain.
[ADR-0040](../../../docs/decisions/0040-native-effect-platform.md)

![Contour panel](../../../docs/images/effects/contour.png)

### Forge

Saturation: eight antialiased curves (tape, tube, transistor, diode, clip,
two folds, rectify) at 16x oversampling, with colour and auto gain.
[ADR-0040](../../../docs/decisions/0040-native-effect-platform.md)

![Forge panel](../../../docs/images/effects/forge.png)

### Orbit

Modulation: chorus, string ensemble, dimension, flanger, phaser, vibrato and
tremolo or auto-pan from one tempo-syncable LFO.
[ADR-0040](../../../docs/decisions/0040-native-effect-platform.md)

![Orbit panel](../../../docs/images/effects/orbit.png)

### Chop

Tempo-locked gate and stutter: 16 or 32 steps, each with a level, gate length,
ratchets, a repeat and a chance.
[ADR-0075](../../../docs/decisions/0075-chop.md)

![Chop panel](../../../docs/images/effects/chop.png)

### Flyby

A sound travelling along a path past the listener, with Doppler shift, the
time and level between the ears, and air absorption.
[ADR-0067](../../../docs/decisions/0067-flyby.md)

![Flyby panel](../../../docs/images/effects/flyby.png)

### Dust

Background texture and ageing: eight synthesised layers (vinyl, tape, hum,
radio static, rain, wind, room tone, machines) that duck under a sidechain,
and wear, wow and flutter on the signal passing through.
[ADR-0069](../../../docs/decisions/0069-dust-texture.md)

![Dust panel](../../../docs/images/effects/dust.png)

## Delay and reverb

### Relay

Stereo, ping-pong or mono delay up to four seconds, free or tempo-synced, in
digital, tape or analogue modes.
[ADR-0040](../../../docs/decisions/0040-native-effect-platform.md)

![Relay panel](../../../docs/images/effects/relay.png)

### Expanse

Algorithmic reverb with six spaces (room, hall, plate, chamber, cathedral,
cloud) on a 16-line feedback delay network, with freeze.
[ADR-0040](../../../docs/decisions/0040-native-effect-platform.md)

![Expanse panel](../../../docs/images/effects/expanse.png)

## EQ and dynamics

### Shape

Eight-band EQ with shelves, bells and pass filters up to 96 dB an octave.
[ADR-0047](../../../docs/decisions/0047-classic-processors-as-engines.md)

![Shape panel](../../../docs/images/effects/shape.png)

### Press

Compressor with threshold, ratio, knee, attack, release, makeup and a
sidechain-keyable detector.
[ADR-0047](../../../docs/decisions/0047-classic-processors-as-engines.md)

![Press panel](../../../docs/images/effects/press.png)

### Ceiling

Lookahead limiter.
[ADR-0047](../../../docs/decisions/0047-classic-processors-as-engines.md)

![Ceiling panel](../../../docs/images/effects/ceiling.png)

### Strata

Multiband dynamics: up to six bands, each with one transfer curve that
compresses or expands above its threshold and lifts or pushes down below it.
[ADR-0082](../../../docs/decisions/0082-strata-multiband.md)

![Strata panel](../../../docs/images/effects/strata.png)

### Temper

Resonance suppressor with dynamic EQ bands and a drawn sensitivity curve.
[ADR-0064](../../../docs/decisions/0064-temper-resonance-suppressor.md)

![Temper panel](../../../docs/images/effects/temper.png)

### Deck

DJ mixer three-band isolator: Linkwitz-Riley crossovers, each band from a full
kill to +6 dB, with kill switches.
[ADR-0055](../../../docs/decisions/0055-deck-dj-isolator.md)

![Deck panel](../../../docs/images/effects/deck.png)

## Sidechain

### Pocket

Kick and bass masking resolver: on the bass, keyed by the kick, it ducks the
bass in the bands where the kick needs room.
[ADR-0041](../../../docs/decisions/0041-pocket-kick-bass.md)

![Pocket panel](../../../docs/images/effects/pocket.png)

### Pump

Volume shaper for sidechain pumping: one cycle's volume is a curve drawn over
the kick it follows, restarted on the grid or on each detected kick.
[ADR-0058](../../../docs/decisions/0058-pump-volume-shaper.md)

![Pump panel](../../../docs/images/effects/pump.png)

## Stereo and utilities

### Vista

Stereo imager: width per band on the side signal only, with mono bass and a
vectorscope.
[ADR-0040](../../../docs/decisions/0040-native-effect-platform.md)

![Vista panel](../../../docs/images/effects/vista.png)

### Trim

Gain, pan, width, mono, swap, polarity, channel solo, DC block and a phase
rotator.
[ADR-0042](../../../docs/decisions/0042-native-utility-suite.md)

![Trim panel](../../../docs/images/effects/trim.png)

### Lens

Spectrum analyser with a spectrogram and reference matching; audio passes
untouched.
[ADR-0042](../../../docs/decisions/0042-native-utility-suite.md)

![Lens panel](../../../docs/images/effects/lens.png)

### Loudness

EBU R128 / BS.1770 loudness meter with history and targets; audio passes
untouched.
[ADR-0042](../../../docs/decisions/0042-native-utility-suite.md)

![Loudness panel](../../../docs/images/effects/loudness.png)

### Scope

Oscilloscope locked to the bar and beat, with the sidechain overlaid and an
alignment meter for the delay and polarity between them.
[ADR-0042](../../../docs/decisions/0042-native-utility-suite.md)

![Scope panel](../../../docs/images/effects/scope.png)

### Tune

Tuner for instruments, bass and kicks; audio passes untouched.
[ADR-0042](../../../docs/decisions/0042-native-utility-suite.md)

![Tune panel](../../../docs/images/effects/tune.png)

### Signal

Test generator: sine, logarithmic sweep, white and pink noise, and impulse
trains, replacing or added to the input.
[ADR-0042](../../../docs/decisions/0042-native-utility-suite.md)

![Signal panel](../../../docs/images/effects/signal.png)
