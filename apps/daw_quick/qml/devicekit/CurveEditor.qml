pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts

// The drawn-curve editor every device shares: one document curve, points
// {x, y, tension, step} with x and y 0..1, joined by segments bent with the
// engine's law (Contracts, spec 08a §3.2). The editor owns the plot mapping,
// hit testing and the pointer gestures; a device draws on its two layers
// (paintBack under, paintShape over) with the helpers below and picks the
// gestures it offers:
//   drag a point (snapped to snapSteps, Shift is free);
//   Alt-drag a segment to bend it (altBends), or drag the ring on its
//   midpoint (bendRings);
//   press empty space to add a point and drag it (pressAddsPoint), or
//   double-click to add one (doubleClickAdds, on the curve with addsOnCurve);
//   double-click a point to toggle its step (doubleClickSteps) or remove it
//   (doubleClickRemoves); right-click a point to remove it.
// Each drag is one document gesture (begin, updates, commit); Escape
// mid-drag cancels it. Devices add their own gestures through pressHook and
// dragHook, and their own value scale through levelOf and valueOf.
Rectangle {
    id: editor

    property KitTheme theme
    property var doc: null
    property string curveId: ""
    property var points: doc !== null && doc.curves_revision >= 0 ? doc.curve(curveId) : []
    // The value an empty curve reads.
    property real emptyValue: 0
    property bool editable: true

    // The plot: x 0..1 from plotLeft to plotRight, value 0..1 from
    // plotBottom up to plotTop.
    property real plotLeft: 0
    property real plotRight: width
    property real plotTop: 0
    property real plotBottom: height

    // Gestures.
    property int snapSteps: 0
    property bool altBends: false
    property bool bendRings: false
    property bool pressAddsPoint: false
    property bool doubleClickAdds: false
    property bool addsOnCurve: false
    property bool doubleClickSteps: false
    property bool doubleClickRemoves: false
    property bool rightClickRemoves: true
    // Whether the last point may be removed as well as the inner ones.
    property bool lastPointRemovable: false
    // Whether moving one end moves the other with it (a looping curve).
    property bool linkEnds: true
    property bool focusOnPress: true
    property bool hoverReadout: false
    // Paint under the curve layer on a layer of its own.
    property bool backLayer: true
    property int pointerShape: area.mode === "bend" ? Qt.SizeVerCursor : Qt.CrossCursor
    // The pointer area's objectName.
    property string areaName: ""

    // The point being dragged, or -1.
    readonly property int dragIndex: area.mode === "point" ? area.index : -1
    // The gesture in progress: mode, index, start and the points it began
    // from, for a device's own gestures (begin, start, cancel).
    property alias gesture: area

    // The flattest segment a ring can bend: below this rise its midpoint
    // carries no usable fraction.
    readonly property real ringFlatRise: 0.01
    readonly property real maxTension: 1

    signal paintBack(var ctx)
    signal paintShape(var ctx)
    signal pointGrabbed(int index)
    signal pointMoved(int index)
    signal tensionDragged(int index, real tension)
    signal ringBent(int index, real fraction)
    signal pointRemoved(int index)
    signal pointerMoved(real x, real y)
    signal pointerLeft()
    signal gestureEnded(string mode, var mouse)
    signal gestureCancelled()

    radius: theme.curveRadius
    color: theme.curveFill
    border.color: theme.curveEdge
    clip: true

    function px(t) { return plotLeft + t * (plotRight - plotLeft) }
    function py(v) { return plotBottom - v * (plotBottom - plotTop) }
    function tx(x) { return Math.max(0, Math.min(1, (x - plotLeft) / (plotRight - plotLeft))) }
    function ty(y) { return Math.max(0, Math.min(1, (plotBottom - y) / (plotBottom - plotTop))) }
    // Where a stored value sits on the plot's 0..1 scale, and back.
    function levelOf(value) { return value }
    function valueOf(level) { return level }
    function snapped(t, free) {
        return free || snapSteps <= 0 ? t : Math.round(t * snapSteps) / snapSteps
    }
    // The curve's value at x, and count + 1 values across it.
    function valueAt(x) { return Contracts.curve_at(points, x, emptyValue) }
    function sample(list, count) { return Contracts.sample_curve(list, count, emptyValue) }
    function nearest(x, y) {
        let best = -1
        let bestDistance = theme.curveHitRadius
        for (let i = 0; i < points.length; ++i) {
            const d = Math.hypot(px(points[i].x) - x, py(levelOf(points[i].y)) - y)
            if (d < bestDistance) {
                best = i
                bestDistance = d
            }
        }
        return best
    }
    // The segment holding t.
    function segmentAt(t) {
        for (let i = points.length - 2; i >= 0; --i)
            if (points[i].x <= t)
                return i
        return 0
    }
    // The segment whose bend ring is under the pointer, or -1.
    function ringAt(x, y) {
        for (let i = 0; i + 1 < points.length; ++i) {
            const a = points[i], b = points[i + 1]
            if (a.step)
                continue
            const mx = (a.x + b.x) / 2
            if (Math.hypot(px(mx) - x, py(levelOf(valueAt(mx))) - y) < theme.curveRingHitRadius)
                return i
        }
        return -1
    }
    function removable(index) {
        return index > 0 && (lastPointRemovable || index + 1 < points.length)
    }
    // A device's own press: true when it took the press.
    function pressHook(mouse, hit) { return false }
    // A device's own drag for a mode it began: true when it handled it.
    function dragHook(mode, mouse) { return false }

    // Painting helpers. traceCurve starts a path through sampled values;
    // strokeCurve strokes it as the glow and the line.
    function traceCurve(ctx, values) {
        ctx.beginPath()
        const last = values.length - 1
        for (let i = 0; i <= last; ++i) {
            const x = px(i / last), y = py(levelOf(values[i]))
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
        }
    }
    function strokeCurve(ctx, dash) {
        const line = theme.curveLine
        ctx.strokeStyle = Qt.rgba(line.r, line.g, line.b, theme.curveGlowAlpha)
        ctx.lineWidth = theme.curveGlowWidth
        ctx.stroke()
        ctx.strokeStyle = line
        ctx.lineWidth = theme.curveLineWidth
        ctx.setLineDash(dash)
        ctx.stroke()
        ctx.setLineDash([])
    }
    // The bend rings on each curved segment's midpoint.
    function paintRings(ctx) {
        for (let i = 0; i + 1 < points.length; ++i) {
            if (points[i].step)
                continue
            const mx = (points[i].x + points[i + 1].x) / 2
            ctx.beginPath()
            ctx.arc(px(mx), py(levelOf(valueAt(mx))), theme.curveRingRadius, 0, Math.PI * 2)
            ctx.fillStyle = theme.curveRingFill
            ctx.fill()
            ctx.strokeStyle = theme.curveRingEdge
            ctx.lineWidth = theme.curveRingWidth
            ctx.stroke()
        }
    }
    // The points, filled when active(i); after(i, x, y) draws a point's
    // own mark over it.
    function paintPoints(ctx, active, after) {
        for (let i = 0; i < points.length; ++i) {
            const x = px(points[i].x), y = py(levelOf(points[i].y))
            ctx.beginPath()
            ctx.arc(x, y, theme.curvePointRadius, 0, Math.PI * 2)
            ctx.fillStyle = active(i) ? theme.curvePointActiveFill : theme.curvePointFill
            ctx.fill()
            ctx.strokeStyle = theme.curvePointEdge
            ctx.lineWidth = theme.curvePointWidth
            ctx.stroke()
            after(i, x, y)
        }
    }

    function repaintBack() { backCanvas.requestPaint() }
    function repaintShape() { shapeCanvas.requestPaint() }
    onPointsChanged: repaintShape()
    onDragIndexChanged: repaintShape()
    onWidthChanged: { repaintBack(); repaintShape() }
    onHeightChanged: { repaintBack(); repaintShape() }

    Canvas {
        id: backCanvas
        anchors.fill: parent
        visible: editor.backLayer
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            editor.paintBack(ctx)
        }
    }
    Canvas {
        id: shapeCanvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            editor.paintShape(ctx)
        }
    }

    Keys.onEscapePressed: area.cancel()

    MouseArea {
        id: area
        objectName: editor.areaName
        property string mode: ""
        property int index: -1
        property bool editing: false
        property real startX: 0
        property real startY: 0
        property real startTension: 0
        property var startPoints: []
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: editor.hoverReadout
        preventStealing: true
        cursorShape: editor.pointerShape
        enabled: editor.editable

        // A gesture with no document edit (a rubber band), and one that
        // opens a document gesture.
        function start(kind, mouse) {
            mode = kind
            startX = mouse.x
            startY = mouse.y
            startPoints = editor.points.slice()
        }
        function begin(kind, mouse) {
            start(kind, mouse)
            editing = true
            editor.doc.begin_edit()
        }
        function reset() {
            mode = ""
            index = -1
            editing = false
        }
        function cancel() {
            if (editing)
                editor.doc.cancel_edit()
            editor.gestureCancelled()
            reset()
        }
        onPressed: mouse => {
            if (editor.focusOnPress)
                editor.forceActiveFocus()
            const hit = editor.nearest(mouse.x, mouse.y)
            if (mouse.button === Qt.RightButton) {
                if (editor.rightClickRemoves && editor.removable(hit)) {
                    editor.doc.remove_curve_point(editor.curveId, hit)
                    editor.pointRemoved(hit)
                }
                return
            }
            if (editor.pressHook(mouse, hit))
                return
            const free = (mouse.modifiers & Qt.ShiftModifier) !== 0
            if (editor.altBends && (mouse.modifiers & Qt.AltModifier)) {
                index = editor.segmentAt(editor.tx(mouse.x))
                startTension = editor.points[index] !== undefined ? editor.points[index].tension : 0
                begin("tension", mouse)
                return
            }
            if (hit >= 0) {
                index = hit
                begin("point", mouse)
                editor.pointGrabbed(hit)
                return
            }
            if (editor.bendRings) {
                const ring = editor.ringAt(mouse.x, mouse.y)
                if (ring >= 0) {
                    index = ring
                    begin("bend", mouse)
                }
                return
            }
            if (!editor.pressAddsPoint)
                return
            editor.doc.begin_edit()
            const added = editor.doc.insert_curve_point(editor.curveId, editor.snapped(editor.tx(mouse.x), free),
                                                        editor.valueOf(editor.ty(mouse.y)))
            if (added < 0) {
                editor.doc.cancel_edit()
                return
            }
            index = added
            editing = true
            start("point", mouse)
            editor.pointGrabbed(added)
        }
        onPositionChanged: mouse => {
            editor.pointerMoved(mouse.x, mouse.y)
            if (!pressed || mode === "")
                return
            if (editor.dragHook(mode, mouse))
                return
            if (mode === "tension") {
                const tension = Math.max(-editor.maxTension, Math.min(editor.maxTension,
                    startTension + (mouse.y - startY) / editor.theme.curveBendTravel))
                editor.doc.set_curve_tension(editor.curveId, index, tension)
                editor.tensionDragged(index, tension)
                return
            }
            if (mode === "bend") {
                const a = editor.points[index], b = editor.points[index + 1]
                if (a === undefined || b === undefined || Math.abs(b.y - a.y) < editor.ringFlatRise)
                    return
                const fraction = (editor.valueOf(editor.ty(mouse.y)) - a.y) / (b.y - a.y)
                editor.doc.set_curve_tension(editor.curveId, index, Contracts.tension_for_midpoint(fraction))
                editor.ringBent(index, fraction)
                return
            }
            const free = (mouse.modifiers & Qt.ShiftModifier) !== 0
            editor.doc.move_curve_point(editor.curveId, index, editor.snapped(editor.tx(mouse.x), free),
                                        editor.valueOf(editor.ty(mouse.y)), editor.linkEnds)
            editor.pointMoved(index)
        }
        onExited: editor.pointerLeft()
        onReleased: mouse => {
            if (editing)
                editor.doc.commit_edit()
            editor.gestureEnded(mode, mouse)
            reset()
        }
        onCanceled: cancel()
        onDoubleClicked: mouse => {
            const hit = editor.nearest(mouse.x, mouse.y)
            if (hit >= 0) {
                if (editor.doubleClickSteps && hit + 1 < editor.points.length)
                    editor.doc.set_curve_step(editor.curveId, hit, !editor.points[hit].step)
                else if (editor.doubleClickRemoves && hit > 0 && hit + 1 < editor.points.length)
                    editor.doc.remove_curve_point(editor.curveId, hit)
                return
            }
            if (!editor.doubleClickAdds)
                return
            const x = editor.snapped(editor.tx(mouse.x), (mouse.modifiers & Qt.ShiftModifier) !== 0)
            editor.doc.insert_curve_point(editor.curveId, x,
                                          editor.addsOnCurve ? editor.valueAt(x) : editor.valueOf(editor.ty(mouse.y)))
        }
    }
}
