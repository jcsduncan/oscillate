pragma ComponentBehavior: Bound
import QtQuick

// The mix's tonal balance against the reference, a third-octave band each:
// the reference's middle half as a corridor about its median (the zero
// line), the mix's centred difference as the line. `curve` holds {hz, mix,
// low, high} in dB; `rangeDb` is the view's reach either side.
Item {
    id: plot
    required property AssistantTheme style
    property var curve: []
    property real rangeDb: 12
    readonly property real lowHz: 20
    readonly property real highHz: 20000

    function xOf(hz) {
        return style.tonalLabelWidth + (width - style.tonalLabelWidth) * Math.log(hz / lowHz) / Math.log(highHz / lowHz)
    }
    function yOf(db) {
        const clamped = Math.max(-rangeDb, Math.min(rangeDb, db))
        return height / 2 - clamped / rangeDb * (height / 2 - style.tonalStroke)
    }

    onCurveChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    Accessible.role: Accessible.Graphic
    Accessible.name: "Tonal balance against the reference"

    Rectangle {
        x: plot.style.tonalLabelWidth
        width: parent.width - plot.style.tonalLabelWidth
        height: parent.height
        radius: plot.style.radiusMd
        color: plot.style.well
        border.color: plot.style.wellEdge
    }
    Repeater {
        model: plot.style.tonalGridHz.length
        delegate: Item {
            required property int index
            x: plot.xOf(plot.style.tonalGridHz[index])
            height: plot.height
            Rectangle {
                width: plot.style.hairline
                height: parent.height
                color: plot.style.line
            }
            Text {
                x: plot.style.spaceXs
                y: parent.height - height - plot.style.spaceXs
                text: plot.style.tonalGridLabels[parent.index]
                color: plot.style.textFaint
                font.pixelSize: plot.style.typeMicro
            }
        }
    }
    Repeater {
        model: [plot.rangeDb / 2, 0, -plot.rangeDb / 2]
        delegate: Text {
            required property var modelData
            width: plot.style.tonalLabelWidth - plot.style.spaceXs
            y: plot.yOf(modelData) - height / 2
            horizontalAlignment: Text.AlignRight
            text: (modelData > 0 ? "+" : "") + modelData
            color: plot.style.textFaint
            font.pixelSize: plot.style.typeMicro
        }
    }
    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const points = plot.curve
            if (points.length < 2)
                return
            // The reference's middle half.
            ctx.beginPath()
            for (let i = 0; i < points.length; ++i) {
                const p = points[i]
                const x = plot.xOf(p.hz), y = plot.yOf(p.high)
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
            }
            for (let i = points.length - 1; i >= 0; --i)
                ctx.lineTo(plot.xOf(points[i].hz), plot.yOf(points[i].low))
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(plot.style.amber.r, plot.style.amber.g, plot.style.amber.b,
                                    plot.style.corridorAlpha)
            ctx.fill()
            // The reference's median.
            ctx.strokeStyle = plot.style.amber
            ctx.lineWidth = plot.style.hairline
            ctx.beginPath()
            ctx.moveTo(plot.xOf(points[0].hz), plot.yOf(0))
            ctx.lineTo(plot.xOf(points[points.length - 1].hz), plot.yOf(0))
            ctx.stroke()
            // The mix.
            ctx.strokeStyle = plot.style.accent
            ctx.lineWidth = plot.style.tonalStroke
            ctx.beginPath()
            for (let i = 0; i < points.length; ++i) {
                const x = plot.xOf(points[i].hz), y = plot.yOf(points[i].mix)
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
            }
            ctx.stroke()
        }
    }
}
