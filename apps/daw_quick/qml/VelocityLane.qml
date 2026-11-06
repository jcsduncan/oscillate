pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The velocity lane under the piano roll: one stem per note, its height the
// velocity. Drag a stem to set that note; drag across empty space to draw a
// line (a crescendo in one gesture); Up/Down on a focused stem nudges it
// (Shift for tens) and the box takes an exact value. Scope: the selected
// note (Piano) or pad (Drums) unless WHOLE CLIP is on. Humanise and Random
// reroll on every click. Every action is one undo step.
Item {
    id: lane
    objectName: "velocityLane"
    required property var bridge
    required property string clipId
    required property var notes
    required property bool drumView
    required property string selectedNoteId
    required property real keyboardWidth
    required property real gridWidth
    // Ticks across the grid: the piano roll's whole visible span.
    required property real spanTicks
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    required property color amberColor
    signal noteSelected(string noteId)
    property bool expanded: true
    property bool wholeClip: false
    property real humanizeStrength: 0.4
    property int randomMinimum: 40
    property int randomMaximum: 110
    readonly property real headerHeight: Theme.velocityHeaderHeight
    readonly property real stemArea: Theme.velocityStemArea
    // A drag across the stems shorter than this (px) draws nothing.
    readonly property real drawThreshold: 3
    implicitHeight: headerHeight + (expanded ? stemArea : 0)

    readonly property var selectedNote: notes.find(n => String(n.id) === selectedNoteId) || null
    // The edit scope as note ids.
    readonly property var scopeIds: {
        if (wholeClip || selectedNote === null)
            return wholeClip ? notes.map(n => String(n.id)) : []
        if (drumView)
            return notes.filter(n => Number(n.key) === Number(selectedNote.key))
                        .map(n => String(n.id))
        return [String(selectedNote.id)]
    }
    // A drawn line reaches every stem it crosses, or the pad's in Drums.
    readonly property var lineIds: drumView && selectedNote !== null && !wholeClip
        ? scopeIds : notes.map(n => String(n.id))
    readonly property var scopeSet: new Set(scopeIds)
    readonly property var fanOffsets: {
        const counts = new Map()
        const seen = new Map()
        const offsets = new Map()
        for (const note of notes) {
            const tick = Number(note.startTicks)
            counts.set(tick, (counts.get(tick) || 0) + 1)
        }
        for (const note of notes) {
            const tick = Number(note.startTicks)
            const index = seen.get(tick) || 0
            offsets.set(String(note.id), (index - (counts.get(tick) - 1) / 2) * Theme.velocityFanOffset)
            seen.set(tick, index + 1)
        }
        return offsets
    }
    readonly property string scopeName: wholeClip ? "whole clip"
        : selectedNote === null ? "nothing selected"
        : drumView ? "selected pad" : "selected note"

    function xAt(ticks) { return keyboardWidth + gridWidth * ticks / spanTicks }
    function ticksAt(x) {
        return Math.max(0, Math.min(spanTicks, (x - keyboardWidth) * spanTicks / gridWidth))
    }
    function velocityAt(y) {
        return Math.max(1, Math.min(127, Math.round(127 * (1 - (y - Theme.velocityStemInset) / (stemArea - 2 * Theme.velocityStemInset)))))
    }
    function stemHeight(velocity) { return (stemArea - 2 * Theme.velocityStemInset) * velocity / 127 }
    // ---- Header: label, scope, value, humanise and random -------------------
    Rectangle {
        width: lane.width; height: lane.headerHeight
        color: Theme.velocityHeaderGround; border.color: lane.lineColor
        Text {
            x: Theme.spaceSm; anchors.verticalCenter: parent.verticalCenter
            text: "VEL " + (lane.expanded ? "▴" : "▾")
            color: lane.accentColor; font.pixelSize: Theme.typeCaption; font.bold: true
            MouseArea {
                objectName: "velocityLaneExpand"
                anchors.fill: parent; anchors.margins: -Theme.spaceXs
                Accessible.role: Accessible.Button
                Accessible.name: lane.expanded ? "Fold the velocity lane" : "Show the velocity lane"
                Tip { text: lane.expanded ? "Fold the velocity lane" : "Show the velocity lane" }
                onClicked: lane.expanded = !lane.expanded
            }
        }
        Row {
            x: lane.keyboardWidth + Theme.spaceSm; height: parent.height; spacing: Theme.spaceSm
            component Chip: Rectangle {
                id: chip
                property string label
                property bool on: false
                property string help
                signal activated()
                width: chipText.implicitWidth + Theme.velocityChipPadding; height: Theme.velocityChipHeight; radius: Theme.radiusMd
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                color: on ? Theme.toggleOn : Theme.toggleOff
                border.color: activeFocus ? lane.accentColor : lane.lineColor
                activeFocusOnTab: lane.visible
                Accessible.role: Accessible.Button
                Accessible.name: help
                Tip { text: chip.help }
                Keys.onReturnPressed: activated()
                Keys.onSpacePressed: activated()
                Text { id: chipText; anchors.centerIn: parent; text: chip.label
                    color: chip.on ? lane.accentColor : lane.mainTextColor
                    font.pixelSize: Theme.typeMicro; font.bold: true }
                MouseArea { anchors.fill: parent; onClicked: chip.activated() }
            }
            Chip {
                objectName: "velocityWholeClip"
                label: "WHOLE CLIP"; on: lane.wholeClip
                help: "Edit the whole clip instead of the " + (lane.drumView ? "selected pad" : "selected note")
                onActivated: lane.wholeClip = !lane.wholeClip
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: !lane.wholeClip
                text: lane.scopeName.toUpperCase()
                color: lane.dimTextColor; font.pixelSize: Theme.typeMicro
            }
            // Exact value for the selected note.
            Rectangle {
                width: Theme.velocityValueWidth; height: Theme.velocityChipHeight; radius: Theme.radiusSm
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.velocityValueGround; border.color: valueInput.activeFocus ? lane.accentColor : lane.lineColor
                visible: lane.selectedNote !== null
                TextInput {
                    id: valueInput
                    objectName: "velocityValue"
                    anchors.fill: parent; anchors.margins: Theme.spaceXxs
                    horizontalAlignment: TextInput.AlignHCenter
                    verticalAlignment: TextInput.AlignVCenter
                    color: lane.mainTextColor; font.pixelSize: Theme.typeSmall
                    validator: IntValidator { bottom: 1; top: 127 }
                    text: lane.selectedNote ? String(lane.selectedNote.velocity) : ""
                    Accessible.role: Accessible.EditableText
                    Accessible.name: "Velocity of the selected note, 1 to 127"
                    onAccepted: lane.bridge.noteTools.set_note_velocity(lane.clipId, lane.selectedNoteId,
                                                              Number(text))
                }
            }
            Item { width: Theme.spaceMd; height: Theme.hairline }
            Chip {
                objectName: "velocityHumanize"
                label: "HUMANISE " + Math.round(lane.humanizeStrength * 100) + "%"
                help: "Humanise the " + lane.scopeName + ": small variation that keeps accents. "
                      + "Click again for another variation. Mouse wheel sets the strength."
                enabled: lane.scopeIds.length > 0
                opacity: enabled ? 1 : Theme.velocityDisabledOpacity
                onActivated: lane.bridge.noteTools.humanize_velocities(lane.clipId, lane.scopeIds,
                                                             lane.humanizeStrength)
                WheelHandler {
                    onWheel: event => lane.humanizeStrength = Math.max(0.05, Math.min(1,
                        lane.humanizeStrength + Math.sign(event.angleDelta.y) * 0.05))
                }
            }
            Chip {
                objectName: "velocityRandomize"
                label: "RANDOM " + lane.randomMinimum + "–" + lane.randomMaximum
                help: "Randomise the " + lane.scopeName + " between " + lane.randomMinimum
                      + " and " + lane.randomMaximum + ". Click again to reroll. "
                      + "Mouse wheel moves the range; Shift with the wheel widens it."
                enabled: lane.scopeIds.length > 0
                opacity: enabled ? 1 : Theme.velocityDisabledOpacity
                onActivated: lane.bridge.noteTools.randomize_velocities(lane.clipId, lane.scopeIds,
                                                              lane.randomMinimum, lane.randomMaximum)
                WheelHandler {
                    onWheel: event => {
                        const step = Math.sign(event.angleDelta.y) * 5
                        if (event.modifiers & Qt.ShiftModifier) {
                            lane.randomMinimum = Math.max(1, Math.min(lane.randomMaximum - 5,
                                                                      lane.randomMinimum - step))
                            lane.randomMaximum = Math.min(127, Math.max(lane.randomMinimum + 5,
                                                                        lane.randomMaximum + step))
                        } else {
                            const shift = Math.max(1 - lane.randomMinimum,
                                                   Math.min(127 - lane.randomMaximum, step))
                            lane.randomMinimum += shift
                            lane.randomMaximum += shift
                        }
                    }
                }
            }
        }
    }

    // ---- Stems ---------------------------------------------------------------
    Item {
        id: stems
        visible: lane.expanded
        y: lane.headerHeight; width: lane.width; height: lane.stemArea
        property bool drawing: false
        property real fromX: 0
        property real fromY: 0
        property real toX: 0
        property real toY: 0
        Rectangle {
            x: lane.keyboardWidth; width: lane.gridWidth; height: parent.height
            color: Theme.velocityGround; border.color: Theme.noteLaneEdge
            Repeater {
                // Guides at 32, 64, 96 and 127.
                model: [32, 64, 96, 127]
                delegate: Rectangle {
                    required property int modelData
                    y: stems.height - Theme.velocityStemInset - lane.stemHeight(modelData)
                    width: parent.width; height: Theme.hairline; color: Theme.velocityGuide
                }
            }
            MouseArea {
                objectName: "velocityLaneDraw"
                anchors.fill: parent
                Accessible.role: Accessible.Pane
                Accessible.name: "Velocity lane. Drag across the stems to draw a line."
                onPressed: mouse => {
                    stems.drawing = true
                    stems.fromX = mouse.x + lane.keyboardWidth; stems.fromY = mouse.y
                    stems.toX = stems.fromX; stems.toY = stems.fromY
                }
                onPositionChanged: mouse => {
                    if (!pressed) return
                    stems.toX = mouse.x + lane.keyboardWidth; stems.toY = mouse.y
                }
                onReleased: {
                    stems.drawing = false
                    if (Math.abs(stems.toX - stems.fromX) < lane.drawThreshold) return
                    lane.bridge.noteTools.draw_velocity_line(lane.clipId, lane.lineIds,
                        lane.ticksAt(stems.fromX), lane.velocityAt(stems.fromY),
                        lane.ticksAt(stems.toX), lane.velocityAt(stems.toY))
                }
                onCanceled: stems.drawing = false
            }
        }
        // The line being drawn.
        Canvas {
            anchors.fill: parent
            visible: stems.drawing
            onVisibleChanged: requestPaint()
            Connections {
                target: stems
                function onToXChanged() { if (stems.drawing) parent.requestPaint() }
                function onToYChanged() { if (stems.drawing) parent.requestPaint() }
            }
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = lane.amberColor
                ctx.lineWidth = Theme.velocityDrawStroke
                ctx.beginPath()
                ctx.moveTo(stems.fromX, stems.fromY)
                ctx.lineTo(stems.toX, stems.toY)
                ctx.stroke()
            }
        }
        Repeater {
            model: lane.notes
            delegate: Item {
                id: stem
                required property var modelData
                readonly property string noteId: String(modelData.id)
                readonly property int velocity: Number(modelData.velocity)
                readonly property bool inScope: lane.scopeSet.has(noteId)
                readonly property bool selected: lane.selectedNoteId === noteId
                property int dragVelocity: velocity
                property bool dragging: false
                property real lastAuditionMs: 0
                readonly property int shown: dragging ? dragVelocity : velocity
                objectName: "velocityStem-" + noteId
                x: lane.xAt(Number(modelData.startTicks)) + (lane.fanOffsets.get(noteId) || 0) - width / 2
                width: Theme.velocityStemHitWidth; height: stems.height
                z: selected ? 3 : 2
                activeFocusOnTab: lane.visible && lane.expanded
                Accessible.role: Accessible.Slider
                Accessible.name: "Velocity " + shown + ". Drag or press Up and Down to change."
                function nudge(delta) {
                    lane.bridge.noteTools.set_note_velocity(lane.clipId, noteId,
                                                  Math.max(1, Math.min(127, velocity + delta)))
                }
                Keys.onUpPressed: event => nudge(event.modifiers & Qt.ShiftModifier ? 10 : 1)
                Keys.onDownPressed: event => nudge(event.modifiers & Qt.ShiftModifier ? -10 : -1)
                Rectangle {
                    x: (parent.width - width) / 2; width: Theme.velocityStemWidth
                    height: lane.stemHeight(stem.shown)
                    y: stems.height - Theme.velocityStemInset - height
                    color: stem.selected ? lane.amberColor
                        : stem.inScope ? lane.accentColor : Theme.velocityStem
                }
                Rectangle {
                    x: (parent.width - width) / 2; width: Theme.velocityHeadSize; height: Theme.velocityHeadSize; radius: width / 2
                    y: stems.height - Theme.velocityStemInset - lane.stemHeight(stem.shown) - height / 2
                    color: stem.selected ? lane.amberColor : Theme.velocityHead
                    border.color: stem.activeFocus ? Theme.velocityHeadFocusEdge : Theme.velocityHeadEdge
                }
                Text {
                    visible: stem.dragging || stem.selected
                    x: Theme.velocityLabelX; y: Math.max(0, stems.height - Theme.velocityStemInset - lane.stemHeight(stem.shown) - Theme.velocityLabelRise)
                    text: stem.shown
                    color: lane.mainTextColor; font.pixelSize: Theme.typeCaption; font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SizeVerCursor
                    preventStealing: true
                    onPressed: mouse => {
                        stem.forceActiveFocus()
                        lane.noteSelected(stem.noteId)
                        stem.dragVelocity = stem.velocity
                        stem.dragging = true
                    }
                    onPositionChanged: mouse => {
                        if (!pressed) return
                        const next = lane.velocityAt(mapToItem(stems, 0, mouse.y).y)
                        if (next === stem.dragVelocity) return
                        stem.dragVelocity = next
                        const now = Date.now()
                        if (now - stem.lastAuditionMs >= 125) {
                            stem.lastAuditionMs = now
                            lane.bridge.preview_note_velocity(lane.clipId, stem.noteId, next)
                        }
                    }
                    onReleased: {
                        stem.dragging = false
                        if (stem.dragVelocity !== stem.velocity)
                            lane.bridge.noteTools.set_note_velocity(lane.clipId, stem.noteId,
                                                          stem.dragVelocity)
                    }
                    onCanceled: stem.dragging = false
                }
            }
        }
    }
}
