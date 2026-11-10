pragma ComponentBehavior: Bound
import QtQuick

// A role's EQ response (the controller's curve: dB at log-spaced
// frequencies from lowHz to highHz) over a ±rangeDb well, with a dot where
// each band sits. Drawn, not edited: the knobs below set it.
Item {
    id: plot
    required property RebalanceTheme style
    // dB values, log-spaced from lowHz to highHz.
    property var curve: []
    required property real lowHz
    required property real highHz
    required property real rangeDb
    property color color: style.accent
    // The bands' centres, in Hz.
    property var bands: []
    property bool active: true

    function xOf(hz) {
        return width * Math.log(hz / lowHz) / Math.log(highHz / lowHz)
    }
    function yOf(db) {
        const clamped = Math.max(-rangeDb, Math.min(rangeDb, db))
        return height / 2 - clamped / rangeDb * (height / 2 - style.curveStroke)
    }
    // The response at hz, read between the curve's points.
    function dbAt(hz) {
        if (curve.length < 2)
            return 0
        const position = Math.log(hz / lowHz) / Math.log(highHz / lowHz) * (curve.length - 1)
        const index = Math.max(0, Math.min(curve.length - 2, Math.floor(position)))
        const fraction = Math.max(0, Math.min(1, position - index))
        return Number(curve[index]) * (1 - fraction) + Number(curve[index + 1]) * fraction
    }

    onCurveChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onActiveChanged: canvas.requestPaint()
    Accessible.role: Accessible.Graphic
    Accessible.name: "EQ response"

    Rectangle {
        anchors.fill: parent
        radius: plot.style.radiusMd
        color: plot.style.well
        border.color: plot.style.wellEdge
    }
    Repeater {
        model: plot.style.curveGridHz.length
        delegate: Item {
            id: gridLine
            required property int index
            readonly property real hz: plot.style.curveGridHz[index]
            x: plot.xOf(hz)
            height: plot.height
            Rectangle {
                width: plot.style.hairline
                height: parent.height
                color: plot.style.line
                opacity: plot.style.curveGridAlpha
            }
            Text {
                x: plot.style.spaceXxs
                y: plot.height - height - plot.style.spaceXxs
                text: plot.style.curveGridLabels[gridLine.index]
                color: plot.style.textFaint
                font.pixelSize: plot.style.typeMicro
                font.family: plot.style.monoFamily
            }
        }
    }
    Rectangle {
        y: plot.height / 2
        width: plot.width
        height: plot.style.hairline
        color: plot.style.lineStrong
    }
    Canvas {
        id: canvas
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const values = plot.curve
            if (values.length < 2)
                return
            const step = width / (values.length - 1)
            const middle = height / 2
            ctx.beginPath()
            ctx.moveTo(0, middle)
            for (let i = 0; i < values.length; ++i)
                ctx.lineTo(i * step, plot.yOf(Number(values[i])))
            ctx.lineTo(width, middle)
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(plot.color.r, plot.color.g, plot.color.b,
                                    plot.active ? plot.style.curveFillAlpha : plot.style.curveFillAlpha / 2)
            ctx.fill()
            ctx.beginPath()
            for (let i = 0; i < values.length; ++i) {
                if (i === 0)
                    ctx.moveTo(0, plot.yOf(Number(values[0])))
                else
                    ctx.lineTo(i * step, plot.yOf(Number(values[i])))
            }
            ctx.lineWidth = plot.style.curveStroke
            ctx.strokeStyle = plot.active ? plot.color : plot.style.textFaint
            ctx.stroke()
        }
    }
    Repeater {
        model: plot.bands.length
        delegate: Rectangle {
            required property int index
            readonly property real hz: Number(plot.bands[index])
            width: 2 * plot.style.bandDotRadius
            height: width
            radius: width / 2
            x: plot.xOf(hz) - width / 2
            y: plot.yOf(plot.dbAt(hz)) - height / 2
            color: plot.style.panel
            border.color: plot.active ? plot.color : plot.style.textFaint
            border.width: plot.style.strokeBold
        }
    }
}
