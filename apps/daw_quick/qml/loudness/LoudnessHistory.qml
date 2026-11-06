pragma ComponentBehavior: Bound
import QtQuick

// The loudness history: short-term as a filled teal trace and momentary as
// a fine pale line over the last `seconds` (10 values a second, newest on
// the right), with the target as a line in a +-1 LU tolerance band. The
// vertical range follows the meter's EBU scale around the target. Values
// at or below -199 are gaps.
Item {
    id: history
    required property LoudnessTheme theme
    property var shortTerm: []
    property var momentary: []
    property real target: -14
    property real scaleTop: 9
    property real seconds: 60
    readonly property real rate: 10

    readonly property real scaleBottom: -2 * scaleTop
    readonly property real px: theme.historyScaleWidth
    readonly property real py: theme.historyTop
    readonly property real pw: width - px - theme.historyRight
    readonly property real ph: height - py - theme.historyBottom
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

    Rectangle {
        x: history.px
        y: history.py
        width: history.pw
        height: history.ph
        color: history.theme.well
        border.color: history.theme.wellEdge
        radius: history.theme.radiusSm
    }

    // Absolute LUFS labels every 3 (or 6) LU around the target.
    Repeater {
        model: Math.round((history.scaleTop - history.scaleBottom) / history.gridStep) + 1
        delegate: Text {
            id: label
            required property int index
            readonly property real lu: history.scaleTop - label.index * history.gridStep
            x: 0
            width: history.px - history.theme.spaceSm
            y: history.yOf(history.target + label.lu) - history.theme.scaleLabelLift
            horizontalAlignment: Text.AlignRight
            text: Math.round(history.target + label.lu)
            color: label.lu === 0 ? history.theme.accent : history.theme.inkFaint
            font.family: history.theme.mono
            font.pixelSize: history.theme.typeCaption
        }
    }
    // Time labels.
    Repeater {
        model: history.timeLabels
        delegate: Text {
            id: when
            required property int index
            readonly property int last: history.timeLabels - 1
            readonly property real age: history.seconds * (when.last - when.index) / when.last
            x: history.px + history.pw * when.index / when.last - (when.index === when.last ? width : when.index === 0 ? 0 : width / 2)
            y: history.py + history.ph + history.theme.timeLabelGap
            text: when.age === 0 ? "NOW" : "−" + Math.round(when.age) + " s"
            color: history.theme.inkFaint
            font.family: history.theme.mono
            font.pixelSize: history.theme.typeCaption
        }
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
