pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The project tempo lane under the ruler (ADR-0112). It draws the tempo as
// it plays, ramps included, across the visible bars. Collapsed it is a thin
// line; expanded it edits: drag across bars on empty lane to draw a ramp to
// the tempo under the pointer, drag a point to move it, double-click empty
// lane to add a point there, double-click a point to remove it, and
// right-click for the shape of the ramp after a point (straight, S-curve,
// ease in, ease out, hold). Positions snap to bars (Alt: free) and every
// gesture is one undo step that plays live.
Rectangle {
    id: lane
    objectName: "tempoLane"
    required property var controller
    required property real viewStartBar
    required property real visibleBars
    required property bool expanded
    required property string monoFamily
    signal expandRequested()

    readonly property var points: controller.points
    readonly property real low: controller.lowBpm
    readonly property real high: controller.highBpm
    readonly property real pxPerBar: width / Math.max(1e-9, visibleBars)
    readonly property real padding: expanded ? Theme.tempoLanePadding : Theme.hairline
    readonly property int beatsPerBar: 4
    // Tempi land on a tenth of a BPM unless Alt is held.
    readonly property real bpmStep: 0.1
    readonly property var guideSteps: [1, 2, 5, 10, 20, 50]
    // A drag shorter than this (px) is a click.
    readonly property real dragSlop: 4
    // Curve samples: one every two pixels, within the controller's limits.
    readonly property int pixelsPerSample: 2
    readonly property int maxSamples: 1024
    // The new ramp's shape.
    readonly property string rampShape: "s_curve"
    readonly property real barEpsilon: 1e-9
    // A free (Alt) ramp's length shows to a hundredth of a bar.
    readonly property int barDecimals: 2

    // Gesture state: "", "point" or "ramp".
    property string mode: ""
    property int dragIndex: -1
    property real pressX: 0
    property real pressY: 0
    // A point only moves once the pointer has really moved.
    property bool dragged: false
    // A double-click counts only when both presses were the left button on
    // the lane (a right-click then a click in its menu is not one).
    property int lastButton: Qt.NoButton
    property bool leftTwice: false
    property real dragBar: 0
    property real dragBpm: 0
    property real rampFrom: 0
    property real hoverX: -1
    property int menuIndex: -1
    property real menuX: 0

    color: Theme.tempoLaneGround
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: "Project tempo lane. Drag across bars to draw a ramp, drag a point to move it, double-click to add or remove a point, right-click for its shape."

    function barAt(x) { return viewStartBar + x / Math.max(1e-9, pxPerBar) }
    function xAt(bar) { return (bar - viewStartBar) * pxPerBar }
    function yAt(bpm) { return padding + (high - bpm) / Math.max(1e-9, high - low) * (height - 2 * padding) }
    function bpmAt(y, free) {
        const bpm = high - (y - padding) / Math.max(1, height - 2 * padding) * (high - low)
        const bounded = Math.max(low, Math.min(high, bpm))
        return free ? bounded : Math.round(bounded / bpmStep) * bpmStep
    }
    function snapBar(bar, free) {
        const bounded = Math.max(0, bar)
        return free ? bounded : Math.round(bounded)
    }
    function pointNear(x, y) {
        let best = -1
        let distance = Theme.tempoLanePointGrab
        for (let index = 0; index < points.length; ++index) {
            const point = points[index]
            const d = Math.hypot(xAt(point.bar) - x, yAt(point.bpm) - y)
            if (d < distance) {
                distance = d
                best = index
            }
        }
        return best
    }
    // The point that starts the segment under `bar`.
    function segmentAt(bar) {
        let found = 0
        for (let index = 0; index < points.length; ++index)
            if (points[index].bar <= bar + barEpsilon)
                found = index
        return found
    }
    function shapeName(key) {
        const found = controller.shapes.find(shape => shape.key === key)
        return found ? found.name : ""
    }
    function bars(count) {
        const rounded = Number(count.toFixed(barDecimals))
        return rounded + (rounded === 1 ? " bar" : " bars")
    }
    // The tag for the segment from point `index`.
    function segmentTag(index) {
        const from = points[index]
        const to = index + 1 < points.length ? points[index + 1] : null
        if (!to || from.shape === "hold")
            return from.bpm.toFixed(2) + " · hold"
        return from.bpm.toFixed(2) + " → " + to.bpm.toFixed(2) + " over " + bars(to.bar - from.bar)
            + " · " + shapeName(from.shape)
    }
    onWidthChanged: curve.requestPaint()
    onHeightChanged: curve.requestPaint()
    onViewStartBarChanged: curve.requestPaint()
    onVisibleBarsChanged: curve.requestPaint()
    Connections {
        target: lane.controller
        // An undo or redo under a drag ends the drag: its index may now
        // name another point.
        function onChanged() {
            lane.mode = ""
            lane.dragIndex = -1
            lane.menuIndex = -1
            curve.requestPaint()
        }
    }

    // BPM guides, expanded only.
    Repeater {
        model: {
            if (!lane.expanded || !(lane.high > lane.low))
                return []
            const pxPerBpm = (lane.height - 2 * lane.padding) / (lane.high - lane.low)
            const step = lane.guideSteps.find(candidate => candidate * pxPerBpm >= Theme.tempoLaneGuideMinSpacing)
                || lane.guideSteps[lane.guideSteps.length - 1]
            const guides = []
            for (let bpm = Math.ceil(lane.low / step) * step; bpm <= lane.high; bpm += step)
                guides.push(bpm)
            return guides
        }
        delegate: Item {
            required property real modelData
            y: lane.yAt(modelData)
            width: lane.width
            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.tempoLaneGuide }
            Text {
                x: Theme.spaceSm
                y: -implicitHeight
                text: parent.modelData
                color: Theme.textDim
                font.pixelSize: Theme.typeMicro
                font.family: lane.monoFamily
            }
        }
    }

    // The ramp being drawn.
    Rectangle {
        objectName: "tempoLaneRampPreview"
        visible: lane.mode === "ramp"
        readonly property real from: Math.min(lane.rampFrom, lane.dragBar)
        readonly property real to: Math.max(lane.rampFrom, lane.dragBar)
        x: lane.xAt(from)
        width: Math.max(Theme.hairline, lane.xAt(to) - x)
        height: lane.height
        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, Theme.tempoLaneRampWashAlpha)
        border.color: Theme.accent
        border.width: Theme.hairline
    }

    Canvas {
        id: curve
        objectName: "tempoLaneCurve"
        anchors.fill: parent
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            if (width < 2)
                return
            const samples = Math.max(2, Math.min(lane.maxSamples, Math.ceil(width / lane.pixelsPerSample)))
            const values = lane.controller.curve(lane.viewStartBar, lane.viewStartBar + lane.visibleBars, samples)
            if (values.length < 2)
                return
            context.strokeStyle = Theme.tempoLaneCurve
            context.lineWidth = Theme.tempoLaneCurveWidth
            context.beginPath()
            for (let index = 0; index < values.length; ++index) {
                const x = index * width / (values.length - 1)
                const y = lane.yAt(values[index])
                if (index === 0)
                    context.moveTo(x, y)
                else
                    context.lineTo(x, y)
            }
            context.stroke()
            // The ramp being drawn, straight from its start to the pointer.
            if (lane.mode === "ramp") {
                const from = Math.min(lane.rampFrom, lane.dragBar)
                const to = Math.max(lane.rampFrom, lane.dragBar)
                context.strokeStyle = String(Theme.accent)
                context.beginPath()
                context.moveTo(lane.xAt(from), lane.yAt(lane.controller.bpm_at(from)))
                context.lineTo(lane.xAt(to), lane.yAt(lane.dragBpm))
                context.stroke()
            }
        }
    }

    // The tempo at the view's start.
    Text {
        objectName: "tempoLaneNow"
        visible: lane.expanded
        x: Theme.spaceSm
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spaceXxs
        // Read again whenever the map changes (points notifies with it).
        readonly property real bpm: {
            lane.points
            return lane.controller.bpm_at(Math.max(0, lane.viewStartBar))
        }
        text: bpm.toFixed(2)
        color: Theme.tempoLaneCurve
        font.pixelSize: Theme.typeMicro
        font.family: lane.monoFamily
    }

    Repeater {
        model: lane.expanded ? lane.points : []
        delegate: Item {
            id: point
            objectName: "tempoLanePoint"
            required property var modelData
            required property int index
            readonly property bool dragged: lane.mode === "point" && lane.dragIndex === index
            x: lane.xAt(dragged ? lane.dragBar : modelData.bar)
            y: lane.yAt(dragged ? lane.dragBpm : modelData.bpm)
            visible: x >= -Theme.tempoLanePointSize && x <= lane.width + Theme.tempoLanePointSize
            Rectangle {
                x: -width / 2
                y: -height / 2
                width: Theme.tempoLanePointSize
                height: Theme.tempoLanePointSize
                radius: width / 2
                color: Theme.canvas
                border.color: Theme.tempoLaneCurve
                border.width: Theme.strokeBold
            }
            Text {
                x: Theme.spaceSm
                y: point.y - lane.padding < implicitHeight ? Theme.spaceXxs : -implicitHeight - Theme.spaceXxs
                text: (point.dragged ? lane.dragBpm : point.modelData.bpm).toFixed(2)
                color: Theme.tempoLaneCurve
                font.pixelSize: Theme.typeMicro
                font.family: lane.monoFamily
            }
        }
    }

    // The hovered or drawn ramp's tag.
    Rectangle {
        objectName: "tempoLaneTag"
        readonly property string text: {
            if (lane.mode === "ramp") {
                const from = Math.min(lane.rampFrom, lane.dragBar)
                const to = Math.max(lane.rampFrom, lane.dragBar)
                return lane.controller.bpm_at(from).toFixed(2) + " → " + lane.dragBpm.toFixed(2)
                    + " over " + lane.bars(to - from) + " · " + lane.shapeName(lane.rampShape)
            }
            if (lane.mode === "point")
                return lane.dragBpm.toFixed(2) + " BPM"
            return lane.hoverX >= 0 && lane.points.length > 0 ? lane.segmentTag(lane.segmentAt(lane.barAt(lane.hoverX))) : ""
        }
        visible: lane.expanded && text.length > 0 && lane.menuIndex < 0
        x: Math.max(0, Math.min(lane.width - width, (lane.mode === "ramp" ? lane.xAt(Math.min(lane.rampFrom, lane.dragBar)) : lane.hoverX) + Theme.spaceSm))
        y: Theme.spaceXs
        width: tagText.implicitWidth + Theme.spaceLg
        height: Theme.tempoLaneTagHeight
        radius: Theme.radiusSm
        color: lane.mode === "ramp" ? Theme.accent : Theme.panelRaised
        border.color: Theme.controlEdge
        Text {
            id: tagText
            anchors.centerIn: parent
            text: parent.text
            color: lane.mode === "ramp" ? Theme.canvas : Theme.chipText
            font.pixelSize: Theme.typeMicro
            font.family: lane.monoFamily
        }
    }

    MouseArea {
        id: area
        objectName: "tempoLaneArea"
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onExited: lane.hoverX = -1
        onPressed: mouse => {
            lane.leftTwice = lane.lastButton === Qt.LeftButton && mouse.button === Qt.LeftButton
            lane.lastButton = mouse.button
            lane.menuIndex = -1
            if (!lane.expanded) {
                lane.expandRequested()
                return
            }
            const free = (mouse.modifiers & Qt.AltModifier) !== 0
            const near = lane.pointNear(mouse.x, mouse.y)
            if (mouse.button === Qt.RightButton) {
                lane.menuIndex = near >= 0 ? near : lane.segmentAt(lane.barAt(mouse.x))
                lane.menuX = mouse.x
                return
            }
            lane.pressX = mouse.x
            lane.pressY = mouse.y
            lane.dragged = false
            if (near >= 0) {
                lane.mode = "point"
                lane.dragIndex = near
                lane.dragBar = lane.points[near].bar
                lane.dragBpm = lane.points[near].bpm
            } else {
                lane.mode = "ramp"
                lane.rampFrom = lane.snapBar(lane.barAt(mouse.x), free)
                lane.dragBar = lane.rampFrom
                lane.dragBpm = lane.bpmAt(mouse.y, free)
            }
        }
        onPositionChanged: mouse => {
            lane.hoverX = mouse.x
            if (!pressed || lane.mode === "")
                return
            if (!lane.dragged && Math.abs(mouse.x - lane.pressX) < lane.dragSlop
                    && Math.abs(mouse.y - lane.pressY) < lane.dragSlop)
                return
            lane.dragged = true
            const free = (mouse.modifiers & Qt.AltModifier) !== 0
            lane.dragBpm = lane.bpmAt(mouse.y, free)
            if (lane.mode === "point" && lane.points[lane.dragIndex].initial)
                lane.dragBar = 0
            else
                lane.dragBar = lane.snapBar(lane.barAt(mouse.x), free)
            curve.requestPaint()
        }
        onReleased: mouse => {
            const moved = Math.abs(mouse.x - lane.pressX) >= lane.dragSlop
            if (lane.mode === "point" && lane.dragged) {
                const point = lane.points[lane.dragIndex]
                if (point && (Math.abs(lane.dragBar - point.bar) > lane.barEpsilon || Math.abs(lane.dragBpm - point.bpm) > lane.bpmStep / 2))
                    lane.controller.move_point(point.tick, lane.dragBar, lane.dragBpm)
            } else if (lane.mode === "ramp" && moved && Math.abs(lane.dragBar - lane.rampFrom) > lane.barEpsilon) {
                lane.controller.ramp(Math.min(lane.rampFrom, lane.dragBar), Math.max(lane.rampFrom, lane.dragBar),
                                     lane.dragBpm, lane.rampShape)
            }
            lane.mode = ""
            lane.dragIndex = -1
            curve.requestPaint()
        }
        onCanceled: {
            lane.mode = ""
            lane.dragIndex = -1
            curve.requestPaint()
        }
        onDoubleClicked: mouse => {
            if (!lane.expanded || mouse.button !== Qt.LeftButton || !lane.leftTwice)
                return
            const near = lane.pointNear(mouse.x, mouse.y)
            const free = (mouse.modifiers & Qt.AltModifier) !== 0
            if (near >= 0)
                lane.controller.remove_point(lane.points[near].tick)
            else
                lane.controller.add_point(lane.snapBar(lane.barAt(mouse.x), free), lane.bpmAt(mouse.y, free))
        }
    }

    // A right-click: the shape of the ramp after a point, or remove it.
    Rectangle {
        id: menu
        objectName: "tempoLaneMenu"
        visible: lane.menuIndex >= 0 && lane.menuIndex < lane.points.length
        readonly property var point: visible ? lane.points[lane.menuIndex] : null
        x: Math.max(0, Math.min(lane.width - width, lane.menuX))
        y: Theme.spaceXxs
        z: 3
        width: Theme.tempoLaneMenuWidth
        height: lane.height - Theme.spaceXs
        radius: Theme.radiusLg
        color: Theme.panelRaised
        border.color: Theme.controlEdge
        Accessible.role: Accessible.PopupMenu
        Accessible.name: "Tempo point commands"
        Flow {
            anchors.fill: parent
            anchors.margins: Theme.spaceXs
            spacing: Theme.spaceXs
            Repeater {
                model: menu.point ? lane.controller.shapes.concat(menu.point.initial ? [] : [{ key: "remove", name: "Remove" }]) : []
                delegate: Rectangle {
                    id: choice
                    required property var modelData
                    readonly property bool active: !!menu.point && (modelData.key === "remove" || !menu.point.last)
                    readonly property bool lit: !!menu.point && menu.point.shape === modelData.key && !menu.point.last
                    objectName: modelData.key === "remove" ? "tempoRemovePoint" : "tempoShape_" + modelData.key
                    width: choiceText.implicitWidth + Theme.spaceLg
                    height: Theme.automationChoiceHeight
                    radius: Theme.radiusMd
                    opacity: active ? 1 : Theme.clipEditorInactiveOpacity
                    color: lit ? Theme.menuHover : choiceArea.containsMouse && active ? Theme.automationChoiceHover : Theme.canvas
                    border.color: Theme.controlEdge
                    Accessible.role: Accessible.MenuItem
                    Accessible.name: modelData.key === "remove" ? "Remove tempo point" : modelData.name + " to the next point"
                    Text {
                        id: choiceText
                        anchors.centerIn: parent
                        text: choice.modelData.name
                        color: Theme.chipText
                        font.pixelSize: Theme.typeSmall
                    }
                    MouseArea {
                        id: choiceArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            const point = menu.point
                            const active = choice.active
                            lane.menuIndex = -1
                            if (!active || !point)
                                return
                            if (choice.modelData.key === "remove")
                                lane.controller.remove_point(point.tick)
                            else
                                lane.controller.set_shape(point.tick, choice.modelData.key)
                        }
                    }
                }
            }
        }
    }
}
