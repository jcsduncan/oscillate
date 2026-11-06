pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// An automation envelope in the arrangement (ADR-0051), laid over a track's
// clips (overlay) or in a lane beneath them. It draws
// the parameter's envelope across the visible bars and edits it in place:
// click to add a point (snapped to the beat; Alt places it freely), drag
// points or the selection (one undo step), drag a segment to move it up or
// down, Ctrl-drag between two points to bow that segment (back near
// straight, it is a line again), Shift-drag a box to select, double-click or
// Delete to remove, right-click a point for its curve, and with Draw on,
// paint the envelope. Lines are straight unless a curve is chosen.
Rectangle {
    id: lane
    objectName: "automationLane"
    required property var controller
    required property string trackId
    required property string laneKey
    // The lane's projection ({laneId, name, discrete, points}) or null when
    // the parameter has no points yet.
    required property var laneData
    required property real viewStartBar
    required property real visibleBars
    required property bool snapToBeat
    required property bool drawMode
    required property color laneColor
    required property color lineColor
    required property color textColor
    required property string monoFamily
    // Over the clip row: translucent, so the waveform or notes show through
    // dimmed, with the Draw switch at its corner.
    property bool overlay: false
    signal drawToggled()

    readonly property real padding: Theme.spaceSm
    readonly property real pxPerBar: width / Math.max(1, visibleBars)
    readonly property var points: laneData ? laneData.points : []
    property var selection: []
    // Point id → {bar, value} while a gesture previews them.
    property var preview: ({})
    property string mode: ""
    property real pressBar: 0
    property real pressValue: 0
    property var origins: ({})
    // The point the pointer grabbed: the group's snap follows it.
    property string anchorId: ""
    property var stroke: []
    property real bandX: 0
    property real bandWidth: 0
    property real readoutX: -1
    property string readout: ""
    property string menuPoint: ""
    property real menuX: 0
    // A Ctrl-drag bowing one segment: its first point, and its bow.
    property int bendIndex: -1
    property real bendFrom: 0
    property real bendValue: 0
    readonly property real maxBend: 2 / 3
    readonly property int beatsPerBar: 4
    // The finest grid a snap reaches, as divisions of a bar.
    readonly property int finestGridDivision: 256
    // A snapped draw writes a point every sixteenth of a bar.
    readonly property int drawStepsPerBar: 16

    color: overlay ? Theme.automationOverlayGround : Theme.automationLaneGround
    clip: true
    activeFocusOnTab: true
    Accessible.role: Accessible.Graphic
    Accessible.name: (laneData ? laneData.name : laneKey) + " automation lane. Click to add a point, drag to move, Delete removes the selection."

    function barAt(x) { return viewStartBar + x / pxPerBar }
    function xAt(bar) { return (bar - viewStartBar) * pxPerBar }
    function valueAt(y) { return Math.max(0, Math.min(1, 1 - (y - padding) / (height - 2 * padding))) }
    function yAt(value) { return padding + (1 - value) * (height - 2 * padding) }
    // Snaps to the finest power-of-two division of a bar still 12 px wide.
    function snapped(bar, free) {
        if (free || !snapToBeat)
            return Math.max(0, bar)
        let division = beatsPerBar
        while (division < finestGridDivision && pxPerBar / (division * 2) >= Theme.arrangementGridMinSpacing)
            division *= 2
        return Math.max(0, Math.round(bar * division) / division)
    }
    function shown(point) {
        const previewed = preview[point.id]
        return previewed ? previewed : { bar: point.bar, value: point.value }
    }
    function nearestPoint(x, y) {
        let best = ""
        let bestDistance = Theme.automationPointHitRadius
        for (const point of points) {
            const at = shown(point)
            const distance = Math.hypot(xAt(at.bar) - x, yAt(at.value) - y)
            if (distance < bestDistance) {
                bestDistance = distance
                best = point.id
            }
        }
        return best
    }
    // The two points a segment under (x, y) joins, when the pointer is on it.
    function segmentAt(x, y) {
        for (let index = 0; index + 1 < points.length; ++index) {
            const left = points[index]
            const right = points[index + 1]
            const x0 = xAt(left.bar)
            const x1 = xAt(right.bar)
            if (x <= x0 + Theme.automationSegmentHitDistance || x >= x1 - Theme.automationSegmentHitDistance)
                continue
            const u = (x - x0) / Math.max(1, x1 - x0)
            const y0 = yAt(left.value)
            const y1 = yAt(left.shape === "hold" ? left.value : right.value)
            if (Math.abs(y0 + (y1 - y0) * u - y) <= Theme.automationSegmentHitDistance)
                return [left.id, right.id]
        }
        return []
    }
    // The segment whose span holds `bar`: the index of its first point, or -1.
    function segmentIndexAt(bar) {
        for (let index = 0; index + 1 < points.length; ++index) {
            if (bar > points[index].bar && bar < points[index + 1].bar)
                return index
        }
        return -1
    }
    // A bowed segment's progress at u (0..1): the cubic Bezier the
    // controller saves for a bend, its handles on the thirds lifted together.
    function bowed(u, bend) {
        const y1 = Math.max(0, Math.min(1, 1 / 3 + bend))
        const y2 = Math.max(0, Math.min(1, 2 / 3 + bend))
        const cubic = (t, a, b) => 3 * (1 - t) * (1 - t) * t * a + 3 * (1 - t) * t * t * b + t * t * t
        let low = 0
        let high = 1
        for (let iteration = 0; iteration < 24; ++iteration) {
            const middle = (low + high) / 2
            if (cubic(middle, 1 / 3, 2 / 3) < u)
                low = middle
            else
                high = middle
        }
        return cubic((low + high) / 2, y1, y2)
    }
    function clearGesture() {
        mode = ""
        bendIndex = -1
        preview = ({})
        origins = ({})
        stroke = []
        bandWidth = 0
        canvas.requestPaint()
    }
    function commitMoves() {
        const moves = []
        for (const id in preview)
            moves.push({ id: id, bar: preview[id].bar, value: preview[id].value })
        if (moves.length > 0 && laneData)
            controller.move_points(laneData.laneId, moves)
    }
    function removeSelection() {
        if (laneData && selection.length > 0)
            controller.remove_points(laneData.laneId, selection)
        selection = []
    }

    onPointsChanged: canvas.requestPaint()
    onViewStartBarChanged: canvas.requestPaint()
    onVisibleBarsChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onSelectionChanged: canvas.requestPaint()
    // Delete and Backspace remove the lane's selected points before the
    // shell's clip shortcut sees them.
    Keys.onShortcutOverride: function(event) {
        event.accepted = selection.length > 0
            && (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
    }
    Keys.onDeletePressed: removeSelection()
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Backspace) {
            removeSelection()
            event.accepted = true
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            context.strokeStyle = lane.lineColor
            if (!lane.overlay) {
                context.fillStyle = Theme.automationLaneDivider
                context.fillRect(0, height - Theme.hairline, width, Theme.hairline)
            }
            const stroke = Qt.rgba(lane.laneColor.r, lane.laneColor.g, lane.laneColor.b, 1)
            const fill = Qt.rgba(lane.laneColor.r, lane.laneColor.g, lane.laneColor.b, Theme.automationFillAlpha)
            context.lineWidth = Theme.automationStroke
            if (lane.points.length === 0) {
                const y = lane.yAt(lane.controller.current_value(lane.trackId, lane.laneKey))
                context.strokeStyle = Qt.rgba(lane.laneColor.r, lane.laneColor.g, lane.laneColor.b, Theme.automationIdleAlpha)
                context.setLineDash(Theme.automationIdleDash)
                context.beginPath()
                context.moveTo(0, y)
                context.lineTo(width, y)
                context.stroke()
                context.setLineDash([])
                return
            }
            // While points move, straight segments preview them; otherwise
            // the curve exactly as it plays.
            const path = []
            if (Object.keys(lane.preview).length > 0 || lane.stroke.length > 0) {
                const pts = lane.points.map(point => lane.shown(point))
                    .concat(lane.stroke).sort((a, b) => a.bar - b.bar)
                path.push([0, lane.yAt(pts[0].value)])
                for (const point of pts)
                    path.push([lane.xAt(point.bar), lane.yAt(point.value)])
                path.push([width, lane.yAt(pts[pts.length - 1].value)])
            } else {
                const samples = Math.max(2, Math.ceil(width / Theme.automationCurveStep))
                const curve = lane.controller.curve(lane.laneData.laneId, lane.viewStartBar,
                                                    lane.viewStartBar + lane.visibleBars, samples)
                // A segment being bowed previews its new curve.
                const bending = lane.mode === "bend" && lane.bendIndex >= 0
                const left = bending ? lane.points[lane.bendIndex] : null
                const right = bending ? lane.points[lane.bendIndex + 1] : null
                for (let index = 0; index < curve.length; ++index) {
                    const x = index * width / (curve.length - 1)
                    const bar = lane.barAt(x)
                    let value = curve[index]
                    if (bending && bar > left.bar && bar < right.bar) {
                        const u = (bar - left.bar) / (right.bar - left.bar)
                        value = left.value + (right.value - left.value) * lane.bowed(u, lane.bendValue)
                    }
                    path.push([x, lane.yAt(value)])
                }
            }
            context.beginPath()
            context.moveTo(path[0][0], height)
            for (const [x, y] of path)
                context.lineTo(x, y)
            context.lineTo(path[path.length - 1][0], height)
            context.closePath()
            context.fillStyle = fill
            context.fill()
            context.beginPath()
            context.moveTo(path[0][0], path[0][1])
            for (const [x, y] of path)
                context.lineTo(x, y)
            context.strokeStyle = stroke
            context.stroke()
            for (const point of lane.points) {
                const at = lane.shown(point)
                const x = lane.xAt(at.bar)
                if (x < -Theme.automationPointCullMargin || x > width + Theme.automationPointCullMargin)
                    continue
                const selected = lane.selection.indexOf(point.id) >= 0
                context.beginPath()
                context.arc(x, lane.yAt(at.value), selected ? Theme.automationPointRadiusSelected : Theme.automationPointRadius, 0, 2 * Math.PI)
                context.fillStyle = selected ? stroke : Theme.canvas
                context.fill()
                context.strokeStyle = stroke
                context.stroke()
            }
        }
    }

    // The selection box.
    Rectangle {
        visible: lane.mode === "band"
        x: Math.min(lane.bandX, lane.bandX + lane.bandWidth)
        width: Math.abs(lane.bandWidth)
        height: parent.height
        color: Qt.rgba(lane.laneColor.r, lane.laneColor.g, lane.laneColor.b, Theme.automationBandWashAlpha)
        border.color: Qt.rgba(lane.laneColor.r, lane.laneColor.g, lane.laneColor.b, Theme.automationBandEdgeAlpha)
    }

    // The value under the pointer, or of the point being moved.
    Rectangle {
        visible: lane.readoutX >= 0 && lane.readout.length > 0
        x: Math.max(0, Math.min(lane.width - width, lane.readoutX + Theme.automationReadoutOffset))
        y: Theme.spaceXxs
        width: readoutText.implicitWidth + Theme.automationReadoutPadding
        height: Theme.automationReadoutHeight
        radius: Theme.radiusSm
        color: Theme.panelRaised
        border.color: Theme.controlEdge
        Text {
            id: readoutText
            anchors.centerIn: parent
            text: lane.readout
            color: lane.textColor
            font.pixelSize: Theme.typeSmall
            font.family: lane.monoFamily
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: lane.drawMode ? Qt.CrossCursor : Qt.ArrowCursor
        onExited: lane.readoutX = -1
        onPressed: function(mouse) {
            lane.forceActiveFocus()
            lane.menuPoint = ""
            const bar = lane.barAt(mouse.x)
            const value = lane.valueAt(mouse.y)
            const free = (mouse.modifiers & Qt.AltModifier) !== 0
            const near = lane.nearestPoint(mouse.x, mouse.y)
            if (mouse.button === Qt.RightButton) {
                if (near.length > 0) {
                    lane.menuPoint = near
                    lane.menuX = mouse.x
                }
                return
            }
            lane.pressBar = bar
            lane.pressValue = value
            // Ctrl-drag between two points bows their segment.
            if ((mouse.modifiers & Qt.ControlModifier) !== 0 && !lane.drawMode && lane.laneData
                    && !lane.laneData.discrete) {
                const index = lane.segmentIndexAt(bar)
                if (index >= 0) {
                    lane.mode = "bend"
                    lane.bendIndex = index
                    lane.bendFrom = Number(lane.points[index].bend || 0)
                    lane.bendValue = lane.bendFrom
                    canvas.requestPaint()
                }
                return
            }
            if (lane.drawMode) {
                lane.mode = "draw"
                lane.stroke = [{ bar: lane.snapped(bar, free), value: value }]
                return
            }
            if (near.length > 0) {
                if ((mouse.modifiers & Qt.ShiftModifier) !== 0) {
                    const next = lane.selection.slice()
                    const at = next.indexOf(near)
                    if (at >= 0) next.splice(at, 1)
                    else next.push(near)
                    lane.selection = next
                } else if (lane.selection.indexOf(near) < 0) {
                    lane.selection = [near]
                }
                lane.anchorId = near
                lane.mode = "point"
            } else if ((mouse.modifiers & Qt.ShiftModifier) !== 0) {
                lane.mode = "band"
                lane.bandX = mouse.x
                lane.bandWidth = 0
                return
            } else {
                const segment = lane.segmentAt(mouse.x, mouse.y)
                if (segment.length === 2) {
                    lane.selection = segment
                    lane.mode = "segment"
                } else {
                    const added = lane.controller.add_point(lane.trackId, lane.laneKey,
                                                            lane.snapped(bar, free), value)
                    if (added.length === 0)
                        return
                    lane.selection = [added]
                    lane.anchorId = added
                    lane.mode = "added"
                    return
                }
            }
            const origins = {}
            for (const point of lane.points) {
                if (lane.selection.indexOf(point.id) >= 0)
                    origins[point.id] = { bar: point.bar, value: point.value }
            }
            lane.origins = origins
        }
        onPositionChanged: function(mouse) {
            const bar = lane.barAt(mouse.x)
            const value = lane.valueAt(mouse.y)
            const free = (mouse.modifiers & Qt.AltModifier) !== 0
            lane.readoutX = mouse.x
            if (!pressed) {
                lane.readout = lane.controller.value_text(lane.trackId, lane.laneKey, value)
                return
            }
            if (lane.mode === "bend") {
                // Up bows the line upwards, down bows it downwards, whichever
                // way the segment runs; half the lane's height is a full bow.
                const left = lane.points[lane.bendIndex]
                const right = lane.points[lane.bendIndex + 1]
                const rising = right.value >= left.value ? 1 : -1
                lane.bendValue = Math.max(-lane.maxBend, Math.min(lane.maxBend,
                    lane.bendFrom + (value - lane.pressValue) * rising * lane.maxBend / 0.5))
                lane.readout = Math.abs(lane.bendValue) < 0.02 ? "Straight"
                    : "Curve " + Math.round(Math.abs(lane.bendValue) / lane.maxBend * 100) + "%"
                canvas.requestPaint()
                return
            }
            if (lane.mode === "added") {
                // A point placed by this press follows the pointer.
                const origins = {}
                for (const point of lane.points) {
                    if (lane.selection.indexOf(point.id) >= 0)
                        origins[point.id] = { bar: point.bar, value: point.value }
                }
                lane.origins = origins
                lane.mode = "point"
            }
            if (lane.mode === "point" || lane.mode === "segment") {
                const ids = Object.keys(lane.origins)
                if (ids.length === 0)
                    return
                const lead = lane.origins[lane.anchorId] || lane.origins[ids[0]]
                const deltaBar = lane.mode === "segment" ? 0
                    : lane.snapped(lead.bar + bar - lane.pressBar, free) - lead.bar
                const deltaValue = value - lane.pressValue
                const next = {}
                for (const id of ids) {
                    const origin = lane.origins[id]
                    const moved = lane.laneData && lane.laneData.discrete
                        ? Math.round(Math.max(0, Math.min(1, origin.value + deltaValue)))
                        : Math.max(0, Math.min(1, origin.value + deltaValue))
                    next[id] = { bar: Math.max(0, origin.bar + deltaBar), value: moved }
                }
                lane.preview = next
                lane.readout = lane.controller.value_text(lane.trackId, lane.laneKey, next[ids[0]].value)
                canvas.requestPaint()
            } else if (lane.mode === "band") {
                lane.bandWidth = mouse.x - lane.bandX
                const from = lane.barAt(Math.min(lane.bandX, mouse.x))
                const to = lane.barAt(Math.max(lane.bandX, mouse.x))
                lane.selection = lane.points.filter(point => point.bar >= from && point.bar <= to)
                    .map(point => point.id)
            } else if (lane.mode === "draw") {
                const step = lane.snapToBeat && !free ? 1 / lane.drawStepsPerBar : 1 / lane.pxPerBar * Theme.automationDrawColumn
                const column = Math.max(0, Math.round(bar / step) * step)
                const stroke = lane.stroke.filter(point => Math.abs(point.bar - column) > step / 2)
                stroke.push({ bar: column, value: value })
                lane.stroke = stroke
                lane.readout = lane.controller.value_text(lane.trackId, lane.laneKey, value)
                canvas.requestPaint()
            }
        }
        onReleased: function(mouse) {
            if (lane.mode === "bend" && lane.bendIndex >= 0) {
                lane.controller.bend(lane.laneData.laneId, lane.points[lane.bendIndex].id,
                                     lane.bendValue)
            } else if (lane.mode === "point" || lane.mode === "segment") {
                lane.commitMoves()
            } else if (lane.mode === "draw" && lane.stroke.length > 0) {
                const stroke = lane.stroke.slice().sort((a, b) => a.bar - b.bar)
                lane.controller.draw(lane.trackId, lane.laneKey, stroke[0].bar,
                                     stroke[stroke.length - 1].bar, stroke)
            }
            lane.clearGesture()
        }
        onCanceled: lane.clearGesture()
        onDoubleClicked: function(mouse) {
            const near = lane.nearestPoint(mouse.x, mouse.y)
            if (near.length > 0 && lane.laneData) {
                lane.controller.remove_points(lane.laneData.laneId, [near])
                lane.selection = []
            }
        }
    }

    Rectangle {
        objectName: "automationDrawSwitch"
        visible: lane.overlay
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceSm
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spaceXs
        z: 2
        width: drawText.implicitWidth + Theme.spaceLg
        height: Theme.automationDrawHeight
        radius: Theme.radiusMd
        color: lane.drawMode ? lane.laneColor : Theme.panelRaised
        border.color: Theme.controlEdge
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: lane.drawMode ? "Stop drawing automation" : "Draw automation"
        Keys.onReturnPressed: lane.drawToggled()
        Keys.onSpacePressed: lane.drawToggled()
        Text {
            id: drawText
            anchors.centerIn: parent
            text: "✎ Draw"
            color: lane.drawMode ? Theme.canvas : lane.textColor
            font.pixelSize: Theme.typeSmall
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            onClicked: lane.drawToggled()
        }
    }

    // A point's curve, from its right-click.
    Rectangle {
        objectName: "automationCurveMenu"
        visible: lane.menuPoint.length > 0
        x: Math.max(0, Math.min(lane.width - width, lane.menuX))
        y: Theme.spaceXxs
        width: curveRow.implicitWidth + Theme.spaceMd
        height: lane.height - Theme.spaceXs
        radius: Theme.radiusLg
        color: Theme.panelRaised
        border.color: Theme.controlEdge
        z: 3
        Row {
            id: curveRow
            anchors.centerIn: parent
            spacing: Theme.spaceXs
            Repeater {
                model: [["linear", "Line"], ["ease_in_out", "Ease"], ["exponential", "Exp"],
                        ["quadratic", "Slow"], ["hold", "Step"]]
                delegate: Rectangle {
                    id: curveChoice
                    required property var modelData
                    width: choiceText.implicitWidth + Theme.spaceLg
                    height: Theme.automationChoiceHeight
                    radius: Theme.radiusMd
                    color: choiceArea.containsMouse ? Theme.automationChoiceHover : Theme.canvas
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    border.color: Theme.controlEdge
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: curveChoice.modelData[1] + " curve"
                    Text {
                        id: choiceText
                        anchors.centerIn: parent
                        text: curveChoice.modelData[1]
                        color: lane.textColor
                        font.pixelSize: Theme.typeSmall
                    }
                    MouseArea {
                        Tip {}
                        id: choiceArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (lane.laneData)
                                lane.controller.set_curve(lane.laneData.laneId, lane.menuPoint,
                                                          curveChoice.modelData[0])
                            lane.menuPoint = ""
                        }
                    }
                }
            }
        }
    }
}
