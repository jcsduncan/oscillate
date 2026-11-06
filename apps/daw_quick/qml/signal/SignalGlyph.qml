import QtQuick

// A small drawing of one of Signal's waves: a sine cycle, a sweep whose
// cycles crowd to the right, pink noise falling to the right, white noise
// level across, or a train of impulses. `inverted` draws it upside down
// (for the polarity check's right side).
Canvas {
    id: glyph
    property int wave: 0
    property color tone: "white"
    property real lineWidth: 1.6
    property bool inverted: false
    // The drawing's proportions: the wave's height either side of the
    // middle (share of the height), and the impulses' first position, pitch
    // (shares of the width) and overshoot past the wave's height.
    readonly property real amplitude: 0.38
    readonly property real impulseStart: 0.12
    readonly property real impulsePitch: 0.25
    readonly property real impulseReach: 1.15

    renderStrategy: Canvas.Cooperative
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const w = width
        const h = height
        const mid = h / 2
        const amp = h * amplitude * (inverted ? -1 : 1)
        ctx.strokeStyle = tone
        ctx.fillStyle = tone
        ctx.lineWidth = lineWidth
        ctx.lineJoin = "round"
        ctx.lineCap = "round"
        ctx.beginPath()
        if (wave === 0) {
            for (let i = 0; i <= 64; ++i) {
                const x = i / 64
                const y = mid - amp * Math.sin(2 * Math.PI * x)
                i === 0 ? ctx.moveTo(x * w, y) : ctx.lineTo(x * w, y)
            }
        } else if (wave === 1) {
            // Phase of an exponential sweep: cycles crowd toward the right.
            for (let i = 0; i <= 160; ++i) {
                const x = i / 160
                const phase = 1.2 * (Math.exp(x * 2.6) - 1)
                const y = mid - amp * Math.sin(2 * Math.PI * phase)
                i === 0 ? ctx.moveTo(x * w, y) : ctx.lineTo(x * w, y)
            }
        } else if (wave === 2 || wave === 3) {
            // Seeded, so the drawing is stable; pink leans on slow motion.
            let seed = wave === 2 ? 7 : 11
            const next = () => {
                seed = (seed * 1103515245 + 12345) % 2147483648
                return seed / 1073741824 - 1
            }
            let slow = 0
            for (let i = 0; i <= 48; ++i) {
                const x = i / 48
                slow = wave === 2 ? 0.7 * slow + 0.5 * next() : 0
                const v = wave === 2 ? slow + 0.35 * next() : next()
                const y = mid - amp * Math.max(-1, Math.min(1, v))
                i === 0 ? ctx.moveTo(x * w, y) : ctx.lineTo(x * w, y)
            }
        } else {
            ctx.moveTo(0, mid)
            ctx.lineTo(w, mid)
            for (let k = 0; k < 4; ++k) {
                const x = (impulseStart + k * impulsePitch) * w
                ctx.moveTo(x, mid)
                ctx.lineTo(x, mid - amp * impulseReach)
            }
        }
        ctx.stroke()
    }
    onWaveChanged: requestPaint()
    onToneChanged: requestPaint()
    onInvertedChanged: requestPaint()
}
