import QtQuick
import Oscillate.Format

// Edge's transfer display: the input level (after Input gain, dBFS) across,
// the output level up, the 0 dBFS ceiling dashed, the line the curve leaves
// dotted, the curve from the engine, and a dot where the signal sits now.
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
    readonly property real leftEdge: theme.plotLeft
    readonly property real rightEdge: width - theme.plotRight
    readonly property real topEdge: theme.plotTop
    readonly property real bottomEdge: height - theme.plotBottom

    function px(db) {
        return leftEdge + (db - inMin) / (inMax - inMin) * (rightEdge - leftEdge)
    }
    function py(db) {
        const clamped = Math.max(outMin, Math.min(outMax, db))
        return bottomEdge - (clamped - outMin) / (outMax - outMin) * (bottomEdge - topEdge)
    }

    onCurveChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: plot.theme.radiusXl
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
                ctx.moveTo(plot.px(db), plot.topEdge)
                ctx.lineTo(plot.px(db), plot.bottomEdge)
                ctx.stroke()
            }
            for (const db of plot.outTicks) {
                ctx.beginPath()
                ctx.moveTo(plot.leftEdge, plot.py(db))
                ctx.lineTo(plot.rightEdge, plot.py(db))
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
            ctx.moveTo(plot.leftEdge, plot.py(0))
            ctx.lineTo(plot.rightEdge, plot.py(0))
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
    Text {
        objectName: "edgeCeilingLabel"
        x: plot.rightEdge - width - plot.theme.spaceXs
        y: plot.py(0) - height - plot.theme.spaceXxs
        text: "0 dBFS"
        color: plot.theme.accent
        font.family: plot.theme.mono
        font.pixelSize: plot.theme.typeSmall
    }
    Repeater {
        model: plot.outTicks
        delegate: Text {
            required property var modelData
            x: plot.theme.spaceXs
            y: plot.py(modelData) - plot.theme.plotLabelLift
            text: Format.signed_number(modelData, 0)
            color: plot.theme.inkFaint
            font.family: plot.theme.mono
            font.pixelSize: plot.theme.typeCaption
        }
    }
    Repeater {
        model: plot.inTicks
        delegate: Text {
            required property var modelData
            x: Math.min(plot.rightEdge - width, Math.max(plot.leftEdge, plot.px(modelData) - width / 2))
            y: plot.bottomEdge + plot.theme.spaceXs
            text: Format.signed_number(modelData, 0)
            color: plot.theme.inkFaint
            font.family: plot.theme.mono
            font.pixelSize: plot.theme.typeCaption
        }
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
