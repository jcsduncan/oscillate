import QtQuick

// Strike's view: the last two seconds scrolling right to left. Each slice's
// input peak is mirrored about the centre on a decibel scale as a grey
// ghost, what Strike let out drawn over it as a light wash and its outline;
// over both, each band's gain as
// a line in the band's colour, 0 dB at the centre, a lift rising and a cut
// falling, on the gain scale the frame labels (gainTicks at gainAt). With
// Clip on, the ceiling is dashed. It fills its Kit.PlotFrame's plot area.
Item {
    id: view
    required property StrikeTheme theme
    // Per slice, oldest first: the input's and the output's peak (linear),
    // and per band its gain furthest from 0 dB (dB).
    property var inputs: []
    property var outputs: []
    property var gains: []
    property int bands: 1
    property bool live: false
    property bool clipping: false
    property real ceilingDb: 0

    readonly property real floorDb: -48
    readonly property real gainRangeDb: 18
    readonly property var gainTicks: [18, 9, 0, -9, -18]
    // Where a level sits from the centre (0) to the top edge (1).
    function levelAt(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / -floorDb))
    }
    // Where a gain sits from the bottom (0) to the top (1).
    function gainAt(db) {
        return Math.max(0, Math.min(1, 0.5 + db / (2 * gainRangeDb)))
    }
    function dbOf(gain) {
        return gain > 0 ? 20 * Math.log(gain) / Math.LN10 : floorDb
    }

    onInputsChanged: canvas.requestPaint()
    onOutputsChanged: canvas.requestPaint()
    onGainsChanged: canvas.requestPaint()
    onBandsChanged: canvas.requestPaint()
    onClippingChanged: canvas.requestPaint()
    onCeilingDbChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: view.theme.screen
    }
    Canvas {
        id: canvas
        objectName: "strikeViewCanvas"
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const w = width
            const h = height
            const mid = h / 2
            ctx.lineWidth = t.hairline
            ctx.strokeStyle = t.grid
            for (const db of view.gainTicks) {
                const y = h * (1 - view.gainAt(db))
                ctx.beginPath()
                ctx.moveTo(0, y)
                ctx.lineTo(w, y)
                ctx.stroke()
            }

            const count = view.inputs.length
            if (count > 0) {
                const slot = w / count
                const barWidth = Math.max(t.hairline, slot - t.sliceGap)
                ctx.fillStyle = t.held
                for (let i = 0; i < count; ++i) {
                    const held = view.levelAt(view.dbOf(view.inputs[i])) * mid
                    if (held > 0)
                        ctx.fillRect(i * slot, mid - held, barWidth, 2 * held)
                }
                // What Strike let out: a light wash and its outline, so a
                // lift shows above the ghost and a cut inside it.
                const outCount = Math.min(count, view.outputs.length)
                ctx.globalAlpha = t.outputWashAlpha
                ctx.fillStyle = t.output
                for (let i = 0; i < outCount; ++i) {
                    const out = view.levelAt(view.dbOf(view.outputs[i])) * mid
                    if (out > 0)
                        ctx.fillRect(i * slot, mid - out, barWidth, 2 * out)
                }
                ctx.globalAlpha = 1
                ctx.lineWidth = t.hairline
                ctx.strokeStyle = t.output
                for (const side of [-1, 1]) {
                    ctx.beginPath()
                    for (let i = 0; i < outCount; ++i) {
                        const y = mid + side * view.levelAt(view.dbOf(view.outputs[i])) * mid
                        if (i === 0)
                            ctx.moveTo((i + 0.5) * slot, y)
                        else
                            ctx.lineTo((i + 0.5) * slot, y)
                    }
                    ctx.stroke()
                }
            }

            ctx.strokeStyle = t.centre
            ctx.beginPath()
            ctx.moveTo(0, mid)
            ctx.lineTo(w, mid)
            ctx.stroke()

            if (view.clipping) {
                const ceiling = view.levelAt(view.ceilingDb) * mid
                ctx.lineWidth = t.ceilingStroke
                ctx.strokeStyle = t.warning
                ctx.setLineDash([t.dashOn, t.dashOff])
                ctx.beginPath()
                ctx.moveTo(0, mid - ceiling)
                ctx.lineTo(w, mid - ceiling)
                ctx.moveTo(0, mid + ceiling)
                ctx.lineTo(w, mid + ceiling)
                ctx.stroke()
                ctx.setLineDash([])
            }

            ctx.lineWidth = t.traceStroke
            ctx.lineJoin = "round"
            for (let b = 0; b < Math.min(view.bands, view.gains.length); ++b) {
                const trace = view.gains[b]
                if (!trace || trace.length === 0)
                    continue
                const slot = w / trace.length
                ctx.strokeStyle = t.bandColours[b]
                ctx.beginPath()
                for (let i = 0; i < trace.length; ++i) {
                    const x = (i + 0.5) * slot
                    const y = h * (1 - view.gainAt(trace[i]))
                    if (i === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
        }
    }
}
