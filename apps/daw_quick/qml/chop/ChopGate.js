.pragma library

// Chop's pattern as the panel draws it, mirroring the engine
// (src/native_dsp/effects/chop.cpp and chop_table.hpp, ADR-0075): the
// steps, swing, rotate, ratchets and the gate's edges over one pattern.
// The engine is the sound; this is its picture.

// chop.mode's choices (chop_table::Mode).
var mode = { gate: 0, repeat: 1, gateRepeat: 2 }
// Swing at 100 % starts each even step half a step late (max_swing_delay):
// 50 % straight to 75 %.
var maxSwingDelay = 0.5
var straightPercent = 50
var swingPercent = 25
// Shape at Soft lets go over this many times Smooth (release_stretch).
var releaseStretch = 12
// Shape's names, by thirds of its travel.
var shapeNames = ["Hard", "Punch", "Soft"]
// A division's label ("1/16") as quarter-note beats.
var quartersPerWhole = 4
function divisionBeats(label) {
    return quartersPerWhole / Number(String(label).split("/")[1])
}
// The tempo the drawn edges assume before anything plays.
var previewBpm = 124
var secondsPerMinute = 60
var millisecondsPerSecond = 1000
// Steps a beat group spans on the grid.
var beatSteps = 4

// The lanes, top to bottom: each a per-step field of chop.step.<n>.
var lanes = [
    { field: "level", title: "Level", caption: "drag to draw", tall: true },
    { field: "length", title: "Length", caption: "gate per step" },
    { field: "ratchet", title: "Ratchet", caption: "1–4 hits" },
    { field: "repeat", title: "Repeat", caption: "off · 2 · 4 · 8×" },
    { field: "chance", title: "Chance", caption: "seeded" }
]

function stepId(step, field) {
    return "chop.step." + (step + 1) + "." + field
}

// A new seed from the current one, the same every time for the same seed
// and never the same seed: a full-period step through 1..maxSeed.
var reseedStride = 6151
function reseed(seed, minSeed, maxSeed) {
    var span = maxSeed - minSeed + 1
    return minSeed + ((seed - minSeed + reseedStride) % span + span) % span
}

function swingText(value) {
    return Math.round(straightPercent + swingPercent * value) + " %"
}
function shapeText(value) {
    return shapeNames[Math.min(shapeNames.length - 1, Math.floor(value * shapeNames.length))]
}

// The pattern's steps as the engine plays them: for slot k (where it
// sounds), the step it plays and where it starts and ends (0..1 of the
// pattern), after swing and rotate.
function slots(count, swing, rotate) {
    var delay = swing * maxSwingDelay
    var out = []
    for (var k = 0; k < count; ++k) {
        var pairStart = k - (k % 2)
        var start = k % 2 === 0 ? pairStart : pairStart + 1 + delay
        var end = k % 2 === 0 ? pairStart + 1 + delay : pairStart + 2
        out.push({ slot: k, step: ((k - rotate) % count + count) % count,
                   start: start / count, end: end / count })
    }
    return out
}

function edgeCurve(x) {
    return 0.5 - 0.5 * Math.cos(Math.PI * Math.max(0, Math.min(1, x)))
}

// The gate's level (0..1, before Depth) at x (0..1 of the pattern), from
// the steps' values `steps` ([{level, length, ratchet, chance}]),
// `played` (whether each step plays this time; all when unknown), the
// slots, and the edge and release lengths as fractions of a step.
function gateAt(x, steps, played, layout, edge, release) {
    var count = layout.length
    var k = 0
    while (k + 1 < count && x >= layout[k + 1].start)
        ++k
    var slot = layout[k]
    var s = steps[slot.step]
    var length = slot.end - slot.start
    var into = (x - slot.start) / length * count // In steps.
    var span = length * count
    var sub = span / s.ratchet
    var hit = Math.min(s.ratchet - 1, Math.floor(into / sub))
    var t = into - hit * sub
    var open = played[slot.step] !== false && s.length > 0
    if (!open)
        return 0
    var openFor = Math.min(sub, Math.max(s.length * sub, Math.min(edge, sub)))
    var rise = edge > 0 ? edgeCurve(t / edge) : 1
    if (t < openFor || openFor >= sub)
        return s.level * rise
    return s.level * (1 - edgeCurve((t - openFor) / Math.max(release, 1e-6)))
}
