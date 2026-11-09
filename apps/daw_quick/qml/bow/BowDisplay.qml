pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The bow display: the wheel's dynamics over the last few seconds as an
// amber curve over a soft fill, the newest note's vibrato depth as a teal
// line under it, and a dot where the bow is now, at the right edge, over
// the last `seconds`. While nothing plays it previews the wheel's setting
// as a flat line. `history` is a list of {dynamics, vibrato}, oldest first,
// each 0..1.
Kit.PlotFrame {
    id: display
    property var history: []
    property real wheel: 0
    // How many seconds the history spans, for the time axis.
    property real seconds: 6
    // The vibrato line's band at the bottom, as a share of the height.
    readonly property real vibratoBase: 0.08
    readonly property real vibratoSpan: 0.28
    readonly property real dynamicsWidth: 2.5
    readonly property real vibratoWidth: 1.5
    readonly property real dotRadius: 5
    readonly property real dotEdge: 2
    readonly property var shown: live && history.length > 1 ? history
        : [{ dynamics: wheel, vibrato: 0 }, { dynamics: wheel, vibrato: 0 }]
    readonly property real lastDynamics: shown[shown.length - 1].dynamics

    objectName: "bowDisplay"
    grid: true
    xTicks: [{ at: 0, label: "-" + display.seconds }, { at: 0.5, label: "-" + display.seconds / 2 },
             { at: 1, label: "now" }]
    xUnit: "s"
    Accessible.role: Accessible.Graphic
    Accessible.name: "Bow dynamics and vibrato"
    Accessible.description: display.live ? "Playing" : "Preview of the mod wheel's setting"
    onShownChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        width: display.plotArea.width
        height: display.plotArea.height
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const points = display.shown
            const step = width / (points.length - 1)
            const yOf = d => height - Math.max(0, Math.min(1, d)) * height
            // The fill under the dynamics, then its line.
            ctx.beginPath()
            ctx.moveTo(0, height)
            for (let i = 0; i < points.length; ++i)
                ctx.lineTo(i * step, yOf(points[i].dynamics))
            ctx.lineTo(width, height)
            ctx.closePath()
            ctx.fillStyle = display.theme.accentSoft
            ctx.fill()
            ctx.beginPath()
            for (let i = 0; i < points.length; ++i) {
                const y = yOf(points[i].dynamics)
                if (i === 0)
                    ctx.moveTo(0, y)
                else
                    ctx.lineTo(i * step, y)
            }
            ctx.strokeStyle = display.theme.accent
            ctx.lineWidth = display.dynamicsWidth
            ctx.stroke()
            // Vibrato depth, in its band at the bottom.
            ctx.beginPath()
            for (let i = 0; i < points.length; ++i) {
                const v = display.vibratoBase + display.vibratoSpan * Math.max(0, Math.min(1, points[i].vibrato))
                const y = height - v * height
                if (i === 0)
                    ctx.moveTo(0, y)
                else
                    ctx.lineTo(i * step, y)
            }
            ctx.strokeStyle = display.theme.vibratoInk
            ctx.lineWidth = display.vibratoWidth
            ctx.stroke()
        }
    }
    // The dynamics' ends, inside the plot as the mockup draws them.
    Text {
        x: display.dotRadius
        y: display.dotRadius
        text: "ff"
        color: display.theme.inkDim
        font.family: display.theme.mono
        font.pixelSize: display.theme.smallSize
    }
    Text {
        x: display.dotRadius
        y: canvas.height - height - display.dotRadius
        text: "pp"
        color: display.theme.inkDim
        font.family: display.theme.mono
        font.pixelSize: display.theme.smallSize
    }
    // Where the bow is now.
    Rectangle {
        objectName: "bowPlayhead"
        x: canvas.width - width
        y: canvas.height - Math.max(0, Math.min(1, display.lastDynamics)) * canvas.height - height / 2
        width: 2 * display.dotRadius
        height: width
        radius: width / 2
        color: display.theme.ink
        border.color: display.theme.accent
        border.width: display.dotEdge
    }
}
