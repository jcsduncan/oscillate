pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The loudness history: short-term as a filled teal trace and momentary as
// a fine pale line over the last `seconds` (10 values a second, newest on
// the right), with the target as a line in a +-1 LU tolerance band. The
// vertical range follows the meter's EBU scale around the target. Values
// at or below -199 are gaps. The shared plot frame labels LUFS and seconds
// and says LIVE while the meter is playing.
Item {
    id: history
    required property LoudnessTheme theme
    property var shortTerm: []
    property var momentary: []
    property real target: -14
    property real scaleTop: 9
    property real seconds: 60
    // Whether the meter is running (the frame's LIVE chip).
    property bool running: false
    readonly property real rate: 10

    readonly property real scaleBottom: -2 * scaleTop
    // The plot's own coordinates: the frame's plot area.
    readonly property real px: 0
    readonly property real py: 0
    readonly property real pw: frame.plotArea.width
    readonly property real ph: frame.plotArea.height
    // LU between grid lines and labels: 3 on EBU +9, 6 on EBU +18.
    readonly property real gridStep: scaleTop > 9 ? 6 : 3
    // The target's tolerance either side, in LU.
    readonly property real toleranceLu: 1
    // Values at or below this are gaps.
    readonly property real silent: -199
    // Samples this far left of the plot are skipped.
    readonly property real cullMargin: 2
    // Time labels along the bottom, oldest to now.
    readonly property int timeLabels: 5
    // Absolute LUFS every 3 (or 6) LU around the target, bottom up.
    readonly property var yTicks: {
        const ticks = []
        const span = scaleTop - scaleBottom
        const count = Math.round(span / gridStep)
        for (let i = 0; i <= count; ++i) {
            const lu = scaleBottom + i * gridStep
            ticks.push({ at: (lu - scaleBottom) / span, label: String(Math.round(target + lu)) })
        }
        return ticks
    }
    // Seconds ago, oldest to now.
    readonly property var xTicks: {
        const ticks = []
        const last = timeLabels - 1
        for (let i = 0; i <= last; ++i) {
            const age = seconds * (last - i) / last
            ticks.push({ at: i / last, label: age === 0 ? "NOW" : String(Math.round(-age)) })
        }
        return ticks
    }

    objectName: "loudnessHistory"

    function yOf(lufs) {
        const lu = Math.max(scaleBottom, Math.min(scaleTop, lufs - target))
        return py + ph * (1 - (lu - scaleBottom) / (scaleTop - scaleBottom))
    }
    // x of sample i of n, the newest at the right edge.
    function xOf(i, n) {
        const age = (n - 1 - i) / rate
        return px + pw * (1 - age / seconds)
    }

    onShortTermChanged: canvas.requestPaint()
    onMomentaryChanged: canvas.requestPaint()
    onTargetChanged: canvas.requestPaint()
    onScaleTopChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Kit.PlotFrame {
        id: frame
        anchors.fill: parent
        theme: history.theme
        live: history.running
        xTicks: history.xTicks
        xUnit: "s"
        yTicks: history.yTicks
        yUnit: "LUFS"

        Rectangle {
            width: history.pw
            height: history.ph
            color: history.theme.well
            border.color: history.theme.wellEdge
            radius: history.theme.radiusSm
        }

        Canvas {
            id: canvas
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const t = history.theme
                const left = history.px
                const right = history.px + history.pw
                ctx.save()
                ctx.beginPath()
                ctx.rect(left + t.hairline, history.py + t.hairline, history.pw - 2 * t.hairline, history.ph - 2 * t.hairline)
                ctx.clip()
                // Grid every 3 LU (6 on EBU +18), on the pixel centres.
                ctx.strokeStyle = t.grid
                ctx.lineWidth = t.hairline
                for (let lu = history.scaleBottom; lu <= history.scaleTop; lu += history.gridStep) {
                    const y = Math.round(history.yOf(history.target + lu)) + t.hairline / 2
                    ctx.beginPath()
                    ctx.moveTo(left, y)
                    ctx.lineTo(right, y)
                    ctx.stroke()
                }
                // The tolerance band and the target line.
                const bandTop = history.yOf(history.target + history.toleranceLu)
                const bandBottom = history.yOf(history.target - history.toleranceLu)
                ctx.fillStyle = t.toleranceFill
                ctx.fillRect(left, bandTop, history.pw, bandBottom - bandTop)
                ctx.strokeStyle = t.targetLine
                ctx.setLineDash(t.targetDash)
                ctx.beginPath()
                ctx.moveTo(left, history.yOf(history.target))
                ctx.lineTo(right, history.yOf(history.target))
                ctx.stroke()
                ctx.setLineDash([])

                const trace = (values, filled, color, width) => {
                    const n = values.length
                    let open = false
                    let first = 0
                    let last = 0
                    ctx.beginPath()
                    for (let i = 0; i < n; ++i) {
                        const x = history.xOf(i, n)
                        if (x < left - history.cullMargin) continue
                        if (values[i] <= history.silent) {
                            if (open && filled) {
                                ctx.lineTo(last, history.py + history.ph)
                                ctx.lineTo(first, history.py + history.ph)
                                ctx.closePath()
                            }
                            open = false
                            continue
                        }
                        const y = history.yOf(values[i])
                        if (!open) {
                            ctx.moveTo(x, y)
                            first = x
                            open = true
                        } else {
                            ctx.lineTo(x, y)
                        }
                        last = x
                    }
                    if (open && filled) {
                        ctx.lineTo(last, history.py + history.ph)
                        ctx.lineTo(first, history.py + history.ph)
                        ctx.closePath()
                    }
                    if (filled) {
                        const fill = ctx.createLinearGradient(0, history.py, 0, history.py + history.ph)
                        fill.addColorStop(0, Qt.rgba(color.r, color.g, color.b, t.traceFillTopAlpha))
                        fill.addColorStop(1, Qt.rgba(color.r, color.g, color.b, t.traceFillBottomAlpha))
                        ctx.fillStyle = fill
                        ctx.fill()
                    } else {
                        ctx.strokeStyle = color
                        ctx.lineWidth = width
                        ctx.stroke()
                    }
                }
                trace(history.shortTerm, true, t.shortTerm, 0)
                trace(history.momentary, false, t.momentaryLine, t.hairline)
                // The short-term edge, crisp over its fill.
                const edge = (values, color) => {
                    const n = values.length
                    let open = false
                    ctx.beginPath()
                    for (let i = 0; i < n; ++i) {
                        const x = history.xOf(i, n)
                        if (x < left - history.cullMargin || values[i] <= history.silent) {
                            open = false
                            continue
                        }
                        const y = history.yOf(values[i])
                        if (!open) {
                            ctx.moveTo(x, y)
                            open = true
                        } else {
                            ctx.lineTo(x, y)
                        }
                    }
                    ctx.strokeStyle = color
                    ctx.lineWidth = t.traceEdgeWidth
                    ctx.stroke()
                }
                edge(history.shortTerm, t.shortTerm)
                ctx.restore()
            }
        }
    }
}
