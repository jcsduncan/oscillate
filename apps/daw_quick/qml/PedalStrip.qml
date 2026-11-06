pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The pedal strip under the piano roll (ADR-0035): pedal-down spans drawn
// like notes. Drag on an empty stretch to press the pedal there; drag a
// span to move it, its ends to move the press or the lift, its top to set
// half-pedal depth; double-click or Delete to lift it. Sostenuto and una
// corda fold out under sustain.
Item {
    id: strip
    objectName: "pedalStrip"
    required property var bridge
    required property string clipId
    required property var pedals          // {sustain: [spans], sostenuto: [...], soft: [...]}
    required property real keyboardWidth
    required property real gridWidth
    // Ticks across the grid (the roll's visible span) and the grid's
    // columns over it; stepsPerBar is the grid division the bridge takes.
    // Pedals are edited inside the pattern, the part of the clip that plays.
    required property real spanTicks
    required property int columns
    required property int stepsPerBar
    required property real patternTicks
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    required property color amberColor
    property bool expanded: false
    readonly property real rowHeight: Theme.pedalRowHeight
    readonly property var rows: expanded
        ? [{pedal: "sustain", label: "PED", name: "Sustain pedal"},
           {pedal: "sostenuto", label: "SOS", name: "Sostenuto pedal"},
           {pedal: "soft", label: "U.C.", name: "Una corda (soft) pedal"}]
        : [{pedal: "sustain", label: "PED", name: "Sustain pedal"}]
    readonly property real gridTicks: spanTicks / columns
    readonly property int patternSteps: Math.max(1, Math.floor(patternTicks / gridTicks))
    implicitHeight: rows.length * rowHeight

    function tickAt(x) {
        return Math.max(0, Math.min(spanTicks, (x - keyboardWidth) * spanTicks / gridWidth))
    }
    function stepAt(x) {
        return Math.max(0, Math.min(patternSteps, Math.round(tickAt(x) / gridTicks)))
    }
    function xAt(ticks) { return keyboardWidth + gridWidth * ticks / spanTicks }

    Repeater {
        model: strip.rows
        delegate: Item {
            id: lane
            required property var modelData
            required property int index
            readonly property string pedal: modelData.pedal
            readonly property var spans: strip.pedals ? strip.pedals[pedal] || [] : []
            property int paintFrom: -1
            property int paintTo: -1
            y: index * strip.rowHeight
            width: strip.width; height: strip.rowHeight

            // Label, with the fold-out toggle on sustain's row.
            Rectangle {
                width: strip.keyboardWidth; height: parent.height
                color: Theme.pedalLabelGround; border.color: strip.lineColor
                Text {
                    x: Theme.spaceSm; anchors.verticalCenter: parent.verticalCenter
                    text: lane.modelData.label
                    color: strip.amberColor; font.pixelSize: Theme.typeCaption; font.bold: true
                }
                Text {
                    objectName: "pedalStripExpand"
                    visible: lane.index === 0
                    anchors.right: parent.right; anchors.rightMargin: Theme.pedalExpandMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: strip.expanded ? "▴" : "▾"
                    color: strip.dimTextColor; font.pixelSize: Theme.typeSmall
                    MouseArea {
                        anchors.fill: parent; anchors.margins: -Theme.spaceXs
                        Accessible.role: Accessible.Button
                        Accessible.name: strip.expanded ? "Hide sostenuto and una corda rows"
                                                        : "Show sostenuto and una corda rows"
                        Tip { text: strip.expanded ? "Hide sostenuto and una corda rows" : "Show sostenuto and una corda rows" }
                        onClicked: strip.expanded = !strip.expanded
                    }
                }
            }
            // The empty lane: drag to press the pedal over a stretch.
            Rectangle {
                x: strip.keyboardWidth; width: strip.gridWidth; height: parent.height
                color: Theme.pedalLaneGround; border.color: Theme.noteLaneEdge
                MouseArea {
                    objectName: "pedalLane-" + lane.pedal
                    anchors.fill: parent
                    Accessible.role: Accessible.Pane
                    Accessible.name: lane.modelData.name + ". Drag to press the pedal over a stretch."
                    onPressed: mouse => {
                        lane.paintFrom = strip.stepAt(mouse.x + strip.keyboardWidth)
                        lane.paintTo = lane.paintFrom
                    }
                    onPositionChanged: mouse => {
                        if (pressed) lane.paintTo = strip.stepAt(mouse.x + strip.keyboardWidth)
                    }
                    onReleased: {
                        const from = Math.min(lane.paintFrom, lane.paintTo)
                        const to = Math.max(lane.paintFrom, lane.paintTo)
                        if (to > from)
                            strip.bridge.noteTools.paint_pedal_grid(strip.clipId, lane.pedal, from, to, 1.0,
                                                          strip.stepsPerBar)
                        lane.paintFrom = -1
                    }
                    onCanceled: lane.paintFrom = -1
                }
            }
            // Live preview while drawing.
            Rectangle {
                visible: lane.paintFrom >= 0 && lane.paintTo !== lane.paintFrom
                x: strip.xAt(Math.min(lane.paintFrom, lane.paintTo) * strip.gridTicks)
                width: strip.gridWidth * Math.abs(lane.paintTo - lane.paintFrom) / strip.columns
                y: Theme.pedalSpanInset; height: parent.height - 2 * Theme.pedalSpanInset; radius: Theme.radiusXs
                color: strip.amberColor; opacity: Theme.pedalPreviewOpacity
            }
            Repeater {
                model: lane.spans
                delegate: Rectangle {
                    id: span
                    required property var modelData
                    readonly property real start: Number(modelData.startTicks)
                    readonly property real end: Number(modelData.endTicks)
                    readonly property real depth: Number(modelData.depth)
                    property real dragStart: start
                    property real dragEnd: end
                    property real dragDepth: depth
                    objectName: "pedalSpan-" + lane.pedal + "-" + start
                    x: strip.xAt(dragStart)
                    width: Math.max(Theme.pedalSpanMinSize, strip.xAt(dragEnd) - strip.xAt(dragStart))
                    // Full height is full pedal; lower is half-pedal.
                    height: Math.max(Theme.pedalSpanMinSize, (lane.height - 2 * Theme.pedalSpanInset) * dragDepth)
                    y: lane.height - Theme.pedalSpanInset - height
                    radius: Theme.radiusXs
                    color: activeFocus ? Theme.pedalSpanFocused : strip.amberColor
                    opacity: Theme.pedalSpanRestOpacity + Theme.pedalSpanDepthOpacity * dragDepth
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: lane.modelData.name + " down"
                        + (depth < 1 ? " at " + Math.round(depth * 100) + " percent" : "")
                        + ". Drag to move, drag the ends to change press or lift, drag the top "
                        + "for half pedal, Delete to lift."
                    Tip { text: lane.modelData.name + " down" + (span.depth < 1 ? " at " + Math.round(span.depth * 100) + "%" : "")
                                + " — drag to move, drag the ends to change press or lift, drag the top for half pedal, Delete lifts" }
                    Keys.onDeletePressed: strip.bridge.noteTools.remove_pedal_at(strip.clipId, lane.pedal,
                                                                       span.start)
                    // The lift mark.
                    Rectangle {
                        x: parent.width - Theme.strokeBold / 2; y: -Theme.strokeBold; width: Theme.strokeBold; height: parent.height + Theme.strokeBold
                        color: Theme.pedalLiftMark
                        visible: span.end < strip.patternTicks
                    }
                    // Body: move.
                    MouseArea {
                        anchors.fill: parent; anchors.leftMargin: Theme.pedalGrabWidth; anchors.rightMargin: Theme.pedalGrabWidth
                        anchors.topMargin: Theme.pedalDepthGrabHeight
                        property real pressTick: 0
                        cursorShape: Qt.SizeAllCursor
                        onPressed: mouse => {
                            span.forceActiveFocus()
                            pressTick = strip.tickAt(mapToItem(strip, mouse.x, 0).x)
                        }
                        onPositionChanged: mouse => {
                            if (!pressed) return
                            const delta = strip.tickAt(mapToItem(strip, mouse.x, 0).x) - pressTick
                            const length = span.end - span.start
                            const steps = Math.round((span.start + delta) / strip.gridTicks)
                            span.dragStart = Math.max(0, Math.min(strip.patternTicks - length,
                                                                  steps * strip.gridTicks))
                            span.dragEnd = span.dragStart + length
                        }
                        onReleased: strip.commit(lane.pedal, span)
                        onDoubleClicked: strip.bridge.noteTools.remove_pedal_at(strip.clipId, lane.pedal,
                                                                      span.start)
                    }
                    // Ends: press and lift.
                    Repeater {
                        model: 2
                        delegate: MouseArea {
                            id: edge
                            required property int index
                            x: index === 0 ? 0 : span.width - Theme.pedalGrabWidth; width: Theme.pedalGrabWidth; height: span.height
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            onPositionChanged: mouse => {
                                if (!pressed) return
                                const step = strip.stepAt(mapToItem(strip, mouse.x, 0).x)
                                if (edge.index === 0)
                                    span.dragStart = Math.min(step * strip.gridTicks,
                                                              span.dragEnd - strip.gridTicks)
                                else
                                    span.dragEnd = Math.max(step * strip.gridTicks,
                                                            span.dragStart + strip.gridTicks)
                            }
                            onReleased: strip.commit(lane.pedal, span)
                        }
                    }
                    // Top: half-pedal depth.
                    MouseArea {
                        width: span.width; height: Theme.pedalDepthGrabHeight
                        cursorShape: Qt.SizeVerCursor
                        preventStealing: true
                        onPositionChanged: mouse => {
                            if (!pressed) return
                            const y = mapToItem(lane, 0, mouse.y).y
                            span.dragDepth = Math.max(0.1, Math.min(1.0,
                                (lane.height - Theme.pedalSpanInset - y) / (lane.height - 2 * Theme.pedalSpanInset)))
                        }
                        onReleased: strip.bridge.noteTools.set_pedal_depth(strip.clipId, lane.pedal,
                                                                 span.start, span.dragDepth)
                    }
                }
            }
        }
    }

    // A moved or resized span commits as one edit and snaps back if refused.
    function commit(pedal, span) {
        if (span.dragStart !== span.start || span.dragEnd !== span.end)
            bridge.noteTools.move_pedal_span(clipId, pedal, span.start, span.dragStart, span.dragEnd)
        span.dragStart = Qt.binding(() => span.start)
        span.dragEnd = Qt.binding(() => span.end)
    }
}
