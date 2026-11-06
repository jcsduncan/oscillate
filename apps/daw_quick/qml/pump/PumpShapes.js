.pragma library
.import Oscillate.Contracts 1.0 as Law

// Pump's shape library and curve law, as the panel reads them. The shapes
// mirror src/native_dsp/effects/pump_presets.hpp exactly (Classic Pump
// first, the default); the law is the engine's, read from Contracts (spec
// 08a §3.2). Points are {x, y, tension, step}: x and y 0..1, y the
// volume (1 untouched), tension the bend of the segment to the next.

// Slot A's curve; B to D are "shape.b" to "shape.d", and Three Band gives
// each slot "<slot>.mid" and "<slot>.high" (pump.hpp).
var curveId = "shape"
var slotNames = ["A", "B", "C", "D"]
var bandNames = ["LOW", "MID", "HIGH"]
// The most points a document curve holds (project::max_native_curve_points).
var maxPoints = 64
// The closest two points may sit and survive the document's tidy (1/512).
var gap = 0.004

function curveIdFor(slot, band) {
    var id = curveId + (slot > 0 ? "." + "abcd".charAt(slot) : "")
    return id + (band === 1 ? ".mid" : band === 2 ? ".high" : "")
}

// "C1" for 36: middle C (60) is C3, as samplers number octaves.
function noteName(note) {
    if (note < 0)
        return "—"
    return Law.Contracts.note_name(note, Law.Contracts.ascii, Law.Contracts.daw_middle_c)
}

function p(x, y, tension) {
    return { x: x, y: y, tension: tension, step: false }
}

var shapes = [
    { name: "Classic Pump", points: [p(0, 0, 0), p(0.06, 0.05, 0.5), p(0.42, 0.97, -0.4), p(1, 1, 0)] },
    { name: "Fast Release", points: [p(0, 0, 0), p(0.03, 0.1, 0.6), p(0.2, 1, 0), p(1, 1, 0)] },
    { name: "Long Tail", points: [p(0, 0, 0), p(0.08, 0.05, 0.2), p(0.85, 1, -0.6), p(1, 1, 0)] },
    { name: "Gate", points: [p(0, 0, 0), p(0.18, 0, 0), p(0.2, 1, 0), p(1, 1, 0)] },
    { name: "Soft Duck", points: [p(0, 0.45, 0), p(0.05, 0.5, 0.3), p(0.5, 1, -0.3), p(1, 1, 0)] },
    { name: "Hold & Snap", points: [p(0, 0, 0), p(0.3, 0.05, 0.9), p(0.38, 1, 0), p(1, 1, 0)] },
    { name: "Breathe", points: [p(0, 0.2, 0), p(0.5, 1, 0.5), p(1, 0.2, 0)] },
    { name: "Swell", points: [p(0, 0, 0), p(0.9, 1, 0.7), p(1, 1, 0)] },
    { name: "Off-Beat", points: [p(0, 1, 0), p(0.45, 1, 0), p(0.5, 0, 0.4), p(0.95, 1, -0.3), p(1, 1, 0)] },
    { name: "Stutter", points: [p(0, 0, 0), p(0.1, 1, 0), p(0.25, 0, 0), p(0.35, 1, 0), p(0.5, 0, 0),
                                p(0.6, 1, 0), p(1, 1, 0)] },
    { name: "Ducker", points: [p(0, 0.2, 0), p(0.04, 0.2, 0.4), p(0.3, 1, -0.2), p(1, 1, 0)] }
]

// The volume a point leaves untouched, and what an empty curve reads.
var untouched = 1

// The drawn volume at x (0..1), and count + 1 volumes across the cycle, by
// the engine's curve law.
function gainAt(points, x) {
    return Law.Contracts.curve_at(points, x, untouched)
}
function sampled(points, count) {
    return Law.Contracts.sample_curve(points, count, untouched)
}

// The library entry these points are, or -1.
function match(points) {
    for (var s = 0; s < shapes.length; ++s) {
        var q = shapes[s].points
        if (q.length !== points.length)
            continue
        var same = true
        for (var i = 0; i < q.length && same; ++i) {
            same = Math.abs(q[i].x - points[i].x) < 1e-6 && Math.abs(q[i].y - points[i].y) < 1e-6
                && Math.abs(q[i].tension - points[i].tension) < 1e-6 && !points[i].step
        }
        if (same)
            return s
    }
    return -1
}

// Beats in one cycle: pump.rate (1/16 .. 2 bars, a 4/4 bar) times the feel.
function cycleBeats(rate, feel) {
    var beats = [0.25, 0.5, 1, 2, 4, 8][Math.max(0, Math.min(5, rate))]
    return beats * [1, 1.5, 2 / 3][Math.max(0, Math.min(2, feel))]
}

// A synthetic 909-style kick's peak envelope (a click, then a body falling
// over about 320 ms), for the preview before any audio plays: `points`
// peaks over `seconds`.
function previewKick(seconds, points) {
    var out = []
    for (var i = 0; i < points; ++i) {
        var t = seconds * i / points
        var body = Math.exp(-t * 8.5)
        out.push(t < 0.002 ? 1 : 0.92 * body)
    }
    return out
}

// The x where the kick envelope `kick` (peaks over one cycle) has fallen
// below `fraction` of its peak after it, or -1 when there is no kick.
function releaseAt(kick, fraction) {
    var top = 0, at = 0
    for (var i = 0; i < kick.length; ++i) {
        if (kick[i] > top) {
            top = kick[i]
            at = i
        }
    }
    if (top <= 1e-4)
        return -1
    for (var j = at; j < kick.length; ++j) {
        if (kick[j] < fraction * top)
            return j / kick.length
    }
    return 1
}

// A Classic-Pump-like duck that recovers at `release` (0..1).
function fitted(release) {
    var r = Math.max(0.06, Math.min(0.95, release))
    return [p(0, 0, 0), p(Math.min(0.06, r * 0.15), 0.05, 0.5), p(r, 0.97, -0.4), p(1, 1, 0)]
}

// Upside down: what was ducked plays and what played ducks.
function flipped(points) {
    var out = []
    for (var i = 0; i < points.length; ++i)
        out.push({ x: points[i].x, y: 1 - points[i].y, tension: points[i].tension, step: points[i].step })
    return out
}

function copyPoint(q, x, y) {
    return { x: x !== undefined ? x : q.x, y: y !== undefined ? y : q.y,
             tension: q.tension || 0, step: q.step === true }
}
function byX(a, b) { return a.x - b.x }

// Sorted, ends pinned to 0 and 1, points closer than `gap` dropped (a later
// point at the same place wins), and no more than maxPoints.
function tidied(points) {
    var sorted = points.slice().sort(byX)
    var out = []
    for (var i = 0; i < sorted.length; ++i) {
        var q = sorted[i]
        if (out.length > 0 && q.x - out[out.length - 1].x < gap * 0.5)
            out[out.length - 1] = q
        else
            out.push(q)
    }
    if (out.length < 2)
        return out
    out[0] = copyPoint(out[0], 0)
    out[out.length - 1] = copyPoint(out[out.length - 1], 1)
    return limited(out)
}

// Merges runs of equal flat steps, then thins evenly, until it fits.
function limited(points) {
    var out = []
    for (var i = 0; i < points.length; ++i) {
        var q = points[i], last = out[out.length - 1]
        if (last && last.step && q.step && Math.abs(last.y - q.y) < 1e-9 && i + 1 < points.length)
            continue
        out.push(q)
    }
    while (out.length > maxPoints) {
        var thinned = [out[0]]
        for (var j = 1; j + 1 < out.length; ++j)
            if (j % 2 === 1 || out.length - (j - thinned.length) <= maxPoints)
                thinned.push(out[j])
        thinned.push(out[out.length - 1])
        if (thinned.length === out.length)
            break
        out = thinned
    }
    return out
}

// The segments of a curve: start and end x, the value leaving the start
// and arriving at the end, and the bend.
function segments(points) {
    var out = []
    for (var i = 0; i + 1 < points.length; ++i) {
        var a = points[i], b = points[i + 1]
        out.push({ x0: a.x, x1: b.x, v0: a.y, v1: a.step ? a.y : b.y,
                   tension: a.tension || 0, step: a.step === true })
    }
    return out
}

// Points from segments laid end to end. Where one ends at a different
// level than the next starts, a flat one becomes a step (it holds, then
// jumps); a sloped one gets a point just before the join to keep the edge.
function fromSegments(list) {
    var out = []
    for (var i = 0; i < list.length; ++i) {
        var s = list[i]
        var prev = list[i - 1]
        if (prev && Math.abs(prev.v1 - s.v0) > 1e-9) {
            if (Math.abs(prev.v0 - prev.v1) < 1e-9)
                out[out.length - 1].step = true
            else if (s.x0 - gap > prev.x0 + gap)
                out.push({ x: s.x0 - gap, y: prev.v1, tension: 0, step: false })
        }
        out.push({ x: s.x0, y: s.v0, tension: s.tension, step: s.step })
    }
    var end = list[list.length - 1]
    out.push({ x: 1, y: end.v1, tension: 0, step: false })
    return tidied(out)
}

// Back to front in time: the cycle played in reverse.
function mirrored(points) {
    if (points.length < 2)
        return points
    var list = segments(points)
    var out = []
    for (var i = list.length - 1; i >= 0; --i) {
        var s = list[i]
        out.push({ x0: 1 - s.x1, x1: 1 - s.x0, v0: s.v1, v1: s.v0,
                   tension: s.step ? 0 : -s.tension, step: s.step })
    }
    return fromSegments(out)
}

// Rotated by `delta` of the cycle (wrapping): the cycle repeats, so what
// leaves one end comes in at the other.
function shifted(points, delta) {
    if (points.length < 2)
        return points
    delta = delta - Math.floor(delta)
    if (delta < 1e-9)
        return points
    var list = segments(points)
    var out = []
    // The segment the new start falls in splits there, by value.
    var cut = 1 - delta
    for (var i = 0; i < list.length; ++i) {
        var s = list[i]
        if (s.x0 < cut - 1e-9 && s.x1 > cut + 1e-9) {
            var mid = gainAt(points, cut)
            var straight = s.tension !== 0
            out.push({ x0: s.x0, x1: cut, v0: s.v0, v1: s.step ? s.v0 : mid,
                       tension: straight ? 0 : s.tension, step: s.step })
            out.push({ x0: cut, x1: s.x1, v0: s.step ? s.v0 : mid, v1: s.v1,
                       tension: straight ? 0 : s.tension, step: s.step })
        } else {
            out.push(s)
        }
    }
    var moved = []
    for (var j = 0; j < out.length; ++j) {
        var m = out[j]
        var x0 = m.x0 + delta, x1 = m.x1 + delta
        if (x0 >= 1 - 1e-9) {
            x0 -= 1
            x1 -= 1
        }
        moved.push({ x0: x0, x1: x1, v0: m.v0, v1: m.v1, tension: m.tension, step: m.step })
    }
    moved.sort(function (a, b) { return a.x0 - b.x0 })
    return fromSegments(moved)
}

// Cells of an n-step grid painted as flat steps: `cells` maps a cell index
// to its level. Outside the painted cells the curve keeps its shape.
function painted(points, n, cells) {
    var keys = Object.keys(cells).map(Number).filter(function (k) { return k >= 0 && k < n })
    if (keys.length === 0)
        return points
    keys.sort(function (a, b) { return a - b })
    var inside = function (x) {
        if (x >= 1 - 1e-9)
            return cells[n - 1] !== undefined
        return cells[Math.floor(x * n + 1e-9)] !== undefined
    }
    var out = []
    for (var i = 0; i < points.length; ++i)
        if (!inside(points[i].x))
            out.push(copyPoint(points[i]))
    var hasPoint = function (x) {
        for (var j = 0; j < out.length; ++j)
            if (Math.abs(out[j].x - x) < 1e-9)
                return true
        return false
    }
    var added = []
    for (var k = 0; k < keys.length; ++k) {
        var cell = keys[k], start = cell / n, end = (cell + 1) / n
        // The curve arrives at the run as it was, then steps.
        if (cell > 0 && cells[cell - 1] === undefined)
            added.push({ x: start - gap, y: gainAt(points, start), tension: 0, step: false })
        added.push({ x: start, y: cells[cell], tension: 0, step: true })
        if (cells[cell + 1] !== undefined)
            continue
        if (end >= 1 - 1e-9) {
            added.push({ x: 1, y: cells[cell], tension: 0, step: false })
        } else if (!hasPoint(end)) {
            // The old curve resumes where the run ends.
            var resume = segments(points).filter(function (s) { return s.x0 <= end && s.x1 > end })[0]
            added.push({ x: end, y: gainAt(points, end), tension: 0,
                         step: resume !== undefined && resume.step })
        }
    }
    return tidied(out.concat(added))
}

// A straight ramp from (x0, y0) to (x1, y1) replacing the curve between.
function lined(points, x0, y0, x1, y1) {
    if (x1 < x0) {
        var tx = x0; x0 = x1; x1 = tx
        var ty = y0; y0 = y1; y1 = ty
    }
    if (x1 - x0 < gap)
        return points
    var out = []
    for (var i = 0; i < points.length; ++i)
        if (points[i].x < x0 - 1e-9 || points[i].x > x1 + 1e-9)
            out.push(copyPoint(points[i]))
    if (x0 > 1e-9)
        out.push({ x: Math.max(0, x0 - gap), y: gainAt(points, x0), tension: 0, step: false })
    out.push({ x: x0, y: y0, tension: 0, step: false })
    out.push({ x: x1, y: y1, tension: 0, step: false })
    if (x1 < 1 - 1e-9)
        out.push({ x: Math.min(1, x1 + gap), y: gainAt(points, x1), tension: 0, step: false })
    return tidied(out)
}

// A library shape squeezed into [x0, x1) of the cycle.
function stamped(points, shape, x0, x1) {
    if (x1 - x0 < gap * 4)
        return points
    var out = []
    for (var i = 0; i < points.length; ++i)
        if (points[i].x < x0 - 1e-9 || points[i].x >= x1 - 1e-9)
            out.push(copyPoint(points[i]))
    if (x0 > 1e-9)
        out.push({ x: x0 - gap, y: gainAt(points, x0), tension: 0, step: false })
    for (var j = 0; j < shape.length; ++j) {
        var q = shape[j]
        var x = x0 + q.x * (x1 - x0)
        if (j + 1 === shape.length && x1 < 1 - 1e-9)
            x = x1 - gap
        out.push({ x: x, y: q.y, tension: q.tension || 0, step: q.step === true })
    }
    if (x1 < 1 - 1e-9)
        out.push({ x: x1, y: gainAt(points, x1), tension: 0, step: false })
    return tidied(out)
}

// Points `indices` moved together by (dx, dy): x kept between the unmoved
// neighbours (the ends keep theirs), y within 0..1.
function movedGroup(points, indices, dx, dy) {
    var chosen = {}
    for (var i = 0; i < indices.length; ++i)
        chosen[indices[i]] = true
    // How far the group may slide before it meets an unmoved point.
    var low = -1, high = 1
    for (var j = 0; j < points.length; ++j) {
        if (!chosen[j])
            continue
        if (j === 0 || j === points.length - 1) {
            low = 0
            high = 0
            continue
        }
        var k = j - 1
        while (k >= 0 && chosen[k]) --k
        if (k >= 0)
            low = Math.max(low, points[k].x + gap - points[j].x)
        var m = j + 1
        while (m < points.length && chosen[m]) ++m
        if (m < points.length)
            high = Math.min(high, points[m].x - gap - points[j].x)
    }
    var move = Math.max(low, Math.min(high, dx))
    var out = []
    for (var n = 0; n < points.length; ++n) {
        var q = points[n]
        out.push(chosen[n] ? copyPoint(q, q.x + move, Math.max(0, Math.min(1, q.y + dy)))
                           : copyPoint(q))
    }
    return out
}

// Indices of the points inside a box (curve units, any corner order).
function within(points, x0, y0, x1, y1) {
    var out = []
    var left = Math.min(x0, x1), right = Math.max(x0, x1)
    var bottom = Math.min(y0, y1), top = Math.max(y0, y1)
    for (var i = 0; i < points.length; ++i)
        if (points[i].x >= left && points[i].x <= right && points[i].y >= bottom && points[i].y <= top)
            out.push(i)
    return out
}

// A musical random shape on an n-step grid from `seed`: a duck on the
// first step, then levels that mostly rise, some steps held, some glides.
function randomized(n, seed) {
    var state = (Math.floor(seed) % 2147483647 + 2147483647) % 2147483647 || 1
    var next = function () {
        state = state * 48271 % 2147483647
        return state / 2147483647
    }
    n = Math.max(2, Math.min(32, n))
    var levels = [0, 0.15, 0.35, 0.6, 0.8, 1]
    var out = [{ x: 0, y: levels[Math.floor(next() * 2)], tension: 0, step: next() < 0.4 }]
    var level = out[0].y
    for (var k = 1; k < n; ++k) {
        var r = next()
        if (r < 0.35)
            continue // Held: the step before plays on.
        var target = r < 0.8 ? Math.min(1, level + levels[1 + Math.floor(next() * 4)])
                             : levels[Math.floor(next() * levels.length)]
        level = target
        out.push({ x: k / n, y: level, tension: next() < 0.5 ? (next() - 0.5) * 1.2 : 0,
                   step: next() < 0.45 })
    }
    out.push({ x: 1, y: Math.max(level, 0.85), tension: 0, step: false })
    return tidied(out)
}

// The clipboard text for a shape, and the shape back from clipboard text
// (null unless it is one).
function clipboardText(points) {
    return JSON.stringify({ pumpShape: 1, points: points.map(function (q) { return copyPoint(q) }) })
}
function parsedClipboard(text) {
    var data
    try {
        data = JSON.parse(text)
    } catch (e) {
        return null
    }
    var list = data && data.pumpShape === 1 ? data.points : null
    if (!Array.isArray(list) || list.length < 2 || list.length > maxPoints)
        return null
    var out = []
    for (var i = 0; i < list.length; ++i) {
        var q = list[i]
        if (typeof q !== "object" || !isFinite(q.x) || !isFinite(q.y) || q.x < 0 || q.x > 1
                || q.y < 0 || q.y > 1)
            return null
        var tension = isFinite(q.tension) ? Math.max(-1, Math.min(1, q.tension)) : 0
        out.push({ x: q.x, y: q.y, tension: tension, step: q.step === true })
    }
    return tidied(out)
}
