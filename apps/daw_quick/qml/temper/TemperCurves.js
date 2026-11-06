.pragma library

// Temper's drawn curves, as the engine shapes them (temper.cpp): a dynamic
// band's analog bell or shelf at full range, and the sensitivity curve
// through its nodes (cosine between neighbours in log frequency, held flat
// past the outer nodes).

// Band kinds, in temper_table order.
var bell = 0
var lowShelf = 1
var highShelf = 2

// The band's response in dB at hz, at its full range.
function bandDb(kind, freq, q, rangeDb, hz) {
    var x = hz / freq
    var a = Math.pow(10, rangeDb / 40)
    var x2 = x * x
    var re, im, dre, dim
    if (kind === bell) {
        // (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1), s = jx
        re = 1 - x2; im = x * a / q
        dre = 1 - x2; dim = x / (a * q)
    } else {
        var k = Math.sqrt(a) / q
        if (kind === lowShelf) {
            // A (s^2 + k s + A) / (A s^2 + k s + 1)
            re = a * (a - x2); im = a * k * x
            dre = 1 - a * x2; dim = k * x
        } else {
            // A (A s^2 + k s + 1) / (s^2 + k s + A)
            re = a * (1 - a * x2); im = a * k * x
            dre = a - x2; dim = k * x
        }
    }
    return 10 * Math.log10((re * re + im * im) / Math.max(1e-30, dre * dre + dim * dim))
}

// nodes: [{ hz, db }] in any order.
function sensitivityDb(nodes, hz) {
    if (nodes.length === 0)
        return 0
    var s = nodes.slice().sort(function (a, b) { return a.hz - b.hz })
    if (hz <= s[0].hz)
        return s[0].db
    if (hz >= s[s.length - 1].hz)
        return s[s.length - 1].db
    var x = Math.log(hz)
    for (var i = 0; i < s.length - 1; ++i) {
        var a = Math.log(s[i].hz), b = Math.log(s[i + 1].hz)
        if (x <= b) {
            var u = b > a ? (x - a) / (b - a) : 1
            var k = (1 - Math.cos(u * Math.PI)) / 2
            return s[i].db + (s[i + 1].db - s[i].db) * k
        }
    }
    return 0
}
