pragma ComponentBehavior: Bound
import QtQuick

// The gain-reduction history: the reduction the limiter applies (held 1 s)
// hanging from 0 dB at the top, over the last `seconds`, newest on the
// right, on a 0 to `rangeDb` scale. `samples` is [[time in seconds,
// reduction in dB], ...], oldest first; gaps longer than half a second
// (playback stopped) break the trace.
Item {
    id: history
    required property CeilingTheme theme
    property var samples: []
    property real seconds: 10
    property real rangeDb: 12

    // The dB scale's grid step and the silence that breaks the trace.
    readonly property real gridStepDb: 3
    readonly property real gapSeconds: 0.5
    // The time axis: labels at each quarter of the span.
    readonly property int timeDivisions: 4

    readonly property real px: theme.historyScaleWidth
    readonly property real py: theme.spaceMd
    readonly property real pw: width - px - theme.historyRightPad
    readonly property real ph: height - py - theme.historyAxisHeight
    readonly property real newest: samples.length > 0 ? samples[samples.length - 1][0] : 0

    objectName: "ceilingHistory"

    function yOf(db) {
        return py + ph * Math.max(0, Math.min(1, db / rangeDb))
    }
    function xOf(time) {
        return px + pw * (1 - (newest - time) / seconds)
    }

    onSamplesChanged: canvas.requestPaint()
    onRangeDbChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Rectangle {
        x: history.px
        y: history.py
        width: history.pw
        height: history.ph
        radius: history.theme.radiusSm
        color: history.theme.screen
        border.color: history.theme.wellEdge
    }
    Repeater {
        model: Math.floor(history.rangeDb / history.gridStepDb) + 1
        delegate: Text {
            id: label
            required property int index
            readonly property real db: index * history.gridStepDb
            x: 0
            width: history.px - history.theme.spaceSm
            y: history.yOf(db) - history.theme.historyLabelLift
            horizontalAlignment: Text.AlignRight
            text: db === 0 ? "0" : "−" + db
            color: db === 0 ? history.theme.inkDim : history.theme.inkFaint
            font.family: history.theme.mono
            font.pixelSize: history.theme.typeCaption
        }
    }
    Repeater {
        model: history.timeDivisions + 1
        delegate: Text {
            id: when
            required property int index
            readonly property real age: history.seconds * (history.timeDivisions - index) / history.timeDivisions
            x: history.px + history.pw * index / history.timeDivisions
                - (index === history.timeDivisions ? width : index === 0 ? 0 : width / 2)
            y: history.py + history.ph + history.theme.historyTimeGap
            text: age === 0 ? "NOW" : "−" + Math.round(age * 10) / 10 + " s"
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
            const top = history.py
            ctx.save()
            ctx.beginPath()
            ctx.rect(left + t.hairline, top + t.hairline, history.pw - 2 * t.hairline, history.ph - 2 * t.hairline)
            ctx.clip()
            // Grid every gridStepDb.
            ctx.strokeStyle = t.grid
            ctx.lineWidth = t.hairline
            for (let db = history.gridStepDb; db <= history.rangeDb; db += history.gridStepDb) {
                const y = Math.round(history.yOf(db)) + t.hairline / 2
                ctx.beginPath()
                ctx.moveTo(left, y)
                ctx.lineTo(left + history.pw, y)
                ctx.stroke()
            }
            // The reduction, in runs between gaps, filled down from 0 dB.
            const s = history.samples
            const fill = ctx.createLinearGradient(0, top, 0, top + history.ph)
            const r = t.reduction
            const deep = t.reductionDeep
            fill.addColorStop(0, Qt.rgba(r.r, r.g, r.b, t.reductionFillTopAlpha))
            fill.addColorStop(1, Qt.rgba(deep.r, deep.g, deep.b, t.reductionFillBottomAlpha))
            let run = []
            const flush = () => {
                if (run.length > 1) {
                    ctx.beginPath()
                    ctx.moveTo(history.xOf(run[0][0]), top)
                    for (const p of run)
                        ctx.lineTo(history.xOf(p[0]), history.yOf(p[1]))
                    ctx.lineTo(history.xOf(run[run.length - 1][0]), top)
                    ctx.closePath()
                    ctx.fillStyle = fill
                    ctx.fill()
                    ctx.beginPath()
                    for (let i = 0; i < run.length; ++i) {
                        const x = history.xOf(run[i][0])
                        const y = history.yOf(run[i][1])
                        if (i === 0)
                            ctx.moveTo(x, y)
                        else
                            ctx.lineTo(x, y)
                    }
                    ctx.strokeStyle = r
                    ctx.lineWidth = t.reductionStroke
                    ctx.stroke()
                }
                run = []
            }
            for (let i = 0; i < s.length; ++i) {
                if (history.newest - s[i][0] > history.seconds + history.gapSeconds)
                    continue
                if (run.length > 0 && s[i][0] - run[run.length - 1][0] > history.gapSeconds)
                    flush()
                run.push(s[i])
            }
            flush()
            ctx.restore()
        }
    }
}
