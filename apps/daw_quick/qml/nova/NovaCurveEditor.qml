pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract

// The drawable LFO shape (NOVA-MOD-02): points joined by bent or stepped
// segments, snapped to the LFO's grid (Shift is free). Press empty space to
// add a point and drag it; drag a point to move it; Alt-drag a segment to
// bend it; double-click a point to toggle its segment between curve and
// step; right-click a point to remove it. Each drag is one document gesture
// (begin, updates, commit). An amber playhead follows the LFO's phase.
// From the keyboard: Left and Right pick a point, Up and Down move it
// (Shift: coarse), Ctrl with Left and Right moves it one grid step in
// time, Page Up and Page Down bend the segment after it, Return toggles
// its step, Insert adds a point halfway to the next and Delete removes it
// (not the ends); each key is one undo step. Its holder frames it
// (Kit.PlotFrame) with `xTicks` and `yTicks`: percent of a cycle across,
// percent of the LFO's range up.
Kit.CurveEditor {
    id: editor
    required property var panel
    required property string lfo
    readonly property int number: parseInt(lfo.substring(4))
    readonly property string base: "nova." + lfo
    readonly property int grid: panel.controller.revision >= 0 ? Math.max(1, Math.round(panel.controller.value(base + ".grid"))) : 8
    readonly property real phase: panel.at(Contract.T.lfoPhase + number - 1)
    readonly property real pad: panel.theme.lfoEditorPad
    // The room above the plot for the caption.
    readonly property real captionRoom: panel.theme.lfoCaptionRoom
    // The step mark under a step point: its size and gap below the point.
    readonly property real stepMarkSize: 3
    readonly property real stepMarkGap: 7
    readonly property real playheadDotRadius: panel.theme.lfoPlayheadDotRadius
    readonly property real fillFloorAlpha: panel.theme.lfoFillFloorAlpha
    // The scale its plot frame shows, in percent along each axis.
    readonly property var scalePercents: [0, 50, 100]
    readonly property real percent: 100
    readonly property var xTicks: width > 0
        ? scalePercents.map(p => ({ at: px(p / percent) / width, label: String(p) })) : []
    readonly property var yTicks: height > 0
        ? scalePercents.map(p => ({ at: 1 - py(p / percent) / height, label: String(p) })) : []

    // The point the keys edit, and their steps up and down.
    property int keyIndex: 0
    readonly property real keyStep: 0.01
    readonly property real keyCoarseStep: 0.1
    readonly property real keyBendStep: 0.1
    function keyEdit(change) {
        if (!editable || points.length === 0)
            return
        const index = Math.max(0, Math.min(points.length - 1, keyIndex))
        doc.begin_edit()
        change(index, points[index])
        doc.commit_edit()
        pointMoved(Math.min(index, points.length - 1))
        panel.touchId(base + ".rate", "LFO " + number + " SHAPE")
    }

    objectName: "novaCurveEditor:" + lfo
    theme: panel.theme
    doc: panel.doc
    curveId: lfo
    editable: panel.controller.editable
    plotLeft: pad
    plotRight: width - pad
    plotTop: pad + captionRoom
    plotBottom: height - pad
    snapSteps: grid
    altBends: true
    pressAddsPoint: true
    doubleClickSteps: true
    lastPointRemovable: true
    focusOnPress: false
    backLayer: false
    activeFocusOnTab: true
    border.color: activeFocus ? panel.theme.vi : theme.curveEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "LFO " + number + " shape"
    Accessible.description: "Left and Right pick a point, Up and Down move it, Ctrl with Left and Right moves it in time, "
        + "Page Up and Page Down bend the segment after it, Return toggles its step, Insert adds a point, Delete removes it"
    onPointsChanged: keyIndex = Math.max(0, Math.min(points.length - 1, keyIndex))
    onKeyIndexChanged: repaintShape()
    onActiveFocusChanged: repaintShape()
    Keys.onPressed: event => {
        const step = (event.modifiers & Qt.ShiftModifier) !== 0 ? keyCoarseStep : keyStep
        const inTime = (event.modifiers & Qt.ControlModifier) !== 0
        const sideways = event.key === Qt.Key_Left || event.key === Qt.Key_Right
        const direction = event.key === Qt.Key_Right || event.key === Qt.Key_Up || event.key === Qt.Key_PageUp ? 1 : -1
        if (sideways && !inTime) {
            keyIndex = Math.max(0, Math.min(points.length - 1, keyIndex + direction))
        } else if (sideways || event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            keyEdit((index, point) => doc.move_curve_point(curveId, index,
                sideways ? Math.max(0, Math.min(1, point.x + direction / grid)) : point.x,
                sideways ? point.y : Math.max(0, Math.min(1, point.y + direction * step)), linkEnds))
        } else if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
            if (keyIndex + 1 >= points.length)
                return
            keyEdit((index, point) => doc.set_curve_tension(curveId, index,
                Math.max(-maxTension, Math.min(maxTension, point.tension + direction * keyBendStep))))
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (keyIndex + 1 >= points.length)
                return
            keyEdit((index, point) => doc.set_curve_step(curveId, index, !point.step))
        } else if (event.key === Qt.Key_Insert) {
            if (!editable || keyIndex + 1 >= points.length)
                return
            const a = points[keyIndex], b = points[keyIndex + 1]
            doc.begin_edit()
            const added = doc.insert_curve_point(curveId, (a.x + b.x) / 2, valueAt((a.x + b.x) / 2))
            if (added < 0) {
                doc.cancel_edit()
                return
            }
            doc.commit_edit()
            keyIndex = added
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            if (!editable || !removable(keyIndex) || keyIndex + 1 >= points.length)
                return
            doc.remove_curve_point(curveId, keyIndex)
            pointRemoved(keyIndex)
        } else {
            return
        }
        event.accepted = true
    }

    onPointGrabbed: editor.panel.touchId(editor.base + ".rate", "LFO " + editor.number + " SHAPE")
    onPaintShape: ctx => {
        const t = editor.theme
        const pts = editor.points
        ctx.lineWidth = t.hairline
        for (let i = 0; i <= editor.grid; ++i) {
            ctx.strokeStyle = i === 0 || i === editor.grid ? t.gridLine2 : t.gridLine
            ctx.beginPath()
            ctx.moveTo(editor.px(i / editor.grid), editor.pad)
            ctx.lineTo(editor.px(i / editor.grid), editor.height - editor.pad)
            ctx.stroke()
        }
        ctx.strokeStyle = t.gridLine
        ctx.beginPath()
        ctx.moveTo(editor.pad, editor.py(0.5))
        ctx.lineTo(editor.width - editor.pad, editor.py(0.5))
        ctx.stroke()
        if (pts.length < 2)
            return
        // The glow is a wide faint stroke under the line: a software shadow
        // blur costs a hundred times more to paint.
        editor.traceCurve(ctx, editor.sample(pts, t.curveSamples))
        editor.strokeCurve(ctx, [])
        ctx.lineTo(editor.px(1), editor.py(0))
        ctx.lineTo(editor.px(0), editor.py(0))
        ctx.closePath()
        const fill = ctx.createLinearGradient(0, editor.pad, 0, editor.height)
        fill.addColorStop(0, t.soft(t.vi, t.glowAlpha))
        fill.addColorStop(1, t.soft(t.vi, editor.fillFloorAlpha))
        ctx.fillStyle = fill
        ctx.fill()
        editor.paintPoints(ctx, i => editor.activeFocus && i === editor.keyIndex, (i, x, y) => {
            if (pts[i].step && i + 1 < pts.length) {
                ctx.fillStyle = t.vi
                ctx.fillRect(x - editor.stepMarkSize / 2, y + editor.stepMarkGap,
                             editor.stepMarkSize, editor.stepMarkSize)
            }
        })
        if (editor.panel.live) {
            const x = editor.px(editor.phase)
            ctx.fillStyle = t.am
            ctx.fillRect(x - t.curvePlayheadWidth / 2, editor.pad, t.curvePlayheadWidth, editor.height - 2 * editor.pad)
            ctx.beginPath()
            ctx.arc(x, editor.py(editor.valueAt(editor.phase)), editor.playheadDotRadius, 0, Math.PI * 2)
            ctx.fill()
        }
    }
    onGridChanged: editor.repaintShape()
    onPhaseChanged: editor.repaintShape()

    Text {
        x: editor.panel.theme.displayCaptionX
        y: editor.panel.theme.displayCaptionY
        text: "LFO " + editor.number + " · DRAW POINTS · ALT-DRAG CURVES"
        color: editor.panel.theme.faint
        font.family: editor.panel.theme.sans
        font.pixelSize: editor.panel.theme.typeSmall
        font.weight: Font.Bold
        font.letterSpacing: editor.panel.theme.displayCaptionSpacing
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: editor.panel.theme.displayCaptionX
        y: editor.panel.theme.displayReadoutY
        text: editor.panel.controller.revision < 0 ? ""
            : editor.panel.controller.choice(editor.base + ".sync") === 0
              ? editor.panel.controller.display(editor.base + ".rate") + " · FREE RATE"
              : editor.panel.controller.display(editor.base + ".sync") + " · SYNC"
        color: editor.panel.theme.am
        font.family: editor.panel.theme.mono
        font.pixelSize: editor.panel.theme.typeSmall
    }
}
