pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit
import "../shell" as Shell
import "AnvilMath.js" as AnvilMath

// Anvil's curve editor (ADR-0060): one of the body's curves (pitch, amp or
// drive) drawn over the kick it makes, rendered by the engine itself, with
// the other main curve as a faint ghost and a white-hot playhead while it
// plays. Pitch reads on a logarithmic axis with the note under the pointer.
//
// Drag a point; drag the small ring between two points to bend that
// segment; double-click empty space to add a point, double-click or
// right-click a point to remove it (the ends stay). Each drag is one
// document gesture (one undo step); Escape mid-drag cancels it.
Kit.CurveEditor {
    id: editor
    required property var panel
    readonly property bool pitch: curveId === "pitch"
    // The ghost: amp under pitch, pitch under amp and drive.
    readonly property string ghostId: pitch ? "amp" : "pitch"
    readonly property var ghost: doc.curves_revision >= 0 ? doc.curve(ghostId) : []
    // Peaks (0..1) over the kick, its length, and the playhead (0..1, -1 idle).
    property var kick: []
    property real kickSeconds: 0
    property real phase: -1
    // The pointer's reading, "" when outside.
    property string hoverText: ""

    // The plot's insets: room for the axis labels left and the time
    // labels below.
    readonly property real plotLeftInset: 44
    readonly property real plotRightInset: 12
    readonly property real plotTopInset: 14
    readonly property real plotBottomInset: 22
    // The axis labels: their left edge, their lift above a row's line and
    // their gap right of a time line; the time labels' rise from the bottom.
    readonly property real axisLabelX: 6
    readonly property real axisLabelDrop: 3
    readonly property real timeLabelGap: 3
    readonly property real timeLabelRise: 7
    // The plate behind each label, so the kick drawn across the plot never
    // runs through it: its padding around the text and its opacity.
    readonly property real labelPlatePad: 2
    readonly property real labelPlateAlpha: 0.85
    // The rows: pitches on the log axis, levels on the linear one.
    readonly property var pitchRowsHz: [30, 50, 100, 200, 500, 1000]
    readonly property var levelRows: [0, 0.25, 0.5, 0.75, 1]
    // Time lines every 200 ms over a kick longer than 900 ms, every 100 ms
    // over one longer than 450 ms, else every 50 ms.
    readonly property real longKickMs: 900
    readonly property real mediumKickMs: 450
    readonly property real longStepMs: 200
    readonly property real mediumStepMs: 100
    readonly property real shortStepMs: 50
    // The kick's peaks reach this fraction of the plot's height either side.
    readonly property real kickReach: 0.46

    objectName: "anvilCurve"
    theme: panel.theme
    doc: panel.doc
    curveId: "pitch"
    editable: panel.controller.editable
    plotLeft: plotLeftInset
    plotRight: width - plotRightInset
    plotTop: plotTopInset
    plotBottom: height - plotBottomInset
    bendRings: true
    doubleClickAdds: true
    doubleClickRemoves: true
    // Each end keeps its own level: a kick starts high and ends low.
    linkEnds: false
    hoverReadout: true
    areaName: "anvilCurveArea"

    function valueText(v) {
        if (pitch) {
            const hz = AnvilMath.hzOfY(v)
            return AnvilMath.hzText(hz) + " · " + AnvilMath.noteOf(hz).name
        }
        return Format.fraction(v)
    }
    function timeText(t) {
        return kickSeconds > 0 ? Format.seconds(t * kickSeconds) : Format.fraction(t)
    }

    // The grid, the axis and the kick behind; repainted on a new render.
    onPaintBack: ctx => {
        const t = editor.theme
        const L = editor.plotLeft, R = editor.plotRight, T = editor.plotTop, B = editor.plotBottom
        ctx.fillStyle = t.inkFaint
        ctx.font = t.typeCaption + "px '" + t.mono + "'"
        const rows = editor.pitch
            ? editor.pitchRowsHz.map(hz => [AnvilMath.yOfHz(hz), AnvilMath.hzText(hz)])
            : editor.levelRows.map(v => [v, Format.fraction(v)])
        // Labels are drawn last, over the kick, each on its plate.
        const labels = []
        for (const row of rows) {
            const y = Math.round(editor.py(row[0]))
            ctx.fillStyle = t.grid
            ctx.fillRect(L, y, R - L, t.hairline)
            labels.push([row[1], editor.axisLabelX, y + editor.axisLabelDrop])
        }
        if (editor.kickSeconds > 0) {
            const ms = editor.kickSeconds * 1000
            const step = ms > editor.longKickMs ? editor.longStepMs
                : ms > editor.mediumKickMs ? editor.mediumStepMs : editor.shortStepMs
            for (let at = step; at < ms; at += step) {
                const x = Math.round(editor.px(at / ms))
                ctx.fillStyle = t.grid
                ctx.fillRect(x, T, t.hairline, B - T)
                labels.push([Format.milliseconds(at), x + editor.timeLabelGap, editor.height - editor.timeLabelRise])
            }
        }
        // The kick as peaks, symmetric about the middle.
        const list = editor.kick
        if (list.length > 0) {
            const mid = (T + B) / 2
            const amp = (B - T) * editor.kickReach
            const columns = Math.max(1, Math.round(R - L))
            ctx.beginPath()
            ctx.moveTo(L, mid)
            for (let c = 0; c <= columns; ++c) {
                const i = Math.min(list.length - 1, Math.floor(c / columns * list.length))
                ctx.lineTo(L + c, mid - list[i] * amp)
            }
            for (let c = columns; c >= 0; --c) {
                const i = Math.min(list.length - 1, Math.floor(c / columns * list.length))
                ctx.lineTo(L + c, mid + list[i] * amp)
            }
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(t.wave.r, t.wave.g, t.wave.b, t.waveFillAlpha)
            ctx.fill()
            ctx.strokeStyle = Qt.rgba(t.wave.r, t.wave.g, t.wave.b, t.waveEdgeAlpha)
            ctx.lineWidth = t.waveEdgeWidth
            ctx.stroke()
        }
        const pad = editor.labelPlatePad
        for (const label of labels) {
            const width = ctx.measureText(label[0]).width
            ctx.fillStyle = Qt.rgba(t.well.r, t.well.g, t.well.b, editor.labelPlateAlpha)
            ctx.fillRect(label[1] - pad, label[2] - t.typeCaption, width + 2 * pad, t.typeCaption + 2 * pad)
            ctx.fillStyle = t.inkFaint
            ctx.fillText(label[0], label[1], label[2])
        }
    }
    // The ghost, the curve, its points and bend rings.
    onPaintShape: ctx => {
        const t = editor.theme
        if (editor.ghost.length >= 2) {
            // The ghost of pitch is drawn on its own (log) axis, which is
            // what its stored 0..1 already is.
            editor.traceCurve(ctx, editor.sample(editor.ghost, t.ghostSamples))
            ctx.strokeStyle = t.ghost
            ctx.lineWidth = t.ghostWidth
            ctx.setLineDash(t.ghostDash)
            ctx.stroke()
            ctx.setLineDash([])
        }
        const pts = editor.points
        if (pts.length < 2)
            return
        editor.traceCurve(ctx, editor.sample(pts, t.curveSamples))
        editor.strokeCurve(ctx, [])
        editor.paintRings(ctx)
        editor.paintPoints(ctx, i => i === editor.dragIndex, (i, x, y) => {})
    }
    onGhostChanged: editor.repaintShape()
    onCurveIdChanged: editor.repaintBack()
    onKickChanged: editor.repaintBack()
    onKickSecondsChanged: editor.repaintBack()
    onPointerMoved: (x, y) => {
        editor.hoverText = editor.valueText(editor.ty(y)) + " at " + editor.timeText(editor.tx(x))
    }
    onPointerLeft: editor.hoverText = ""
    onRingBent: (index, fraction) => {
        editor.panel.touch("BEND", Format.fraction(fraction) + " at the midpoint")
    }
    onPointMoved: index => {
        const point = editor.points[index]
        if (point !== undefined)
            editor.panel.touch(editor.curveId.toUpperCase(),
                               editor.valueText(point.y) + " at " + editor.timeText(point.x))
    }

    Rectangle {
        objectName: "anvilPlayhead"
        visible: editor.phase >= 0 && editor.phase <= 1
        x: editor.px(Math.max(0, editor.phase)) - width / 2
        y: editor.plotTop
        width: editor.theme.curvePlayheadWidth
        height: editor.plotBottom - editor.plotTop
        color: editor.theme.time
        opacity: editor.theme.playheadOpacity
    }

    Shell.Tip {
        text: "Drag a point; drag a ring to bend a segment; double-click to add or remove a point"
    }
}
