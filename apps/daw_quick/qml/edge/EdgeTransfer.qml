import QtQuick

// Edge's transfer display: the input level (after Input gain, dBFS) across,
// the output level up, the 0 dBFS ceiling dashed, the line the curve leaves
// dotted, the curve from the engine, and a dot where the signal sits now.
// It fills its Kit.PlotFrame's plot area; the frame draws the scales from
// inTicks/outTicks at inAt/outAt.
Item {
    id: plot
    required property EdgeTheme theme
    // Output dB for inputs evenly spaced from inMin to inMax.
    property var curve: []
    property bool live: false
    property real dotIn: -120
    property real dotOut: -120

    readonly property real inMin: -24
    readonly property real inMax: 12
    readonly property real outMin: -24
    readonly property real outMax: 3
    readonly property var inTicks: [-24, -12, 0, 12]
    readonly property var outTicks: [-24, -12, -6, 0]
    // Where an input or output level sits across or up the plot, 0..1.
    function inAt(db) {
        return (db - inMin) / (inMax - inMin)
    }
    function outAt(db) {
        return (Math.max(outMin, Math.min(outMax, db)) - outMin) / (outMax - outMin)
    }
    function px(db) {
        return inAt(db) * width
    }
    function py(db) {
        return (1 - outAt(db)) * height
    }

    onCurveChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: plot.theme.screen
    }
    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = plot.theme
            ctx.lineWidth = t.hairline
            ctx.strokeStyle = t.grid
            for (const db of plot.inTicks) {
                ctx.beginPath()
                ctx.moveTo(plot.px(db), 0)
                ctx.lineTo(plot.px(db), plot.height)
                ctx.stroke()
            }
            for (const db of plot.outTicks) {
                ctx.beginPath()
                ctx.moveTo(0, plot.py(db))
                ctx.lineTo(plot.width, plot.py(db))
                ctx.stroke()
            }
            // Output = input, as far as the plot reaches.
            ctx.setLineDash([t.unityDashOn, t.dashOff])
            ctx.strokeStyle = t.unity
            ctx.beginPath()
            ctx.moveTo(plot.px(plot.inMin), plot.py(plot.inMin))
            ctx.lineTo(plot.px(plot.outMax), plot.py(plot.outMax))
            ctx.lineTo(plot.px(plot.inMax), plot.py(plot.outMax))
            ctx.stroke()
            ctx.setLineDash([t.dashOn, t.dashOff])
            ctx.strokeStyle = t.accent
            ctx.beginPath()
            ctx.moveTo(0, plot.py(0))
            ctx.lineTo(plot.width, plot.py(0))
            ctx.stroke()
            ctx.setLineDash([])
            const points = plot.curve
            if (points.length > 1) {
                ctx.lineWidth = t.curveStroke
                ctx.lineJoin = "round"
                ctx.beginPath()
                for (let i = 0; i < points.length; ++i) {
                    const db = plot.inMin + (plot.inMax - plot.inMin) * i / (points.length - 1)
                    if (i === 0)
                        ctx.moveTo(plot.px(db), plot.py(points[i]))
                    else
                        ctx.lineTo(plot.px(db), plot.py(points[i]))
                }
                ctx.stroke()
            }
        }
    }
    // The ceiling's name, at the left where the curve runs below it.
    Text {
        objectName: "edgeCeilingLabel"
        x: plot.theme.spaceSm
        y: plot.py(0) - height - plot.theme.spaceXxs
        text: "CEILING 0 dBFS"
        color: plot.theme.accent
        font.family: plot.theme.mono
        font.pixelSize: plot.theme.typeSmall
    }
    Rectangle {
        objectName: "edgeDot"
        visible: plot.live && plot.dotIn > plot.inMin
        readonly property real cx: plot.px(Math.min(plot.inMax, plot.dotIn))
        readonly property real cy: plot.py(plot.dotOut)
        x: cx - width / 2
        y: cy - height / 2
        width: 2 * plot.theme.dotRadius
        height: width
        radius: width / 2
        color: plot.theme.dotFill
        border.color: plot.theme.accent
        border.width: plot.theme.dotStroke
    }
}
