import QtQuick

// Cast's response view: the shaped impulse response as the engine plays it
// (telemetry: its envelope, the loudest response's peak per slice, filled
// in amber, 0 dB at its own peak) and its energy decay curve (a sea-glass
// line), over its length,
// on a decibel scale; the pre-delay shades the gap before it. It fills its
// Kit.PlotFrame's plot area; the frame draws the scales from dbTicks at
// levelAt and from timeTicks.
Item {
    id: view
    required property CastTheme theme
    property var envelope: []
    property var decayCurve: []
    property bool live: false
    // The IR's length and the pre-delay, in seconds.
    property real lengthSeconds: 0
    property real predelaySeconds: 0

    readonly property real floorDb: -72
    readonly property var dbTicks: [0, -24, -48]
    // The time axis spans the pre-delay and the IR.
    readonly property real spanSeconds: Math.max(minimumSpanSeconds, lengthSeconds + predelaySeconds)
    readonly property real minimumSpanSeconds: 0.1
    // Tick steps the time axis picks from, the first giving at most
    // maxTimeTicks ticks.
    readonly property var timeSteps: [0.05, 0.1, 0.25, 0.5, 1, 2, 5]
    readonly property int maxTimeTicks: 6
    readonly property real timeStep: {
        for (const step of timeSteps)
            if (spanSeconds / step <= maxTimeTicks)
                return step
        return timeSteps[timeSteps.length - 1]
    }
    readonly property var timeTicks: {
        const ticks = []
        for (let t = 0; t <= spanSeconds + timeStep / 2; t += timeStep)
            ticks.push(t)
        return ticks
    }

    // Where a level sits from the bottom (0) to the top (1).
    function levelAt(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / -floorDb))
    }
    function timeAt(seconds) {
        return Math.max(0, Math.min(1, seconds / spanSeconds))
    }

    onEnvelopeChanged: canvas.requestPaint()
    onDecayCurveChanged: canvas.requestPaint()
    onLengthSecondsChanged: canvas.requestPaint()
    onPredelaySecondsChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: view.theme.screen
    }
    Canvas {
        id: canvas
        objectName: "castIrCanvas"
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const w = width
            const h = height
            ctx.lineWidth = t.hairline
            ctx.strokeStyle = t.grid
            for (const db of view.dbTicks) {
                const y = h * (1 - view.levelAt(db))
                ctx.beginPath()
                ctx.moveTo(0, y)
                ctx.lineTo(w, y)
                ctx.stroke()
            }
            const start = w * view.timeAt(view.predelaySeconds)
            if (start > 0) {
                ctx.fillStyle = t.predelayShade
                ctx.fillRect(0, 0, start, h)
            }
            const span = w * view.timeAt(view.lengthSeconds)
            const points = view.envelope.length
            const peak = points > 0 ? Math.max(...view.envelope) : 0
            if (points > 1 && span > 0) {
                const step = span / points
                ctx.beginPath()
                ctx.moveTo(start, h)
                for (let i = 0; i < points; ++i) {
                    const y = h * (1 - view.levelAt(view.envelope[i] - peak))
                    ctx.lineTo(start + i * step, y)
                    ctx.lineTo(start + (i + 1) * step, y)
                }
                ctx.lineTo(start + span, h)
                ctx.closePath()
                ctx.fillStyle = Qt.rgba(t.accent.r, t.accent.g, t.accent.b, t.envelopeFillAlpha)
                ctx.fill()
                ctx.lineWidth = t.envelopeStroke
                ctx.strokeStyle = t.accent
                ctx.stroke()
            }
            const curve = view.decayCurve.length
            if (curve > 1 && span > 0) {
                const step = span / (curve - 1)
                ctx.beginPath()
                for (let i = 0; i < curve; ++i) {
                    const x = start + i * step
                    const y = h * (1 - view.levelAt(view.decayCurve[i]))
                    if (i === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                ctx.lineWidth = t.decayStroke
                ctx.strokeStyle = t.decay
                ctx.stroke()
            }
        }
    }
}
