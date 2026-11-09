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

    // The plot's insets: room for a point's dot at each edge (the frame
    // around the editor draws the scales).
    readonly property real plotLeftInset: 12
    readonly property real plotRightInset: 12
    readonly property real plotTopInset: 14
    readonly property real plotBottomInset: 14
    // The rows: pitches on the log axis as [Hz, label], levels on the
    // linear one.
    readonly property var pitchRows: [[30, "30"], [50, "50"], [100, "100"], [200, "200"], [500, "500"], [1000, "1k"]]
    readonly property var levelRows: [0, 0.25, 0.5, 0.75, 1]
    readonly property real percentPerUnit: 100
    readonly property real msPerSecond: 1000
    readonly property real kickMs: kickSeconds * msPerSecond
    // Time lines every 200 ms over a kick longer than 900 ms, every 100 ms
    // over one longer than 450 ms, else every 50 ms.
    readonly property real longKickMs: 900
    readonly property real mediumKickMs: 450
    readonly property real longStepMs: 200
    readonly property real mediumStepMs: 100
    readonly property real shortStepMs: 50
    // The kick's peaks reach this fraction of the plot's height either side.
    readonly property real kickReach: 0.46

    // The rows as stored values with their labels, the time lines in ms,
    // and both as the frame's ticks, from the editor's own mapping.
    readonly property var rows: pitch
        ? pitchRows.map(row => [AnvilMath.yOfHz(row[0]), row[1]])
        : levelRows.map(v => [v, Format.number(v * percentPerUnit, 0)])
    readonly property var timeLinesMs: {
        const ms = kickMs
        if (!(ms > 0))
            return []
        const step = ms > longKickMs ? longStepMs : ms > mediumKickMs ? mediumStepMs : shortStepMs
        const lines = []
        for (let at = step; at < ms; at += step)
            lines.push(at)
        return lines
    }
    readonly property string yUnit: pitch ? "Hz" : "%"
    readonly property var yTicks: height > 0
        ? rows.map(row => ({ at: 1 - py(row[0]) / height, label: row[1] })) : []
    readonly property var xTicks: width > 0 && kickSeconds > 0
        ? [{ at: px(0) / width, label: "0" }].concat(timeLinesMs.map(at => ({
            at: px(at / kickMs) / width, label: Format.number(at, 0) }))) : []

    // Keys: Left/Right pick a point, Up/Down move it (Shift: coarse),
    // Ctrl+Left/Right move it in time, Page Up/Down bend the segment after
    // it, Delete removes it (not the ends). Each key is one undo step.
    property int keyIndex: 0
    // The key point, kept on the curve as points come and go.
    readonly property int keyPoint: Math.max(0, Math.min(points.length - 1, keyIndex))
    readonly property real keyStep: 0.01
    readonly property real keyCoarseStep: 0.1
    readonly property real keyBendStep: 0.1

    function keyEdit(change) {
        if (!editable || points.length === 0)
            return
        const index = keyPoint
        doc.begin_edit()
        change(index, points[index])
        doc.commit_edit()
        pointMoved(Math.min(index, points.length - 1))
    }

    objectName: "anvilCurve"
    activeFocusOnTab: true
    border.color: activeFocus ? theme.accent : theme.curveEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Body " + curveId + " curve"
    Accessible.description: "Left and Right pick a point, Up and Down move it, Ctrl with Left and Right moves it in time, "
        + "Page Up and Page Down bend the segment after it, Delete removes it"
    onKeyPointChanged: repaintShape()
    onActiveFocusChanged: repaintShape()
    Keys.onPressed: event => {
        const step = (event.modifiers & Qt.ShiftModifier) !== 0 ? keyCoarseStep : keyStep
        const inTime = (event.modifiers & Qt.ControlModifier) !== 0
        const sideways = event.key === Qt.Key_Left || event.key === Qt.Key_Right
        const direction = event.key === Qt.Key_Right || event.key === Qt.Key_Up || event.key === Qt.Key_PageUp ? 1 : -1
        if (sideways && !inTime) {
            keyIndex = Math.max(0, Math.min(points.length - 1, keyPoint + direction))
        } else if (sideways || event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            keyEdit((index, point) => doc.move_curve_point(curveId, index,
                sideways ? Math.max(0, Math.min(1, point.x + direction * step)) : point.x,
                sideways ? point.y : Math.max(0, Math.min(1, point.y + direction * step)), linkEnds))
        } else if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
            if (keyPoint + 1 >= points.length)
                return
            keyEdit((index, point) => doc.set_curve_tension(curveId, index,
                Math.max(-maxTension, Math.min(maxTension, point.tension + direction * keyBendStep))))
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            if (!editable || !removable(keyPoint) || keyPoint + 1 >= points.length)
                return
            const removed = keyPoint
            doc.remove_curve_point(curveId, removed)
            pointRemoved(removed)
        } else {
            return
        }
        event.accepted = true
    }

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
        for (const row of editor.rows) {
            ctx.fillStyle = t.grid
            ctx.fillRect(L, Math.round(editor.py(row[0])), R - L, t.hairline)
        }
        for (const at of editor.timeLinesMs) {
            ctx.fillStyle = t.grid
            ctx.fillRect(Math.round(editor.px(at / editor.kickMs)), T, t.hairline, B - T)
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
        editor.paintPoints(ctx, i => i === editor.dragIndex || (editor.activeFocus && i === editor.keyPoint),
                           (i, x, y) => {})
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
