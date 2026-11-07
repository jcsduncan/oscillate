pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit
import "PumpShapes.js" as Shapes

// Pump's shape editor: one cycle of volume drawn over the kick it answers
// to. Behind, the kick (the key's last whole cycle, or a typical kick before
// anything plays) in slate, or the input and output of the last cycle (the
// before and after); over it the drawn volume in coral after depth, its
// ducked region filled, and the volume as actually heard (anti-click and
// mix applied, from the engine) as a paler line; an amber playhead while it
// plays. It edits one curve, `curveId`: a slot's shape or one band of it.
//
// Tools: Select drags a point (snapped to the grid, Shift is free),
// rubber-bands several and drags them together, double-click adds a point,
// right-click removes one, Alt-drag bends a segment; Pencil paints grid
// steps at the pointer's level; Line draws a straight ramp; Stamp squeezes
// the chosen library shape into the grid cells dragged across. Each drag is
// one document gesture (begin, updates, commit); Escape mid-drag cancels it.
Kit.CurveEditor {
    id: editor
    required property var panel
    // The curve edited, and whether the document lacks it (it then shows,
    // and edits start from, the curve it follows).
    property bool follows: false
    property string followsLabel: ""
    property string depthId: "pump.depth"
    readonly property real depth: panel.revision >= 0 ? panel.controller.value(depthId) : 1
    // Peaks over the cycle (0..1), the heard gains, the playhead (or -1).
    property var kick: []
    property var input: []
    property var output: []
    property var applied: []
    property real phase: -1
    // "kick": the kick with the output faint over it; "io": input and output.
    property string display: "kick"
    // Snap divisions per 4/4 bar: 8, 16, 32, or 0 for none.
    property int grid: 16
    property real cycleBeats: 1
    property real cycleSeconds: 0
    // The volume shown in the readout, dB.
    property real gainDb: 0
    // "select", "pencil", "line" or "stamp", and the shape Stamp stamps.
    property string tool: "select"
    property var stampShape: Shapes.shapes[3].points
    // Selected point indices (Select).
    property var selection: []
    // The cells Pencil has painted this drag, and the last one.
    property var painted: ({})
    property int lastCell: -1

    readonly property int steps: grid > 0 ? Math.max(1, Math.round(cycleBeats * grid / 4)) : 0
    // Cells the painting tools work in: the grid, or sixteenths, at most 32.
    readonly property int cells: Math.max(1, Math.min(32, steps > 0 ? steps : 16))
    // The audio's peaks reach this share of the plot's height.
    readonly property real peakShare: 0.42
    // The dB scale's marks.
    readonly property var dbMarks: [0, -3, -6, -12, -24]
    // The scale marks are whole decibels.
    readonly property int markDecimals: 0

    objectName: "pumpCurve"
    theme: panel.theme
    doc: panel.doc
    curveId: Shapes.curveId
    points: doc.curves_revision >= 0 && panel.defaultsRevision >= 0 ? doc.curve(curveId) : []
    emptyValue: Shapes.untouched
    editable: panel.controller.editable
    activeFocusOnTab: false
    // The plot inside the well.
    plotLeft: theme.curvePlotSide
    plotRight: width - theme.curvePlotSide
    plotTop: theme.curvePlotTop
    plotBottom: height - theme.curvePlotBottom
    snapSteps: steps
    altBends: true
    doubleClickAdds: tool === "select"
    addsOnCurve: true
    rightClickRemoves: tool === "select"
    // Each end moves alone: the duck starts low and ends high.
    linkEnds: false
    pointerShape: tool === "select" ? Qt.CrossCursor : Qt.PointingHandCursor
    areaName: "pumpCurveArea"

    function effective(g) { return 1 - depth * (1 - g) }
    // The drawn volume for a height on screen, undoing depth.
    function drawnFor(level) {
        return Math.max(0, Math.min(1, 1 - (1 - level) / Math.max(0.01, depth)))
    }
    function levelOf(value) { return effective(value) }
    function valueOf(level) { return drawnFor(level) }
    function cellAt(x) { return Math.max(0, Math.min(cells - 1, Math.floor(tx(x) * cells))) }
    function dbText(db) {
        return Format.decibels(db)
    }

    // The painting tools, the rubber band and group drags (Select).
    function pressHook(mouse, hit) {
        const gesture = editor.gesture
        if (tool === "pencil") {
            gesture.begin("pencil", mouse)
            painted = ({})
            lastCell = -1
            paintTo(mouse.x, mouse.y)
            return true
        }
        if (tool === "line") {
            gesture.begin("line", mouse)
            return true
        }
        if (tool === "stamp") {
            gesture.begin("stamp", mouse)
            stampTo(mouse.x)
            return true
        }
        if (mouse.modifiers & Qt.AltModifier)
            return false
        if (hit < 0) {
            // A rubber band selects.
            gesture.start("band", mouse)
            rubber.x = mouse.x
            rubber.y = mouse.y
            rubber.width = 0
            rubber.height = 0
            rubber.visible = true
            selection = []
            return true
        }
        if (selection.length > 1 && selection.indexOf(hit) >= 0) {
            gesture.begin("group", mouse)
            return true
        }
        selection = [hit]
        return false
    }
    function dragHook(mode, mouse) {
        const gesture = editor.gesture
        const free = (mouse.modifiers & Qt.ShiftModifier) !== 0
        if (mode === "band") {
            rubber.x = Math.min(gesture.startX, mouse.x)
            rubber.y = Math.min(gesture.startY, mouse.y)
            rubber.width = Math.abs(mouse.x - gesture.startX)
            rubber.height = Math.abs(mouse.y - gesture.startY)
            return true
        }
        if (mode === "pencil") {
            paintTo(mouse.x, mouse.y)
            return true
        }
        if (mode === "line") {
            lineTo(mouse)
            return true
        }
        if (mode === "stamp") {
            stampTo(mouse.x)
            return true
        }
        if (mode === "group") {
            const dx = snapped(tx(mouse.x), free) - snapped(tx(gesture.startX), free)
            const dy = drawnFor(ty(mouse.y)) - drawnFor(ty(gesture.startY))
            doc.set_curve(curveId, Shapes.movedGroup(gesture.startPoints, selection, dx, dy))
            panel.touch("POINTS", selection.length + " moved")
            return true
        }
        return false
    }
    function paintTo(x, y) {
        const cell = cellAt(x)
        const level = drawnFor(ty(y))
        const next = Object.assign({}, painted)
        // Fill the cells a fast drag skipped.
        const from = lastCell < 0 ? cell : lastCell
        for (let c = Math.min(from, cell); c <= Math.max(from, cell); ++c)
            next[c] = level
        painted = next
        lastCell = cell
        doc.set_curve(curveId, Shapes.painted(editor.gesture.startPoints, cells, painted))
        panel.touch("PENCIL", "Step " + (cell + 1) + " · " + Format.fraction(level))
    }
    function stampTo(x) {
        const a = cellAt(editor.gesture.startX), b = cellAt(x)
        const from = Math.min(a, b) / cells, to = (Math.max(a, b) + 1) / cells
        doc.set_curve(curveId, Shapes.stamped(editor.gesture.startPoints, stampShape, from, to))
        panel.touch("STAMP", (Math.abs(b - a) + 1) + (a === b ? " step" : " steps"))
    }
    function lineTo(mouse) {
        const gesture = editor.gesture
        const free = (mouse.modifiers & Qt.ShiftModifier) !== 0
        const x0 = snapped(tx(gesture.startX), free), y0 = drawnFor(ty(gesture.startY))
        const x1 = snapped(tx(mouse.x), free), y1 = drawnFor(ty(mouse.y))
        doc.set_curve(curveId, Shapes.lined(gesture.startPoints, x0, y0, x1, y1))
        panel.touch("LINE", Format.fraction(y0) + " → " + Format.fraction(y1))
    }

    onPointRemoved: editor.selection = []
    onTensionDragged: (index, tension) => editor.panel.touch("BEND", Format.number(tension))
    onPointMoved: index => {
        const point = editor.points[index]
        if (point !== undefined) {
            const gain = editor.effective(point.y)
            const db = 20 * Math.log10(Math.max(1e-3, gain))
            editor.panel.touch("POINT",
                (editor.cycleSeconds > 0 ? Format.seconds(point.x * editor.cycleSeconds)
                                         : Format.fraction(point.x)) + " · " + Format.decibels(db))
        }
    }
    onGestureEnded: (mode, mouse) => {
        if (mode !== "band")
            return
        rubber.visible = false
        editor.selection = Shapes.within(editor.points, editor.tx(editor.gesture.startX),
                                         editor.drawnFor(editor.ty(editor.gesture.startY)), editor.tx(mouse.x),
                                         editor.drawnFor(editor.ty(mouse.y)))
        if (editor.selection.length > 0)
            editor.panel.touch("SELECT", editor.selection.length + " points")
    }
    onGestureCancelled: rubber.visible = false

    // Three layers, each repainted only when what it draws changes: the
    // grid and the audio behind (once a cycle while playing), the shape and
    // the heard curve (on edits), and the playhead (moved, never repainted).
    onPaintBack: ctx => {
        const t = editor.theme
        const L = editor.plotLeft, R = editor.plotRight, T = editor.plotTop, B = editor.plotBottom
        // The snap grid, then the beats.
        ctx.fillStyle = t.grid
        for (let i = 1; i < editor.steps; ++i)
            ctx.fillRect(Math.round(editor.px(i / editor.steps)), T, t.hairline, B - T)
        ctx.fillStyle = t.gridStrong
        for (let b = 1; b < editor.cycleBeats - 1e-6; ++b)
            ctx.fillRect(Math.round(editor.px(b / editor.cycleBeats)), T, t.hairline, B - T)
        // Audio as peaks, symmetric about the middle.
        const mid = (T + B) / 2
        const amp = (B - T) * editor.peakShare
        const columns = Math.max(1, Math.round(R - L))
        const peaks = (list, colour, alpha, scale) => {
            if (list.length === 0)
                return
            ctx.strokeStyle = Qt.rgba(colour.r, colour.g, colour.b, alpha)
            ctx.lineWidth = t.hairline
            ctx.beginPath()
            for (let c = 0; c <= columns; ++c) {
                const i = Math.min(list.length - 1, Math.floor(c / columns * list.length))
                const peak = Math.min(1, list[i] * scale)
                ctx.moveTo(L + c, mid - peak * amp)
                ctx.lineTo(L + c, mid + peak * amp)
            }
            ctx.stroke()
        }
        // Input and output share one scale: the input's peak.
        let top = 0
        for (let i = 0; i < editor.input.length; ++i)
            top = Math.max(top, editor.input[i])
        const ioScale = top > 1e-4 ? 1 / top : 1
        if (editor.display === "io") {
            peaks(editor.input, t.kick, t.ioInputAlpha, ioScale)
            peaks(editor.output, t.heard, t.ioOutputAlpha, ioScale)
        } else {
            peaks(editor.kick, t.kick, t.kickAlpha, 1)
            peaks(editor.output, t.heard, t.kickOutputAlpha, ioScale)
        }
        // The dB scale on the right, time under the cycle.
        ctx.fillStyle = t.inkFaint
        ctx.font = t.typeCaption + "px '" + t.mono + "'"
        for (const db of editor.dbMarks)
            ctx.fillText(Format.decibels(db, editor.markDecimals), R - t.dbLabelRight, editor.py(Math.pow(10, db / 20)) - t.dbLabelLift)
        for (let b = 0; b <= Math.floor(editor.cycleBeats + 1e-6); ++b) {
            if (b < editor.cycleBeats - 1e-6 || b === 0)
                ctx.fillText(b === 0 ? "kick" : "+" + b + " beat", editor.px(b / editor.cycleBeats) + t.beatLabelGap,
                             editor.height - t.footLabelBottom)
        }
        if (editor.cycleSeconds > 0)
            ctx.fillText(Format.seconds(editor.cycleSeconds), R - t.lengthLabelRight, editor.height - t.footLabelBottom)
    }
    onPaintShape: ctx => {
        const t = editor.theme
        const pts = editor.points
        // A followed curve is drawn faint and dashed until it is edited.
        const alpha = editor.follows ? t.followAlpha : 1
        if (pts.length >= 2) {
            const values = editor.sample(pts, t.curveSamples)
            const last = values.length - 1
            // The ducked region, then the drawn volume after depth.
            ctx.globalAlpha = alpha
            ctx.beginPath()
            ctx.moveTo(editor.px(0), editor.py(1))
            for (let i = 0; i <= last; ++i)
                ctx.lineTo(editor.px(i / last), editor.py(editor.effective(values[i])))
            ctx.lineTo(editor.px(1), editor.py(1))
            ctx.closePath()
            ctx.fillStyle = t.shapeSoft
            ctx.fill()
            // The glow is a wide faint stroke under the line: a software
            // shadow blur costs a hundred times more to paint.
            editor.traceCurve(ctx, values)
            editor.strokeCurve(ctx, editor.follows ? t.followDash : [])
            ctx.globalAlpha = 1
        }
        // The volume as heard: anti-click and mix applied.
        const heard = editor.applied
        if (heard.length > 1) {
            ctx.beginPath()
            for (let i = 0; i < heard.length; ++i) {
                const x = editor.px(i / heard.length), y = editor.py(Math.max(0, Math.min(1, heard[i])))
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
            }
            ctx.strokeStyle = Qt.rgba(t.heard.r, t.heard.g, t.heard.b, t.heardAlpha)
            ctx.lineWidth = t.hairline
            ctx.setLineDash(t.heardDash)
            ctx.stroke()
            ctx.setLineDash([])
        }
        // The points; selected ones filled.
        const chosen = {}
        for (let s = 0; s < editor.selection.length; ++s)
            chosen[editor.selection[s]] = true
        ctx.globalAlpha = alpha
        editor.paintPoints(ctx, i => chosen[i] === true, (i, x, y) => {})
        ctx.globalAlpha = 1
    }
    onDepthChanged: editor.repaintShape()
    onAppliedChanged: editor.repaintShape()
    onSelectionChanged: editor.repaintShape()
    onFollowsChanged: editor.repaintShape()
    onCurveIdChanged: editor.selection = []
    onToolChanged: editor.selection = []
    onKickChanged: editor.repaintBack()
    onInputChanged: editor.repaintBack()
    onOutputChanged: editor.repaintBack()
    onDisplayChanged: editor.repaintBack()
    onStepsChanged: editor.repaintBack()
    onCycleBeatsChanged: editor.repaintBack()
    onCycleSecondsChanged: editor.repaintBack()

    // The playhead: a line and a dot riding the curve, moved not painted.
    Item {
        objectName: "pumpPlayhead"
        anchors.fill: parent
        visible: editor.phase >= 0 && editor.points.length >= 2
        Rectangle {
            x: editor.px(Math.max(0, editor.phase)) - width / 2
            y: editor.plotTop
            width: editor.theme.curvePlayheadWidth
            height: editor.plotBottom - editor.plotTop
            color: editor.theme.time
        }
        Rectangle {
            width: editor.theme.playheadDot
            height: editor.theme.playheadDot
            radius: width / 2
            color: editor.theme.time
            x: editor.px(Math.max(0, editor.phase)) - width / 2
            y: editor.py(editor.effective(editor.valueAt(Math.max(0, editor.phase)))) - height / 2
        }
    }
    // The rubber band while selecting.
    Rectangle {
        id: rubber
        objectName: "pumpRubberBand"
        visible: false
        color: Qt.rgba(editor.theme.shape.r, editor.theme.shape.g, editor.theme.shape.b, editor.theme.rubberFillAlpha)
        border.color: Qt.rgba(editor.theme.shape.r, editor.theme.shape.g, editor.theme.shape.b, editor.theme.rubberEdgeAlpha)
    }

    // The key, top left; the volume now, top right.
    Row {
        x: editor.theme.keyInset
        y: editor.theme.spaceMd
        spacing: editor.theme.keyGap
        Repeater {
            model: editor.display === "io"
                ? [["INPUT", editor.theme.kick], ["OUTPUT", editor.theme.heard], ["VOLUME", editor.theme.shape],
                   ["NOW", editor.theme.time]]
                : [[editor.panel.kickLabel, editor.theme.kick], ["VOLUME", editor.theme.shape],
                   ["AS HEARD", editor.theme.heard], ["NOW", editor.theme.time]]
            delegate: Row {
                id: key
                required property var modelData
                spacing: editor.theme.keySwatchGap
                Text {
                    text: "■"
                    color: key.modelData[1]
                    font.pixelSize: editor.theme.typeSmall
                }
                Text {
                    text: key.modelData[0]
                    color: editor.theme.inkDim
                    font.family: editor.theme.sans
                    font.pixelSize: editor.theme.typeSmall
                }
            }
        }
        Text {
            objectName: "pumpFollows"
            visible: editor.follows
            text: editor.followsLabel
            color: editor.theme.time
            font.family: editor.theme.sans
            font.pixelSize: editor.theme.typeSmall
            font.bold: true
        }
    }
    Text {
        objectName: "pumpGainReadout"
        anchors.right: parent.right
        anchors.rightMargin: editor.theme.keyInset
        y: editor.theme.spaceMd
        text: editor.dbText(editor.gainDb)
        color: editor.theme.shape
        font.family: editor.theme.mono
        font.pixelSize: editor.theme.typeBody
    }
}
