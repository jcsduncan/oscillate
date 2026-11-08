.pragma library
.import Oscillate.Contracts 1.0 as Law
.import Oscillate.Format 1.0 as Fmt

// The Nova panel <-> engine contract (docs/specs/08a-nova-panel-contract.md)
// as data the panel reads: telemetry offsets, modulation sources, effect
// modules, built-in media and the shared curve law (from Contracts). Change
// the spec first.

// ---- Telemetry (spec §6) --------------------------------------------------------------
var T = {
    size: 2038,
    version: 0, voices: 1, polyphony: 2, cpu: 3, note: 4, playing: 5,
    position: 8, unison: 11, peak: 14,
    cutoff: 19, resonance: 21,
    envLevel: 23, envStage: 27, envProgress: 31,
    lfoValue: 35, lfoPhase: 43,
    macro: 51, perf: 59,
    meters: 69,
    // The arp's pattern step, counted from the pattern start (each lane
    // shows it modulo its length), or -1.
    clip: 77, clipPlayhead: 78, arpStep: 79,
    routes: 80,
    frames: 336, framePoints: 128,
    fx: 720,
    // Per slot uid 1..24 (§6): input and output peak (dBFS), three detail
    // values and the output spectrum.
    slotIn: 744, slotOut: 768, slotDetail: 792, slotSpectrum: 864,
    // Analogue (ADR-0073): Voice Mod A/B and Spread (unipolar), then each of
    // note voices 0..7's pitch drift (cents) and key (or -1).
    voice: 2016, voiceDrift: 2019, voiceKey: 2027,
    // The arp (ADR-0078): arp.mod and arp.accent as s, then 1 while the
    // step playing is on a variation bar.
    arp: 2035, arpVariation: 2037
}

// ---- The key strip (ADR-0076) ----------------------------------------------------------
var semitonesPerOctave = 12
// The A key's note when the host names no typing-keyboard octave: C3.
var defaultKeyBase = 48
// The strip's white keys over the base note, labelled with the typing
// keyboard's letters (A S D F G H J K L ;), and the black keys (W E T Y U
// O P), each after the white key it follows.
var whiteKeys = [{ offset: 0, label: "A" }, { offset: 2, label: "S" }, { offset: 4, label: "D" },
                 { offset: 5, label: "F" }, { offset: 7, label: "G" }, { offset: 9, label: "H" },
                 { offset: 11, label: "J" }, { offset: 12, label: "K" }, { offset: 14, label: "L" },
                 { offset: 16, label: ";" }]
var blackKeys = [{ offset: 1, label: "W", after: 0 }, { offset: 3, label: "E", after: 1 },
                 { offset: 6, label: "T", after: 3 }, { offset: 8, label: "Y", after: 4 },
                 { offset: 10, label: "U", after: 5 }, { offset: 13, label: "O", after: 7 },
                 { offset: 15, label: "P", after: 8 }]
// MIDI velocities a click plays: softer at the top of a key, louder at its foot.
var keyVelocityMinimum = 40
var keyVelocityMaximum = 127
// Note voices the telemetry frame reports keys for (T.voiceKey..).
var telemetryNoteVoices = 8

var oscKeys = ["a", "b", "c"]
var maxFxUid = 24

// ---- Modulation sources (spec §4) ------------------------------------------------------
var envSources = ["env.1", "env.2", "env.3", "env.4"]
// The envelope every voice's level follows.
var ampEnvelope = "env.1"
var lfoSources = ["lfo.1", "lfo.2", "lfo.3", "lfo.4", "lfo.5", "lfo.6", "lfo.7", "lfo.8"]
var macroSources = ["macro.1", "macro.2", "macro.3", "macro.4", "macro.5", "macro.6", "macro.7", "macro.8"]
var perfSources = ["velocity", "note", "modwheel", "aftertouch", "mpe.slide", "mpe.pressure",
                   "chaos.1", "chaos.2", "random", "bend"]
var perfNames = {
    "velocity": "Velocity", "note": "Note", "modwheel": "Mod Wheel", "aftertouch": "Aftertouch",
    "mpe.slide": "Slide", "mpe.pressure": "Pressure", "chaos.1": "Chaos 1", "chaos.2": "Chaos 2",
    "random": "Random", "bend": "Bend"
}

// Per-voice sources from Analogue's Voice Lanes: each voice's painted or
// seeded Mod A and Mod B amounts, and its place in the spread (-1..1).
var voiceSources = ["voice.mod_a", "voice.mod_b", "voice.spread"]
var voiceNames = { "voice.mod_a": "Voice Mod A", "voice.mod_b": "Voice Mod B", "voice.spread": "Voice Spread" }

// Per-voice sources from the arp (ADR-0078): the step's Mod lane value and
// its accent, held by the voice the step started.
var arpSources = ["arp.mod", "arp.accent"]
var arpNames = { "arp.mod": "Arp Mod", "arp.accent": "Arp Accent" }

function allSources() {
    return envSources.concat(lfoSources, macroSources, perfSources, voiceSources, arpSources)
}

// The source's unipolar value s (0..1) in a telemetry frame, or -1.
function sourceValue(frame, id) {
    if (!frame || frame.length < T.size)
        return -1
    var dot = id.indexOf(".")
    var n = dot > 0 ? parseInt(id.substring(dot + 1)) - 1 : 0
    if (id.indexOf("env.") === 0) return frame[T.envLevel + n]
    if (id.indexOf("lfo.") === 0) return frame[T.lfoValue + n]
    if (id.indexOf("macro.") === 0) return frame[T.macro + n]
    var v = voiceSources.indexOf(id)
    if (v >= 0) return frame[T.voice + v]
    var a = arpSources.indexOf(id)
    if (a >= 0) return frame[T.arp + a]
    var p = perfSources.indexOf(id)
    return p >= 0 ? frame[T.perf + p] : -1
}

// ---- Analogue (VOICES page) --------------------------------------------------------------
// Every voice (each note voice and each unison voice) gets fixed offsets
// from the seed and its number, scaled by Age, plus a slow drift. A part's
// spread is its tolerance (0..1) × its maximum × Age × ageScale.
var analogue = {
    ageScale: 1.6,
    // The note voices the lanes and the voice cards show.
    noteVoices: 8,
    seedMin: 1,
    seedMax: 9999,
    // New synth steps the seed through a full-period affine sequence over
    // seedMin..seedMax: m = 9999 = 3² · 11 · 101, a − 1 = 6666 is a multiple
    // of 3 · 11 · 101 and c = 4127 is coprime to m (Hull–Dobell), so every
    // seed comes round once before any repeats.
    seedStep: 6667,
    seedOffset: 4127,
    // A painted value's resolution, and how far Spread and Randomise reach.
    paintStep: 1 / 50,
    spreadReach: 0.8,
    randomReach: 0.7
}
function nextSeed(seed) {
    var m = analogue.seedMax - analogue.seedMin + 1
    var s = Math.max(0, Math.round(seed) - analogue.seedMin) % m
    return analogue.seedMin + (analogue.seedStep * s + analogue.seedOffset) % m
}

// Tolerances (nova/analogue.hpp): what a lane value of ±1 means for each
// part, and the decimals its amounts show. Cutoff and envelope times scale
// by (1 + max / 100)^value (`ratio`), symmetric in octaves.
var analogueTolerances = [
    { id: "pitch", name: "Pitch", max: 12, unit: "ct", digits: 1 },
    { id: "cutoff", name: "Cutoff", max: 18, unit: "%", digits: 0, ratio: true },
    { id: "resonance", name: "Resonance", max: 10, unit: "%", digits: 0 },
    { id: "env", name: "Env times", max: 25, unit: "%", digits: 0, ratio: true },
    { id: "level", name: "Level", max: 1.5, unit: "dB", digits: 1 },
    { id: "pan", name: "Pan", max: 40, unit: "%", digits: 0 },
    { id: "shape", name: "Osc shape", max: 8, unit: "%", digits: 0 },
    { id: "glide", name: "Glide", max: 35, unit: "%", digits: 0, ratio: true }
]
function analogueTolerance(id) {
    for (var i = 0; i < analogueTolerances.length; ++i)
        if (analogueTolerances[i].id === id)
            return analogueTolerances[i]
    return null
}
// An amount in a tolerance's unit: ± for a spread, signed for one voice.
function toleranceAmount(t, amount, signed) {
    return signed ? Fmt.Format.signed_quantity(amount, t.unit, t.digits)
                  : "±" + Fmt.Format.quantity(Math.abs(amount), t.unit, t.digits)
}
// The spread a tolerance position p gives at Age `age`.
function toleranceText(t, p, age) {
    return toleranceAmount(t, p * t.max * age * analogue.ageScale, false)
}

// Drift: each voice's slow wander, the same at every Age.
// Cents in the drift and flutter readouts keep a decimal; a compressor
// ratio keeps one.
var centsDigits = 1, ratioDigits = 1
var driftMaxCents = 8, driftMinHz = 0.02, driftMaxHz = 0.62, driftMaxCutoff = 10
var analogueDrift = [
    { id: "drift.amount", name: "Amount", fmt: function (p) { return "±" + Fmt.Format.quantity(p * driftMaxCents, "ct", centsDigits) } },
    { id: "drift.rate", name: "Rate", fmt: function (p) { return hz(lin(driftMinHz, driftMaxHz)(p)) } },
    { id: "drift.cutoff", name: "On cutoff", fmt: function (p) { return "±" + Fmt.Format.percent(p * driftMaxCutoff) } }
]

// Re-roll: each note adds a fresh seeded offset up to this share of a
// part's seeded reach (nova/analogue.hpp), like a RANDOM column.
var analogueReroll = { id: "reroll", name: "Re-roll", fmt: function (p) { return pct(p) + " / note" } }

// Flutter: fast, low-passed pitch jitter per voice (analogue_flutter_cents,
// analogue_flutter_tone_min_hz..max_hz, log).
var flutterMaxCents = 15, flutterToneMinHz = 2, flutterToneMaxHz = 60
var analogueFlutter = [
    { id: "flutter.amount", name: "Flutter", fmt: function (p) { return "±" + Fmt.Format.quantity(p * flutterMaxCents, "ct", centsDigits) } },
    { id: "flutter.tone", name: "Flutter tone", fmt: function (p) { return hz(expo(flutterToneMinHz, flutterToneMaxHz)(p)) } }
]

// Voice cards (nova.analogue.cards): 0 gives every note voice its own
// offsets by its slot; N deals notes round-robin over N cards.
var voiceCardsMax = 16
function cardsText(n) { return n <= 0 ? "Per voice" : String(n) }
// The note voices the lanes show: the cards, or `analogue.noteVoices`.
function laneVoices(cards) { return cards >= 1 ? cards : analogue.noteVoices }

// Noise AM (nova.noise.am.*): the noise level follows an oscillator.
var noiseAmLabels = ["OFF", "A", "B", "C"]

// Age's names, each from its lower bound.
var ages = [
    { from: 0, name: "Factory fresh", hint: "Every voice matched by a technician: tight, a bit sterile." },
    { from: 0.2, name: "Lightly used", hint: "Small differences you feel more than hear." },
    { from: 0.4, name: "Well-used", hint: "Like a synth that has been gigged for a few years: you hear it in chords, not on one note." },
    { from: 0.65, name: "Vintage", hint: "Seventies voice cards: chords bloom and beat, unison is thick without any detune." },
    { from: 0.85, name: "Needs a service", hint: "Obviously out: great for lo-fi, wobbly, characterful parts." }
]
function age(p) {
    var at = ages[0]
    for (var i = 1; i < ages.length; ++i)
        if (p >= ages[i].from)
            at = ages[i]
    return at
}

var analogueScopes = ["Notes + unison", "Notes only", "Unison only"]

// Voice Lanes: one bar per voice, -1..1, for each lane. Unison voices share
// their note's filter, envelopes and modulation, so only the `unison` lanes
// reach them.
var voiceLanes = [
    { id: "pitch", name: "Pitch", unison: true, hint: "Each voice's tuning error." },
    { id: "cutoff", name: "Cutoff", unison: false, hint: "Each voice's filter is a little brighter or darker; drift moves it slowly." },
    { id: "env", name: "Env", unison: false, hint: "Envelope times per voice: some open and close a touch slower." },
    { id: "level", name: "Level", unison: true, hint: "Output level per voice card." },
    { id: "pan", name: "Pan", unison: true, hint: "Where each voice sits. On unison voices this adds to the Width spread." },
    { id: "phase", name: "Phase", unison: true, hint: "Start phase offset when Free-running is off." },
    { id: "moda", name: "Mod A", unison: false, source: "voice.mod_a", hint: "Mod A: a per-voice amount you can use as a source in the mod matrix (e.g. LFO 1 → cutoff × Voice Mod A)." },
    { id: "modb", name: "Mod B", unison: false, source: "voice.mod_b", hint: "Mod B: a second per-voice amount for the mod matrix." }
]
// The Phase lane offsets the start phase by up to half a cycle.
var phaseOffsetDegrees = 180
// A lane value (-1..1) in its lane's unit.
function laneText(lane, x) {
    var t = analogueTolerance(lane)
    if (t !== null)
        return toleranceAmount(t, t.ratio ? (Math.pow(1 + t.max / 100, x) - 1) * 100 : x * t.max, true)
    if (lane === "phase")
        return Fmt.Format.signed_quantity(x * phaseOffsetDegrees, "°")
    return signedPct(x)
}

// ---- Built-in media (spec §3.4) ---------------------------------------------------------
// Samples per frame a built-in table is built with (presets.cpp's table()).
var tableFrameLength = 2048
// Every factory table (tables_factory.cpp), grouped by family.
var tables = [
    { id: "basic_shapes", name: "Basic Shapes" }, { id: "analog_saw", name: "Analog Saw" },
    { id: "analog_pulse", name: "Analog Pulse" }, { id: "saw_stack", name: "Saw Stack" },
    { id: "reese_saw", name: "Reese Saw" }, { id: "diode_saw", name: "Diode Saw" },
    { id: "cz_saw", name: "CZ Saw" }, { id: "cz_resonance", name: "CZ Resonance" },
    { id: "sync_saw", name: "Sync Saw" }, { id: "sync_square", name: "Sync Square" },
    { id: "window_sync", name: "Window Sync" }, { id: "harmonic_sweep", name: "Harmonic Sweep" },
    { id: "odd_sweep", name: "Odd Sweep" }, { id: "organ_drawbars", name: "Organ Drawbars" },
    { id: "pluck_damp", name: "Pluck Damp" }, { id: "spectral_pad", name: "Spectral Pad" },
    { id: "glass_formant", name: "Glass Formant" }, { id: "vowel_morph", name: "Vowel Morph" },
    { id: "formant_yoi", name: "Formant Yoi" }, { id: "formant_square", name: "Formant Square" },
    { id: "talk_box", name: "Talk Box" }, { id: "fm_bell", name: "FM Bell" },
    { id: "fm_growl", name: "FM Growl" }, { id: "fm_bark", name: "FM Bark" },
    { id: "metal_clang", name: "Metal Clang" }, { id: "grit_fold", name: "Grit Fold" },
    { id: "west_fold", name: "West Fold" }, { id: "metal_fold", name: "Metal Fold" },
    { id: "comb_hollow", name: "Comb Hollow" }, { id: "growl_saw", name: "Growl Saw" },
    { id: "resample_grind", name: "Resample Grind" }, { id: "roar", name: "Roar" },
    { id: "wub_sine", name: "Wub Sine" }, { id: "bit_grit", name: "Bit Grit" },
    { id: "chiptune", name: "Chiptune" }, { id: "noise_bands", name: "Noise Bands" }
]
// The table `step` places from `id` in the list, wrapping.
function stepTable(id, step) {
    var i = 0
    while (i < tables.length && tables[i].id !== id)
        ++i
    return tables[((i % tables.length) + step + tables.length) % tables.length].id
}
var noises = [
    { id: "white", name: "White" }, { id: "pink", name: "Pink" }, { id: "brown", name: "Brown" },
    { id: "analog_hiss", name: "Analog Hiss" }, { id: "vinyl", name: "Vinyl Crackle" },
    { id: "breath", name: "Breath" }
]
function mediaName(list, id, fallback) {
    for (var i = 0; i < list.length; ++i)
        if (list[i].id === id)
            return list[i].name
    return fallback
}

// ---- Filter models (spec §2.4) -----------------------------------------------------------
var filterGroups = [
    { name: "Analog", first: 0, last: 4 }, { name: "Clean", first: 5, last: 12 },
    { name: "Character", first: 13, last: 18 }, { name: "Morph", first: 19, last: 21 }
]

// ---- Curves (spec §3.2) -------------------------------------------------------------------
var defaultLfo = [{ x: 0, y: 0, tension: 0, step: false }, { x: 0.5, y: 1, tension: 0, step: false },
                  { x: 1, y: 0, tension: 0, step: false }]

// The engine's bend (Contracts): tension above 0 holds the start longer,
// below 0 leaves it sooner.
function bend(t, k) {
    return Law.Contracts.bend(t, k)
}

// The LFO shape library: name and points.
var shapes = [
    { name: "Triangle", points: defaultLfo },
    { name: "Sine", points: [{ x: 0, y: 0.5, tension: -0.35, step: false }, { x: 0.25, y: 1, tension: 0.35, step: false },
                             { x: 0.5, y: 0.5, tension: -0.35, step: false }, { x: 0.75, y: 0, tension: 0.35, step: false },
                             { x: 1, y: 0.5, tension: 0, step: false }] },
    { name: "Saw Down", points: [{ x: 0, y: 1, tension: 0, step: false }, { x: 0.998, y: 0, tension: 0, step: false },
                                 { x: 1, y: 1, tension: 0, step: false }] },
    { name: "Saw Up", points: [{ x: 0, y: 0, tension: 0, step: false }, { x: 0.998, y: 1, tension: 0, step: false },
                               { x: 1, y: 0, tension: 0, step: false }] },
    { name: "Square", points: [{ x: 0, y: 1, tension: 0, step: true }, { x: 0.5, y: 0, tension: 0, step: true },
                               { x: 1, y: 1, tension: 0, step: false }] },
    { name: "Pluck", points: [{ x: 0, y: 1, tension: 0.6, step: false }, { x: 1, y: 1, tension: 0, step: false }] },
    { name: "Wobble", points: [{ x: 0, y: 0.1, tension: 0, step: false }, { x: 0.18, y: 1, tension: -0.5, step: false },
                               { x: 0.5, y: 0.35, tension: 0.4, step: false }, { x: 0.75, y: 0.8, tension: 0, step: false },
                               { x: 1, y: 0.1, tension: 0, step: false }] },
    { name: "Stairs", points: [{ x: 0, y: 0, tension: 0, step: true }, { x: 0.25, y: 0.33, tension: 0, step: true },
                               { x: 0.5, y: 0.66, tension: 0, step: true }, { x: 0.75, y: 1, tension: 0, step: true },
                               { x: 1, y: 0, tension: 0, step: false }] }
]

// ---- Effect rack (spec §2.9, §2.10, §3.3, §5) ------------------------------------------------
function lin(a, b) { return function (p) { return a + (b - a) * p } }
function expo(a, b) { return function (p) { return a * Math.pow(b / a, p) } }
// The inverses, for presets written in real units.
function linP(v, a, b) { return (v - a) / (b - a) }
function expoP(v, a, b) { return Math.log(v / a) / Math.log(b / a) }
function choiceP(index, count) { return count > 1 ? index / (count - 1) : 0 }
// Values in their units, through the display formatter.
function hz(v) { return Fmt.Format.hertz(v) }
function ms(v) { return Fmt.Format.milliseconds(v) }
function seconds(v) { return Fmt.Format.seconds(v) }
function number(v) { return Fmt.Format.number(v) }
function db(v) { return Fmt.Format.signed_decibels(v) }
function pct(v) { return Fmt.Format.fraction(v) }
function signedPct(v) { return Fmt.Format.signed_quantity(v * 100, "%") }
function signedHz(v) { return Fmt.Format.signed_quantity(v, "Hz") }

// Every slot has p0..p15, mix and on (§2.10); the chains (§3.3).
var fxSlotParams = 16
var fxChainSlots = 8
var fxChains = [{ id: "fx.main", name: "MAIN" }, { id: "fx.bus.1", name: "BUS 1" }, { id: "fx.bus.2", name: "BUS 2" }]
// Telemetry per slot (§6): Multiband's three band changes and the 48-band
// output spectrum, band i centred at 20 * 1000^((i + 0.5) / 48).
var fxDetailValues = 3
var fxSpectrumBands = 48
// The floor every dBFS telemetry value rests on.
var floorDb = -120
var fxCategories = ["All", "Drive", "Filter", "Motion", "Space", "Dynamics", "Tone", "Stereo"]

var delayDivisions = ["1/64", "1/32 T", "1/32", "1/16 T", "1/16", "1/16 D", "1/8 T", "1/8", "1/8 D",
                      "1/4 T", "1/4", "1/4 D", "1/2", "1/2 D", "1/1"]
// Each division in quarter-note beats, for the delay's beat grid.
var delayBeats = [1 / 16, 1 / 12, 1 / 8, 1 / 6, 1 / 4, 3 / 8, 1 / 3, 1 / 2, 3 / 4, 2 / 3, 1, 3 / 2, 2, 3, 4]
var delayMaxMs = 2000

// The delay's feedback filter (08a §5, p2): -1 closes a low-pass to 1 kHz,
// +1 opens a high-pass to 2 kHz, the centre leaves the repeats full range.
function delayFilter(p) {
    const tilt = 2 * p - 1
    if (Math.abs(tilt) < 0.005)
        return "Full"
    return tilt < 0 ? "LP " + hz(20000 * Math.pow(0.05, -tilt)) : "HP " + hz(20 * Math.pow(100, tilt))
}

// The shifter's shift (08a §5, p0): 5000 Hz · (2p − 1)³, fine near zero.
var shifterMaxHz = 5000
function shifterHz(p) { return shifterMaxHz * Math.pow(2 * p - 1, 3) }
function shifterP(hzValue) { return (Math.cbrt(hzValue / shifterMaxHz) + 1) / 2 }

// Flyby (08a §5): a sound flown past your head. p0 the path, p1 the
// closest distance (log 1..60 m), p2 the speed (log 1..1000 km/h), p3 how
// it runs, p4 the Sync length, p5 the Position, p6 Doppler (0..200 %), p7
// how you listen, p8 the air (0..400 %), p9 the room, p10 the height
// (0..150 m).
var flybyPaths = ["Left → Right", "Right → Left", "Arc Over", "Orbit", "Overhead"]
var flybyPath = { leftRight: 0, rightLeft: 1, arc: 2, orbit: 3, overhead: 4 }
var flybyRuns = ["Loop", "Sync", "Position"]
var flybyRun = { loop: 0, sync: 1, position: 2 }
var flybyLengths = ["1/4 Bar", "1/2 Bar", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars"]
var flybyListens = ["Headphones", "Speakers"]
var flybyMinM = 1, flybyMaxM = 60
var flybyMinKmh = 1, flybyMaxKmh = 1000
var flybyMaxDoppler = 2, flybyMaxAir = 4, flybyMaxHeight = 150
function flybyMetres(p) { return expo(flybyMinM, flybyMaxM)(p) }
function flybyMetresP(m) { return expoP(m, flybyMinM, flybyMaxM) }
function flybyKmh(p) { return expo(flybyMinKmh, flybyMaxKmh)(p) }
function flybyKmhP(v) { return expoP(v, flybyMinKmh, flybyMaxKmh) }
function metres(v) { return Fmt.Format.quantity(v, "m") }

// The EQ (08a §5): four bands of {type, frequency, gain, Q} on p(4b)..p(4b + 3).
var eqBands = 4
var eqTypes = ["Off", "Bell", "Low Shelf", "High Shelf", "Low Cut 12", "Low Cut 24",
               "High Cut 12", "High Cut 24", "Notch", "Band Pass"]
var eqType = { off: 0, bell: 1, lowShelf: 2, highShelf: 3, lowCut12: 4, lowCut24: 5,
               highCut12: 6, highCut24: 7, notch: 8, bandPass: 9 }
var eqMinHz = 20, eqMaxHz = 20000, eqRangeDb = 24, eqMinQ = 0.25, eqMaxQ = 16
// A 24 dB cut: a Butterworth section (k = 1.8478) and one whose Q scales
// with the band's, so Q 0.707 is a fourth-order Butterworth.
var eqButterworthK = 1.8478
var eqSecondQScale = 1.3066 / 0.7071
function eqHasGain(type) { return type === eqType.bell || type === eqType.lowShelf || type === eqType.highShelf }
function eqTypeIndex(p) { return modeIndex(p, eqTypes.length) }
function eqHz(p) { return expo(eqMinHz, eqMaxHz)(p) }
function eqDb(p) { return lin(-eqRangeDb, eqRangeDb)(p) }
function eqQ(p) { return expo(eqMinQ, eqMaxQ)(p) }
// One band's values as p0..p3 of its four, for presets.
function eqBand(type, hzValue, dbValue, q) {
    return [choiceP(type, eqTypes.length), expoP(hzValue, eqMinHz, eqMaxHz),
            linP(dbValue, -eqRangeDb, eqRangeDb), expoP(q, eqMinQ, eqMaxQ)]
}
function eqValues(bands) {
    const out = {}
    for (let b = 0; b < bands.length; ++b)
        for (let k = 0; k < 4; ++k)
            out["p" + (4 * b + k)] = bands[b][k]
    return out
}
var eqDefaultBands = [eqBand(2, 100, 0, Math.SQRT1_2), eqBand(1, 400, 0, 1),
                      eqBand(1, 2500, 0, 1), eqBand(3, 8000, 0, Math.SQRT1_2)]

// |H(f)|² of one second-order section at r = f / f0 (analog prototypes).
function section2(kind, r, q) {
    const den = Math.pow(1 - r * r, 2) + Math.pow(r / q, 2)
    switch (kind) {
    case "lp": return 1 / den
    case "hp": return Math.pow(r, 4) / den
    case "bp": return Math.pow(r / q, 2) / den
    default: return Math.pow(1 - r * r, 2) / den // notch
    }
}
// One band's gain in dB at f Hz.
function eqBandDb(type, f0, gainDb, q, f) {
    const r = f / f0
    const A = Math.pow(10, gainDb / 40)
    let m2 = 1
    switch (type) {
    case eqType.bell:
        m2 = (Math.pow(1 - r * r, 2) + Math.pow(r * A / q, 2)) / (Math.pow(1 - r * r, 2) + Math.pow(r / (A * q), 2))
        break
    case eqType.lowShelf: {
        const s = Math.sqrt(A) * r / q
        m2 = A * A * (Math.pow(A - r * r, 2) + s * s) / (Math.pow(1 - A * r * r, 2) + s * s)
        break
    }
    case eqType.highShelf: {
        const s = Math.sqrt(A) * r / q
        m2 = A * A * (Math.pow(1 - A * r * r, 2) + s * s) / (Math.pow(A - r * r, 2) + s * s)
        break
    }
    case eqType.lowCut12: m2 = section2("hp", r, q); break
    case eqType.lowCut24: m2 = section2("hp", r, 1 / eqButterworthK) * section2("hp", r, q * eqSecondQScale); break
    case eqType.highCut12: m2 = section2("lp", r, q); break
    case eqType.highCut24: m2 = section2("lp", r, 1 / eqButterworthK) * section2("lp", r, q * eqSecondQScale); break
    case eqType.notch: m2 = section2("notch", r, q); break
    case eqType.bandPass: m2 = section2("bp", r, q); break
    default: m2 = 1
    }
    return 10 * Math.log(Math.max(m2, 1e-12)) / Math.LN10
}

// Each module (§5): `knobs` reach every p value it uses except the mode
// (`mode`, on p7), the delay's sync (`sync`) and the EQ's band types; MIX
// follows them on every strip. `summary` is how many knobs a collapsed
// strip lists. `presets` are factory sounds over the defaults.
var rateFmt = function (p) { return hz(expo(0.01, 10)(p)) }
var modules = [
    { id: "distortion", name: "Distortion", category: "Drive", visual: "transfer",
      blurb: "Twelve shapers, from tube warmth to wavefolding and bit crush.",
      knobs: [{ p: 0, label: "DRIVE", fmt: function (p) { return db(48 * p) } },
              { p: 1, label: "TONE", fmt: function (p) { return signedPct(2 * p - 1) } },
              { p: 2, label: "BIAS", fmt: function (p) { return signedPct(2 * p - 1) } }],
      mode: { p: 7, choices: ["Tube", "Soft", "Hard", "Diode", "Fold", "Rectify", "Sine Fold", "Asym",
                              "Bit Crush", "Downsample", "X-Shaper", "Overdrive"] },
      defaults: { p0: 0.3, p1: 0.5, p2: 0.5, p7: 0, mix: 1 },
      presets: [{ name: "Warm Tube", values: { p0: 0.25, p1: 0.45, p7: choiceP(0, 12) } },
                { name: "Gentle Glue", values: { p0: 0.12, p7: choiceP(1, 12), mix: 0.6 } },
                { name: "Hard Clip", values: { p0: 0.45, p7: choiceP(2, 12) } },
                { name: "Fold Growl", values: { p0: 0.55, p1: 0.6, p7: choiceP(4, 12) } },
                { name: "Bit Crushed", values: { p0: 0.35, p7: choiceP(8, 12), mix: 0.7 } },
                { name: "Overdrive", values: { p0: 0.4, p1: 0.55, p7: choiceP(11, 12) } }] },
    { id: "filter", name: "Filter", category: "Filter", visual: "response",
      blurb: "Nova's filter models after the voices: ladders, SVFs, combs and formants.",
      knobs: [{ p: 0, label: "CUTOFF", fmt: function (p) { return hz(expo(8, 22000)(p)) } },
              { p: 1, label: "RES", fmt: pct },
              { p: 2, label: "DRIVE", fmt: function (p) { return db(24 * p) } }],
      mode: { p: 7, choices: "filterModels" },
      defaults: { p0: 0.6, p1: 0.2, p2: 0, p7: 0, mix: 1 },
      presets: [{ name: "Dark Low-pass", values: { p0: expoP(800, 8, 22000), p1: 0.2 } },
                { name: "Telephone", values: { p0: expoP(1500, 8, 22000), p1: 0.4, p7: choiceP(7, 22) } },
                { name: "Air Lift", values: { p0: expoP(300, 8, 22000), p1: 0.1, p7: choiceP(9, 22) } },
                { name: "Comb Metal", values: { p0: expoP(400, 8, 22000), p1: 0.7, p7: choiceP(15, 22) } },
                { name: "Vowel", values: { p0: expoP(900, 8, 22000), p1: 0.5, p7: choiceP(13, 22) } }] },
    { id: "chorus", name: "Chorus", category: "Motion", visual: "voices",
      blurb: "Two or four voices of slowly moving delay.",
      knobs: [{ p: 0, label: "RATE", fmt: rateFmt },
              { p: 1, label: "DEPTH", fmt: pct },
              { p: 2, label: "FEEDBK", fmt: function (p) { return pct(0.95 * p) } },
              { p: 3, label: "DELAY", fmt: function (p) { return ms(lin(1, 30)(p)) } }],
      mode: { p: 7, choices: ["2 voices", "4 voices"] },
      defaults: { p0: 0.45, p1: 0.4, p2: 0, p3: 0.3, mix: 0.5 },
      presets: [{ name: "Juno I", values: { p0: expoP(0.5, 0.01, 10), p1: 0.35 } },
                { name: "Juno II", values: { p0: expoP(0.83, 0.01, 10), p1: 0.45 } },
                { name: "Wide Shimmer", values: { p0: 0.45, p1: 0.6, p2: 0.3, p3: 0.5, p7: 1, mix: 0.6 } },
                { name: "Seasick", values: { p0: 0.7, p1: 0.9, mix: 0.7 } }] },
    { id: "flanger", name: "Flanger", category: "Motion", visual: "comb",
      blurb: "Classic or through-zero jet sweeps.",
      knobs: [{ p: 0, label: "RATE", fmt: rateFmt },
              { p: 1, label: "DEPTH", fmt: pct },
              { p: 2, label: "FEEDBK", fmt: function (p) { return signedPct(1.9 * p - 0.95) } }],
      mode: { p: 7, choices: ["Classic", "Through-zero"] },
      defaults: { p0: 0.35, p1: 0.5, p2: 0.7, mix: 0.5 },
      presets: [{ name: "Jet", values: { p0: 0.3, p1: 0.7, p2: 0.85 } },
                { name: "Metallic", values: { p0: 0.2, p1: 0.4, p2: 0.95, mix: 0.6 } },
                { name: "Zero Sweep", values: { p0: 0.25, p1: 0.8, p2: 0.6, p7: 1 } }] },
    { id: "phaser", name: "Phaser", category: "Motion", visual: "comb",
      blurb: "Four, eight or twelve all-pass stages sweeping a notch comb.",
      knobs: [{ p: 0, label: "RATE", fmt: rateFmt },
              { p: 1, label: "DEPTH", fmt: pct },
              { p: 2, label: "FEEDBK", fmt: function (p) { return pct(0.95 * p) } },
              { p: 3, label: "CENTRE", fmt: function (p) { return hz(expo(100, 8000)(p)) } }],
      mode: { p: 7, choices: ["4 stages", "8 stages", "12 stages"] },
      defaults: { p0: 0.35, p1: 0.6, p2: 0.4, p3: 0.5, p7: 0.5, mix: 0.5 },
      presets: [{ name: "Small Stone", values: { p0: 0.45, p1: 0.6, p2: 0.5, p7: 0 } },
                { name: "Deep Twelve", values: { p0: 0.3, p1: 0.8, p2: 0.7, p7: 1 } },
                { name: "Vocal Swirl", values: { p0: 0.35, p1: 0.7, p2: 0.4, p3: 0.65 } }] },
    { id: "delay", name: "Delay", category: "Space", visual: "taps",
      blurb: "Stereo, ping-pong or tape echo, synced to the beat or free.",
      knobs: [{ p: 0, label: "TIME", fmt: null },
              { p: 1, label: "FEEDBK", fmt: function (p) { return pct(1.1 * p) } },
              { p: 2, label: "HP/LP", fmt: delayFilter }],
      mode: { p: 7, choices: ["Stereo", "Ping-pong", "Tape"] },
      sync: 5,
      defaults: { p0: 8 / 14, p1: 0.4, p2: 0.5, p5: 1, p7: 0.5, mix: 0.25 },
      presets: [{ name: "Dotted Eighth", values: { p0: choiceP(8, 15), p7: 0 } },
                { name: "Ping-Pong Quarter", values: { p0: choiceP(10, 15), p1: 0.45, mix: 0.3 } },
                { name: "Tape Slap", values: { p0: expoP(110, 1, delayMaxMs), p1: 0.15, p2: 0.35, p5: 0, p7: 1, mix: 0.3 } },
                { name: "Dub Throw", values: { p0: choiceP(11, 15), p1: 0.75, p2: 0.3, p7: 1, mix: 0.35 } },
                { name: "Sixteenth Sparkle", values: { p0: choiceP(4, 15), p1: 0.5, p2: 0.75, mix: 0.2 } }] },
    { id: "reverb", name: "Reverb", category: "Space", visual: "decay",
      blurb: "Hall, plate, room and shimmer spaces.",
      knobs: [{ p: 0, label: "SIZE", fmt: pct },
              { p: 1, label: "DECAY", fmt: function (p) { return Fmt.Format.seconds(expo(0.1, 30)(p)) } },
              { p: 2, label: "DAMP", fmt: pct },
              { p: 3, label: "PRE-DELAY", fmt: function (p) { return ms(250 * p) } }],
      mode: { p: 7, choices: ["Hall", "Plate", "Room", "Shimmer"] },
      defaults: { p0: 0.6, p1: 0.5, p2: 0.4, p3: 0.1, mix: 0.25 },
      presets: [{ name: "Big Hall", values: { p0: 0.8, p1: expoP(3.5, 0.1, 30), p3: 0.12, mix: 0.3 } },
                { name: "Bright Plate", values: { p0: 0.55, p1: expoP(1.8, 0.1, 30), p2: 0.15, p3: 0.05, p7: choiceP(1, 4) } },
                { name: "Small Room", values: { p0: 0.25, p1: expoP(0.6, 0.1, 30), p2: 0.5, p3: 0.02, p7: choiceP(2, 4), mix: 0.2 } },
                { name: "Shimmer Wash", values: { p0: 0.9, p1: expoP(8, 0.1, 30), p2: 0.3, p7: 1, mix: 0.4 } },
                { name: "Infinite", values: { p0: 1, p1: 1, p2: 0.3, mix: 0.5 } }] },
    { id: "compressor", name: "Compressor", category: "Dynamics", visual: "dynamics",
      blurb: "Feed-forward compression with a soft knee and makeup gain.",
      knobs: [{ p: 0, label: "THRESH", fmt: function (p) { return db(-60 + 60 * p) } },
              { p: 1, label: "RATIO", fmt: function (p) { return Fmt.Format.number(expo(1, 20)(p), ratioDigits) + ":1" } },
              { p: 2, label: "ATTACK", fmt: function (p) { return ms(expo(0.1, 100)(p)) } },
              { p: 3, label: "RELEASE", fmt: function (p) { return ms(expo(10, 1000)(p)) } },
              { p: 4, label: "MAKEUP", fmt: function (p) { return db(24 * p) } }],
      mode: { p: 7, choices: ["Self", "Voice sum"] },
      defaults: { p0: 0.7, p1: 0.4, p2: 0.5, p3: 0.5, p4: 0, mix: 1 },
      presets: [{ name: "Glue", values: { p0: linP(-18, -60, 0), p1: expoP(2, 1, 20), p2: expoP(10, 0.1, 100), p3: expoP(150, 10, 1000), p4: 3 / 24 } },
                { name: "Punch", values: { p0: linP(-24, -60, 0), p1: expoP(4, 1, 20), p2: expoP(30, 0.1, 100), p3: expoP(80, 10, 1000), p4: 5 / 24 } },
                { name: "Squash", values: { p0: linP(-30, -60, 0), p1: expoP(10, 1, 20), p2: expoP(1, 0.1, 100), p3: expoP(60, 10, 1000), p4: 9 / 24 } },
                { name: "Pump", values: { p0: linP(-32, -60, 0), p1: expoP(8, 1, 20), p2: expoP(5, 0.1, 100), p3: expoP(250, 10, 1000), p4: 6 / 24 } }] },
    { id: "multiband", name: "Multiband", category: "Dynamics", visual: "bands",
      blurb: "OTT-style: three bands pushed down and lifted up.",
      knobs: [{ p: 0, label: "DEPTH", fmt: pct },
              { p: 1, label: "TIME", fmt: pct },
              { p: 2, label: "UP/DN", fmt: function (p) { return signedPct(2 * p - 1) } },
              { p: 3, label: "LOW", fmt: function (p) { return db(24 * p - 12) } },
              { p: 4, label: "MID", fmt: function (p) { return db(24 * p - 12) } },
              { p: 5, label: "HIGH", fmt: function (p) { return db(24 * p - 12) } }],
      mode: null,
      defaults: { p0: 0.5, p1: 0.5, p2: 0.5, p3: 0.5, p4: 0.5, p5: 0.5, mix: 1 },
      presets: [{ name: "OTT 30%", values: { p0: 0.3 } },
                { name: "OTT Full", values: { p0: 1 } },
                { name: "Upward Only", values: { p0: 0.6, p2: 1 } },
                { name: "Downward Only", values: { p0: 0.6, p2: 0 } },
                { name: "Tame Highs", values: { p0: 0.5, p5: linP(-4, -12, 12) } }] },
    { id: "eq", name: "EQ", category: "Tone", visual: "eq",
      blurb: "Four parametric bands: bells, shelves, cuts, notch and band pass.",
      knobs: [], bands: eqBands, mode: null,
      defaults: Object.assign(eqValues(eqDefaultBands), { mix: 1 }),
      presets: [{ name: "Smile", values: eqValues([eqBand(2, 100, 3, Math.SQRT1_2), eqBand(1, 400, 0, 1), eqBand(1, 2500, 0, 1), eqBand(3, 8000, 3, Math.SQRT1_2)]) },
                { name: "Low Cut Air", values: eqValues([eqBand(5, 80, 0, Math.SQRT1_2), eqBand(1, 400, 0, 1), eqBand(1, 2500, 0, 1), eqBand(3, 10000, 4, Math.SQRT1_2)]) },
                { name: "Mid Scoop", values: eqValues([eqBand(2, 100, 0, Math.SQRT1_2), eqBand(1, 500, -4, Math.SQRT1_2), eqBand(1, 2500, 0, 1), eqBand(3, 8000, 0, Math.SQRT1_2)]) },
                { name: "Telephone", values: eqValues([eqBand(5, 400, 0, Math.SQRT1_2), eqBand(1, 1500, 4, 1.4), eqBand(0, 2500, 0, 1), eqBand(7, 3000, 0, Math.SQRT1_2)]) },
                { name: "De-Harsh", values: eqValues([eqBand(2, 100, 0, Math.SQRT1_2), eqBand(1, 400, 0, 1), eqBand(1, 3500, -4, 2), eqBand(3, 8000, -1.5, Math.SQRT1_2)]) }] },
    { id: "dimension", name: "Dimension", category: "Stereo", visual: "stereo",
      blurb: "Early reflections that widen without chorus wobble.",
      knobs: [{ p: 0, label: "SIZE", fmt: pct }, { p: 1, label: "WIDTH", fmt: pct },
              { p: 2, label: "MOTION", fmt: pct }],
      mode: null,
      defaults: { p0: 0.5, p1: 0.7, p2: 0.3, mix: 0.4 },
      presets: [{ name: "Subtle", values: { p0: 0.3, p1: 0.5, p2: 0.2, mix: 0.3 } },
                { name: "Wide", values: { p0: 0.5, p1: 0.8, p2: 0.3, mix: 0.45 } },
                { name: "Huge", values: { p0: 0.9, p1: 1, p2: 0.5, mix: 0.6 } }] },
    { id: "hyper", name: "Hyper", category: "Stereo", visual: "voices",
      blurb: "Up to seven detuned copies for instant width.",
      knobs: [{ p: 0, label: "DETUNE", fmt: function (p) { return Fmt.Format.quantity(50 * p, "ct") } },
              { p: 1, label: "RATE", fmt: function (p) { return hz(expo(0.05, 5)(p)) } },
              { p: 2, label: "WIDTH", fmt: pct }],
      mode: { p: 7, choices: ["2 voices", "3 voices", "4 voices", "5 voices", "6 voices", "7 voices"] },
      defaults: { p0: 0.3, p1: 0.4, p2: 0.8, p7: 0.6, mix: 0.5 },
      presets: [{ name: "Supersaw Spread", values: { p0: 0.35, p2: 0.9, p7: 1 } },
                { name: "Gentle Double", values: { p0: 0.12, p1: 0.3, p2: 0.6, p7: 0, mix: 0.4 } },
                { name: "Wall", values: { p0: 0.6, p1: 0.5, p2: 1, p7: 1, mix: 0.65 } }] },
    { id: "utility", name: "Utility", category: "Tone", visual: "stereo",
      blurb: "Gain, width, bass mono and pan.",
      knobs: [{ p: 0, label: "GAIN", fmt: function (p) { return db(48 * p - 24) } },
              { p: 1, label: "WIDTH", fmt: function (p) { return pct(2 * p) } },
              { p: 2, label: "MONO", fmt: function (p) { return p <= 0 ? "Off" : hz(expo(20, 500)(p)) } },
              { p: 3, label: "PAN", fmt: function (p) { return Math.abs(p - 0.5) < 0.005 ? "C" : (p < 0.5 ? "L" : "R") + Math.round(Math.abs(p - 0.5) * 200) } }],
      mode: null,
      defaults: { p0: 0.5, p1: 0.5, p2: 0, p3: 0.5, mix: 1 },
      presets: [{ name: "Mono Bass", values: { p2: expoP(120, 20, 500) } },
                { name: "Wide", values: { p1: 0.7 } },
                { name: "Mono", values: { p1: 0 } },
                { name: "−6 dB", values: { p0: linP(-6, -24, 24) } }] },
    { id: "convolve", name: "Convolve", category: "Space", visual: "impulse",
      blurb: "Any impulse response: rooms, springs, cabinets.",
      knobs: [{ p: 0, label: "PRE", fmt: function (p) { return ms(250 * p) } },
              { p: 1, label: "LOW CUT", fmt: function (p) { return hz(expo(20, 2000)(p)) } },
              { p: 2, label: "HIGH CUT", fmt: function (p) { return hz(expo(1000, 20000)(p)) } }],
      mode: null,
      defaults: { p0: 0, p1: 0, p2: 1, mix: 0.3 },
      presets: [{ name: "Close", values: { p1: expoP(80, 20, 2000), mix: 0.2 } },
                { name: "Dark Space", values: { p2: expoP(4000, 1000, 20000), mix: 0.35 } },
                { name: "Thin", values: { p1: expoP(300, 20, 2000), mix: 0.3 } }] },
    { id: "shifter", name: "Shifter", category: "Motion", visual: "shift",
      blurb: "Frequency shifter: inharmonic shifts, barber-pole feedback, ring mod.",
      knobs: [{ p: 0, label: "SHIFT", fmt: function (p) { return signedHz(shifterHz(p)) } },
              { p: 1, label: "FEEDBK", fmt: function (p) { return pct(0.9 * p) } },
              { p: 2, label: "SPREAD", fmt: pct }],
      mode: { p: 7, choices: ["Shift", "Ring"] },
      defaults: { p0: 0.6, p1: 0, p2: 0, p7: 0, mix: 0.5 },
      presets: [{ name: "Wide Detune", values: { p0: shifterP(4), p2: 1 } },
                { name: "Metallic Up", values: { p0: shifterP(120), mix: 0.6 } },
                { name: "Barber Pole", values: { p0: shifterP(30), p1: 0.7 } },
                { name: "Ring Bell", values: { p0: shifterP(440), p7: 1 } },
                { name: "Down Under", values: { p0: shifterP(-200) } }] },
    { id: "flyby", name: "Flyby", category: "Space", visual: "flyby",
      blurb: "Fly the sound past your head: Doppler, distance, air, ears.",
      knobs: [{ p: 1, label: "CLOSEST", fmt: function (p) { return metres(flybyMetres(p)) } },
              { p: 2, label: "SPEED", fmt: function (p) { return Fmt.Format.quantity(flybyKmh(p), "km/h") } },
              { p: 3, label: "RUN", fmt: function (p) { return flybyRuns[modeIndex(p, flybyRuns.length)] } },
              { p: 4, label: "LENGTH", fmt: function (p) { return flybyLengths[modeIndex(p, flybyLengths.length)] } },
              { p: 5, label: "POSITION", fmt: pct },
              { p: 6, label: "DOPPLER", fmt: function (p) { return pct(flybyMaxDoppler * p) } },
              { p: 7, label: "LISTEN", fmt: function (p) { return flybyListens[modeIndex(p, flybyListens.length)] } },
              { p: 8, label: "AIR", fmt: function (p) { return pct(flybyMaxAir * p) } },
              { p: 9, label: "ROOM", fmt: pct },
              { p: 10, label: "HEIGHT", fmt: function (p) { return metres(flybyMaxHeight * p) } }],
      mode: { p: 0, choices: flybyPaths },
      defaults: { p0: 0, p1: flybyMetresP(14), p2: flybyKmhP(90), p3: 0, p4: choiceP(3, 7), p5: 0,
                  p6: 1 / flybyMaxDoppler, p7: 0, p8: 1 / flybyMaxAir, p9: 0.35, p10: 0, mix: 1 },
      presets: [{ name: "Train", values: {} },
                { name: "Car Pass", values: { p1: flybyMetresP(6), p2: flybyKmhP(140) } },
                { name: "Jet Overhead", values: { p0: choiceP(flybyPath.overhead, flybyPaths.length),
                                                  p2: flybyKmhP(380), p10: 90 / flybyMaxHeight, p9: 0.5 } },
                { name: "Bee Orbit", values: { p0: choiceP(flybyPath.orbit, flybyPaths.length),
                                               p1: flybyMetresP(2), p2: flybyKmhP(12), p9: 0.2 } },
                { name: "Vocal Throw", values: { p1: flybyMetresP(4), p3: choiceP(flybyRun.sync, flybyRuns.length),
                                                 p4: choiceP(2, flybyLengths.length), p9: 0.25 } }] }
]

function module(id) {
    for (var i = 0; i < modules.length; ++i)
        if (modules[i].id === id)
            return modules[i]
    return null
}

function fxParam(uid, p) {
    return "nova.fx." + uid + "." + (p === "mix" ? "mix" : "p" + p)
}

// A slot's values for a module: its defaults under `over` (a preset's
// {p<k>, mix}), every p value set so nothing is left from the last module.
function slotValues(uid, spec, over) {
    const values = {}
    const defaults = spec !== null ? spec.defaults : {}
    const pick = function (key, fallback) {
        if (over && over[key] !== undefined)
            return Number(over[key])
        return defaults[key] !== undefined ? defaults[key] : fallback
    }
    for (let k = 0; k < fxSlotParams; ++k)
        values[fxParam(uid, k)] = pick("p" + k, 0)
    values[fxParam(uid, "mix")] = pick("mix", 1)
    return values
}

// A slot's live values from a telemetry frame (§6): input or output peak
// in dBFS, the detail values and the spectrum (null when not live).
function slotDb(frame, offset, uid) {
    return frame && frame.length >= T.size ? Number(frame[offset + uid - 1]) : floorDb
}
function slotDetail(frame, uid) {
    const out = []
    for (let i = 0; i < fxDetailValues; ++i)
        out.push(frame && frame.length >= T.size ? Number(frame[T.slotDetail + (uid - 1) * fxDetailValues + i]) : 0)
    return out
}
function slotSpectrum(frame, uid) {
    if (!frame || frame.length < T.size)
        return null
    const out = []
    const base = T.slotSpectrum + (uid - 1) * fxSpectrumBands
    for (let i = 0; i < fxSpectrumBands; ++i)
        out.push(Number(frame[base + i]))
    return out
}
// The centre of spectrum band i, Hz.
function spectrumHz(i) { return 20 * Math.pow(1000, (i + 0.5) / fxSpectrumBands) }

function delayTime(p0, sync) {
    if (sync)
        return delayDivisions[Math.round(p0 * (delayDivisions.length - 1))]
    return ms(expo(1, delayMaxMs)(p0))
}

// Index of a mode carried on a p value, and the p value for an index.
function modeIndex(p, count) { return count > 1 ? Math.round(p * (count - 1)) : 0 }
function modeValue(index, count) { return count > 1 ? index / (count - 1) : 0 }

// ---- Wavetable previews ----------------------------------------------------------------------
// A deterministic sketch of a built-in table's frame `t` (0..1) at phase x,
// shown until the engine provides frames (spec §6, "Wavetable stack frames").
function tableSample(table, t, x) {
    var ph = x * Math.PI * 2
    var v = 0, h
    switch (table) {
    case "basic_shapes": {
        var s = t * 3
        var shapesAt = [Math.sin(ph), 1 - 4 * Math.abs(((x + 0.25) % 1) - 0.5), 2 * ((x + 0.5) % 1) - 1, x < 0.5 ? 1 : -1]
        var i = Math.min(2, Math.floor(s))
        return shapesAt[i] * (1 - (s - i)) + shapesAt[i + 1] * (s - i)
    }
    case "reese_saw":
        for (h = 1; h < 24; ++h) v += Math.sin(ph * h + t * h * 0.4) / h * (1 + 0.3 * Math.sin(h * t * 3))
        return v * 0.6
    case "glass_formant":
        for (h = 1; h < 30; ++h) v += Math.sin(ph * h) / h * Math.exp(-Math.pow((h * 110 - (500 + t * 1800)) / 260, 2)) * 3
        return v + Math.sin(ph) * 0.3
    case "vowel_morph":
        for (h = 1; h < 28; ++h) v += Math.sin(ph * h) / Math.sqrt(h) * (Math.exp(-Math.pow((h - 3 - t * 6) / 2, 2)) + 0.6 * Math.exp(-Math.pow((h - 12 + t * 4) / 3, 2)))
        return v * 0.7
    case "fm_bell":
        return Math.sin(ph + (0.5 + t * 4) * Math.sin(ph * 3.5)) * 0.9
    case "grit_fold":
        return Math.sin(Math.sin(ph) * (1 + t * 6)) * 0.95
    case "analog_pulse": {
        var w = 0.5 - t * 0.45
        return (x < w ? 1 : -1) * 0.85 * (1 - 0.15 * Math.sin(ph * 2))
    }
    default:
        for (h = 1; h < 16; ++h) v += Math.sin(ph * h * (1 + t * 0.02) + h) / h * Math.cos(h * t * 2)
        return v * 0.8
    }
}

// Note names with middle C (key 60) as C3, as the arrangement shows them.
var noteNames = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"]
function noteName(key) {
    return noteNames[((key % 12) + 12) % 12] + Fmt.Format.number(Math.floor(key / 12) - 2, 0)
}

// ---- Filter sketches ---------------------------------------------------------------------
// The display frequency of fraction x (0..1): 20 Hz .. 20 kHz, logarithmic.
function displayHz(x) { return 20 * Math.pow(1000, x) }
function displayFraction(hzValue) { return Math.log(Math.max(20, hzValue) / 20) / Math.log(1000) }

function lp2(r, q) { return 1 / Math.sqrt(Math.pow(1 - r * r, 2) + Math.pow(r / q, 2)) }
function hp2(r, q) { return r * r * lp2(r, q) }
function bp2(r, q) { return (r / q) * lp2(r, q) }

// An analog-prototype sketch of model `model` (contract §2.4 order) in dB at
// `f` Hz for cutoff `fc` and resonance `res` (0..1). For drawing only; the
// engine's own response is the truth.
function filterDb(model, f, fc, res) {
    var r = f / Math.max(8, fc)
    var q = 0.707 + res * 9
    var m
    switch (model) {
    case 0: case 2: case 3: m = lp2(r, q) * lp2(r, 0.54); break      // Ladder 24, Diode, MG Low
    case 1: case 4: case 5: m = lp2(r, q); break                    // Ladder 12, OTA, LP 12
    case 6: m = lp2(r, q) * lp2(r, 0.54); break                     // LP 24
    case 7: m = bp2(r, q); break
    case 8: m = bp2(r, q) * bp2(r, 0.707); break
    case 9: m = hp2(r, q); break
    case 10: m = hp2(r, q) * hp2(r, 0.54); break
    case 11: m = Math.abs(1 - r * r) * lp2(r, q * 0.5 + 0.3); break // Notch
    case 12: m = 1 + 3 * res * bp2(r, q); break                     // Peak
    case 13: case 14:                                               // Formant, Vowel
        m = 0.08 + bp2(r, 4 + 6 * res) + 0.6 * bp2(f / (fc * 2.3), 5 + 6 * res) + 0.35 * bp2(f / (fc * 3.7), 6 + 6 * res)
        break
    case 15: case 16: {                                             // Comb + / −
        var g = 0.3 + 0.65 * res
        var ph = 2 * Math.PI * f / Math.max(8, fc)
        var s = model === 15 ? 1 : -1
        m = Math.sqrt(Math.pow(1 + s * g * Math.cos(ph), 2) + Math.pow(g * Math.sin(ph), 2)) / (1 + g)
        break
    }
    case 17: case 18: {                                             // Phaser, Flanger
        var depth = 0.6 + 0.35 * res
        var notches = model === 17 ? Math.abs(Math.cos(Math.PI * Math.log(Math.max(r, 1e-3)) * 1.2))
                                   : Math.abs(Math.cos(Math.PI * f / Math.max(8, fc)))
        m = 1 - depth * (1 - notches)
        break
    }
    case 19: {                                                      // LP→BP→HP at the mid point: BP
        m = bp2(r, q)
        break
    }
    case 20: m = bp2(r, q) + bp2(f / (fc * 2), q); break           // Dual Peak
    default: m = 0.5 + 0.5 * bp2(r, q); break                       // Ring
    }
    return 20 * Math.log(Math.max(m, 1e-5)) / Math.LN10
}
