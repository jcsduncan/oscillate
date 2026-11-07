.pragma library
.import Oscillate.Contracts 1.0 as Law
.import Oscillate.Format 1.0 as Fmt

// The Crate panel <-> engine contract (docs/specs/09-crate-sampler.md §2,
// §5.1; include/oscillate/native_dsp/synth/crate.hpp) as data the panel
// reads: telemetry offsets, pad layout, eras, chop modes and the panel's own
// shared sizes and timings. Change the spec and crate.hpp first.

// ---- Engine facts (crate.hpp) ---------------------------------------------------------
var maxChops = 32
var bankSize = 16
var bankCount = maxChops / bankSize
// C1 plays pad 1; bank B continues from 52.
var firstPadKey = 36
var padColumns = 4
var padRows = bankSize / padColumns

// Telemetry (crate.hpp telemetry::): 0..31 each slot's newest playhead
// (0..1 of the chop, -1 idle), then the last slot struck (-1 before any),
// its velocity and the last block's output peak in dBFS.
var T = {
    playheads: 0,
    lastSlot: maxChops,
    lastVelocity: maxChops + 1,
    outputPeakDb: maxChops + 2,
    size: maxChops + 3
}

// ---- Chop tags (spec 09 §1.1) and their colours -----------------------------------------
var tags = ["kick", "snare", "hat", "other"]
var tagColours = { kick: "#f0a23b", snare: "#58c4bb", hat: "#b8a8ff", other: "#e8e2d4" }
var tagNames = { kick: "Kick", snare: "Snare", hat: "Hat", other: "Chop" }

function tagColour(tag) {
    return tagColours[tag] !== undefined ? tagColours[tag] : tagColours.other
}
function chopName(chop) {
    if (!chop)
        return ""
    if (chop.name !== undefined && chop.name !== "")
        return chop.name
    return tagNames[chop.tag] !== undefined ? tagNames[chop.tag] : tagNames.other
}
// "06" for chop index 5.
function chopNumber(index) {
    var n = String(index + 1)
    return n.length < 2 ? "0" + n : n
}

// ---- Keys -------------------------------------------------------------------------------
// Octaves as on hardware samplers: MIDI 60 is C3, so the
// first pad (36) is C1.
function noteName(key) {
    if (key < 0)
        return ""
    return Law.Contracts.note_name(key, Law.Contracts.sharp, Law.Contracts.daw_middle_c)
}
function padKey(index) {
    return firstPadKey + index
}

// ---- Eras (spec 09 §2.4; crate.era choice order) ---------------------------------------
var eras = [
    { label: "CLEAN", name: "CLEAN · 24-bit",
      blurb: "Band-limited playback: pitch a chop up an octave and it stays clean." },
    { label: "SP-12", name: "SP-12 · 12-bit 26 kHz",
      blurb: "Gritty 12-bit at 26 kHz with no interpolation: crunchy drums, aliasing when pitched up, just like the classic." },
    { label: "MPC60", name: "MPC60 · 12-bit 40 kHz",
      blurb: "Warm 12-bit at 40 kHz with a soft output stage and the famous swing." },
    { label: "S950", name: "S950 · 12-bit, swept filter",
      blurb: "12-bit with a resonant anti-alias filter that follows the rate: dark, round chops." },
    { label: "TAPE", name: "TAPE · cassette",
      blurb: "Soft saturation, wow and flutter, and a gentle top-end roll-off." }
]
var times = ["REPITCH", "FOLLOW TEMPO"]

// ---- Chopping (spec 09 §3, §5.1) -------------------------------------------------------
var modes = [
    { id: "transient", label: "TRANSIENT", objectName: "crateModeTransient" },
    { id: "grid", label: "GRID", objectName: "crateModeGrid" },
    { id: "equal", label: "EQUAL", objectName: "crateModeEqual" },
    { id: "manual", label: "MANUAL", objectName: "crateModeManual" }
]
var gridDivisions = [4, 8, 16]
var gridLabels = ["1/4", "1/8", "1/16"]
var minEqualCount = 1
var maxEqualCount = maxChops

function modeIndex(id) {
    for (var i = 0; i < modes.length; ++i)
        if (modes[i].id === id)
            return i
    return 0
}
function percent(value) {
    return Fmt.Format.fraction(value)
}
function modeHint(crate) {
    if (!crate)
        return ""
    switch (crate.chop_mode) {
    case "grid":
        return crate.bpm > 0 ? "Cut on the beat grid · 1/" + crate.grid_division
                             : "Cut on the beat grid · needs the sample's tempo"
    case "equal":
        return "Cut into equal slices · " + crate.equal_count
    case "manual":
        return "Click the waveform to add a cut · double-click a marker to remove it"
    default:
        return "Cut at each hit · sensitivity " + percent(crate.sensitivity)
    }
}

// ---- The selected chop's controls (spec 09 §2.2) ----------------------------------------
var chopKnobs = ["pitch", "fine", "gain", "pan", "start", "length"]
var envelopeKnobs = ["attack", "hold", "decay", "sustain", "release"]
var filterKnobs = ["cutoff", "resonance", "choke"]
var playModes = ["ONE-SHOT", "GATE", "LOOP"]
// crate.chop.NN.filter choices: off, low-pass, high-pass, band-pass, notch.
var filterModes = ["OFF", "LP", "HP", "BP", "NOTCH"]
var characterKnobs = ["crate.bits", "crate.rate", "crate.drive", "crate.vinyl", "crate.wow", "crate.output"]

// ---- Audition and live state -------------------------------------------------------------
// A pad struck from the panel plays at this velocity (1..127).
var auditionVelocity = 100
// Telemetry poll while the panel shows (ms).
var pollInterval = 33
// Waveform: one bar per this many pixels (a bar and a gap).
var barPitch = 3
var barGap = 1

// Audio a drop may load (codec builds also take MP3 and FLAC).
var audioSuffixes = [".wav", ".wave", ".aif", ".aiff", ".flac", ".mp3"]
function isAudio(url) {
    var text = String(url).toLowerCase()
    for (var i = 0; i < audioSuffixes.length; ++i)
        if (text.length >= audioSuffixes[i].length
                && text.lastIndexOf(audioSuffixes[i]) === text.length - audioSuffixes[i].length)
            return true
    return false
}

// ---- Readouts -----------------------------------------------------------------------------
// The header's info line: "2 bars · 96 BPM · F minor · 44.1 kHz" style.
// Bars are shown to the nearest quarter bar.
var barSteps = 4
// A part bar shows its quarters; a tempo shows one decimal.
var quarterBarDecimals = 2, bpmDecimals = 1
function infoLine(crate) {
    if (!crate || !crate.has_sample)
        return ""
    var parts = []
    if (crate.bpm > 0) {
        var rounded = Math.round(crate.sample_bars * barSteps) / barSteps
        parts.push(Fmt.Format.number(rounded, Number.isInteger(rounded) ? 0 : quarterBarDecimals)
                   + (rounded === 1 ? " bar" : " bars"))
        parts.push(Fmt.Format.number(crate.bpm, bpmDecimals) + " BPM")
    } else {
        parts.push(Fmt.Format.seconds(crate.sample_seconds))
    }
    if (crate.key_name !== "")
        parts.push("root " + crate.key_name)
    if (crate.sample_rate > 0)
        parts.push(Fmt.Format.hertz(crate.sample_rate))
    return parts.join(" · ")
}
function milliseconds(seconds) {
    return Fmt.Format.seconds(seconds)
}

// Knob value text where the engine's generic format reads poorly.
function formatPan(value) {
    var amount = Math.round(Math.abs(value) * 100)
    return amount === 0 ? "C" : (value < 0 ? "L" : "R") + amount
}
function formatCount(text) {
    return text.charAt(0) === "+" ? text.substring(1) : text
}
function formatChoke(value) {
    return value < 0.5 ? "OFF" : String(Math.round(value))
}

// ---- Sketches ------------------------------------------------------------------------------
var minHz = 20
var maxHz = 20000
var filterFloorDb = -36
var filterCeilingDb = 18
// Resonance 0..1 maps onto the state-variable filter's Q.
var minQ = 0.707
var maxQ = 12
// The magnitude in dB of the chop's filter at `hz` (an ideal two-pole
// state-variable response, for the sketch only); 0 when off.
function filterDb(mode, cutoff, resonance, hz) {
    if (mode <= 0)
        return 0
    var w = hz / cutoff
    var q = minQ + (maxQ - minQ) * resonance * resonance
    var re = 1 - w * w
    var im = w / q
    var den = Math.sqrt(re * re + im * im)
    var num = mode === 1 ? 1 : mode === 2 ? w * w : mode === 3 ? im : Math.abs(re)
    return 20 * Math.log(Math.max(1e-6, num / den)) / Math.LN10
}
// The x fraction of `hz` on the sketch's log scale, and back.
function hzFraction(hz) {
    return Math.log(hz / minHz) / Math.log(maxHz / minHz)
}
function fractionHz(fraction) {
    return minHz * Math.pow(maxHz / minHz, fraction)
}
// The envelope sketch's time axis: each stage gets at least this share so
// a 1 ms attack still shows, and sustain is drawn this long.
var envelopeMinShare = 0.04
var envelopeSustainShare = 0.25
