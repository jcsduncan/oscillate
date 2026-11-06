.pragma library

// Flyby's path geometry (flyby_table.hpp), in metres around the listener,
// who faces +y with the ears on the x axis. A Pass is a cubic Bézier
// through four points; an Orbit a circle that starts straight ahead and
// turns clockwise (to the right first) or anticlockwise.

var pass = 0
var orbit = 1
var clockwise = 0
// Rings on the chart every this many metres.
var ringMetres = 10
// The chart shows at least and at most this far from you.
var minView = 20
var maxView = 150
// The view fits the path with this much room round it.
var viewMargin = 1.15
// The speed of sound (m/s), and how much slower the chart's wavefronts
// travel so they can be seen.
var speedOfSound = 343
var waveSlowdown = 8

function bezier(pts, t) {
    var s = 1 - t
    var a = s * s * s, b = 3 * s * s * t, c = 3 * s * t * t, d = t * t * t
    return [a * pts[0][0] + b * pts[1][0] + c * pts[2][0] + d * pts[3][0],
            a * pts[0][1] + b * pts[1][1] + c * pts[2][1] + d * pts[3][1]]
}

function orbitAt(radius, direction, u) {
    var turn = 2 * Math.PI * u
    var side = direction === clockwise ? 1 : -1
    return [side * radius * Math.sin(turn), radius * Math.cos(turn)]
}

// `count` + 1 points along the path, start to end.
function sample(kind, pts, radius, direction, count) {
    var out = []
    for (var i = 0; i <= count; ++i) {
        var u = i / count
        out.push(kind === orbit ? orbitAt(radius, direction, u) : bezier(pts, u))
    }
    return out
}

// How far from you the chart must reach to show the path.
function reach(kind, pts, radius) {
    var far = 0
    if (kind === orbit)
        far = radius
    else
        for (var i = 0; i < pts.length; ++i)
            far = Math.max(far, Math.abs(pts[i][0]), Math.abs(pts[i][1]))
    return Math.max(minView, Math.min(maxView, far * viewMargin))
}
