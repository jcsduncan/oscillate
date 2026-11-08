.pragma library

// The strip's one decibel scale, shared by its meters, its fader and the
// scale drawn between them, so a mark, a meter reading and the fader cap at
// the same level line up: -60 dB at the bottom, +12 dB at the top.
const floorDb = -60
const ceilingDb = 12
// The unity mark: a fader here leaves the signal as it is.
const unityDb = 0
// The marked levels above the floor (the floor itself reads −∞).
const marks = [12, 6, 0, -6, -12, -24, -48]

// Decibels from linear gain (20 dB per decade of amplitude).
function toDb(gain) { return 20 * Math.log10(gain) }

// A level in decibels as a position on the scale, 0 (floor) to 1 (top).
function positionOfDb(db) {
    return Math.max(0, Math.min(1, (db - floorDb) / (ceilingDb - floorDb)))
}

// A linear gain as a position on the scale; silence sits on the floor.
function positionOf(gain) { return gain <= 0 ? 0 : positionOfDb(toDb(gain)) }

// A mark's label: a typographic minus, "+" above unity, "−∞" at the floor.
function label(db) {
    if (db <= floorDb)
        return "−∞"
    return (db > 0 ? "+" : "") + String(db).replace("-", "−")
}
