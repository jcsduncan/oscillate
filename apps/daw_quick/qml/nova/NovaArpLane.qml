pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// One arp lane (ADR-0078): its lock, name and length, then its 32 steps,
// those past the length dimmed. Each lane loops at its own length, so the
// amber playhead sits on (pattern step mod length). Levels and numbers are
// painted by dragging across the steps (one undo step a drag); toggles
// paint the state the first step flipped to; choices step on with a
// click (back with the right button). Double-click returns a step to its
// default; the length steps with − and + or the wheel. From the keyboard
// the lock toggles with Space, the length steps with the arrows, and the
// steps take a cursor: Left/Right move it, Up/Down step its value (a
// toggle flips, a choice steps on), Space flips or steps on, Delete
// returns it to its default.
Item {
    id: lane
    required property var panel
    // One of NovaArp.lanes: {id, name, kind, minimum, maximum, fallback, choices}.
    required property var spec
    required property color accent
    property int maxSteps: 32
    property int patternStep: -1
    property real headerWidth: panel.theme.arpLaneHeaderWidth
    property real cellGap: panel.theme.spaceXxs
    readonly property var controller: panel.controller
    readonly property var arp: controller.arp
    readonly property string base: "nova.arp.lane." + spec.id + "."
    readonly property int length: controller.revision >= 0 ? Math.round(controller.value(base + "length")) : 16
    readonly property int playing: patternStep >= 0 ? patternStep % Math.max(1, length) : -1
    readonly property bool locked: arp !== null && arp.locks.indexOf(spec.id) >= 0
    readonly property real cellWidth: (width - headerWidth - cellGap * (maxSteps - 1)) / maxSteps
    readonly property bool signed: spec.minimum < 0
    // A toggle's dot, as a share of the cell's shorter side.
    readonly property real toggleDotShare: 0.42
    // The step the keyboard cursor is on.
    property int cursor: 0

    objectName: "novaArpLane:" + spec.id
    implicitHeight: panel.theme.arpLaneHeight

    function stepId(i) { return base + (i + 1) }
    function valueAt(i) { return controller.revision >= 0 ? controller.value(stepId(i)) : spec.fallback }
    // A step's value from a y inside the cells (top is the maximum).
    function valueFromY(y) {
        const t = Math.max(0, Math.min(1, 1 - y / cells.height))
        const v = spec.minimum + t * (spec.maximum - spec.minimum)
        return spec.kind === "level" ? v : Math.round(v)
    }
    function cellAt(x) {
        return Math.max(0, Math.min(maxSteps - 1, Math.floor(x / (cellWidth + cellGap))))
    }
    function label(v) {
        if (spec.kind === "choice")
            return v === spec.fallback ? "·" : spec.choices[Math.round(v)]
        if (spec.kind === "integer")
            return signed ? Format.signed_number(Math.round(v), 0) : Format.number(Math.round(v), 0)
        return ""
    }
    function touchStep(i) {
        panel.touchId(stepId(i), spec.name.toUpperCase() + " " + (i + 1))
    }
    // Steps a step's value from the keyboard: a toggle flips, a choice
    // steps on (wrapping), a number steps by one, a level by the
    // controller's step.
    function keyStep(i, direction) {
        if (!controller.editable)
            return
        if (spec.kind === "toggle") {
            controller.set_value(stepId(i), valueAt(i) >= 0.5 ? 0 : 1)
        } else if (spec.kind === "choice") {
            const count = spec.choices.length
            controller.set_value(stepId(i), (Math.round(valueAt(i)) + direction + count) % count)
        } else {
            controller.step(stepId(i), direction, false)
        }
        touchStep(i)
    }
    function setLength(n) {
        controller.set_value(base + "length", Math.max(1, Math.min(maxSteps, n)))
        panel.touchId(base + "length", spec.name.toUpperCase() + " LENGTH")
    }

    // ---- The header: lock, name, length ----
    Row {
        id: header
        width: lane.headerWidth - lane.panel.theme.spaceMd
        height: parent.height
        spacing: lane.panel.theme.spaceSm
        Rectangle {
            id: lock
            objectName: "novaArpLock:" + lane.spec.id
            anchors.verticalCenter: parent.verticalCenter
            width: lane.panel.theme.arpLockSize
            height: lane.panel.theme.arpLockSize
            radius: lane.panel.theme.radiusMd
            color: lane.locked ? lane.panel.theme.soft(lane.accent) : "transparent"
            border.color: activeFocus ? lane.accent : lane.locked ? lane.accent : lane.panel.theme.line2
            border.width: activeFocus ? lane.panel.theme.strokeBold : lane.panel.theme.hairline
            activeFocusOnTab: true
            Keys.onSpacePressed: lane.arp.set_lock(lane.spec.id, !lane.locked)
            Keys.onReturnPressed: lane.arp.set_lock(lane.spec.id, !lane.locked)
            Accessible.role: Accessible.CheckBox
            Accessible.name: "Lock " + lane.spec.name
            Accessible.checked: lane.locked
            Text {
                anchors.centerIn: parent
                text: lane.locked ? "●" : "○"
                color: lane.locked ? lane.accent : lane.panel.theme.faint
                font.pixelSize: lane.panel.theme.typeMicro
            }
            MouseArea {
                id: lockArea
                anchors.fill: parent
                anchors.margins: -lane.panel.theme.hitOutset(parent.width)
                hoverEnabled: true
                onClicked: lane.arp.set_lock(lane.spec.id, !lane.locked)
            }
            Kit.Tip {
                objectName: "novaArpLockTip:" + lane.spec.id
                theme: lane.panel.theme
                shown: lockArea.containsMouse
                text: lane.locked ? "Locked" : "Lock " + lane.spec.name
                detail: "A locked lane keeps its steps when a pattern loads or MUTATE runs"
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: header.width - lock.width - lengthBox.width - 2 * header.spacing
            text: lane.spec.name.toUpperCase()
            color: lane.accent
            elide: Text.ElideRight
            font.family: lane.panel.theme.sans
            font.pixelSize: lane.panel.theme.typeCaption
            font.weight: Font.ExtraBold
            font.letterSpacing: lane.panel.theme.arpLaneNameSpacing
        }
        Rectangle {
            id: lengthBox
            objectName: "novaArpLength:" + lane.base + "length"
            anchors.verticalCenter: parent.verticalCenter
            width: lane.panel.theme.arpLengthWidth
            height: lane.panel.theme.arpLengthHeight
            radius: lane.panel.theme.arpLengthRadius
            color: lane.panel.theme.wellFill
            border.color: activeFocus ? lane.accent : lane.panel.theme.line2
            border.width: activeFocus ? lane.panel.theme.strokeBold : lane.panel.theme.hairline
            activeFocusOnTab: lane.controller.editable
            Keys.onUpPressed: lane.setLength(lane.length + 1)
            Keys.onRightPressed: lane.setLength(lane.length + 1)
            Keys.onDownPressed: lane.setLength(lane.length - 1)
            Keys.onLeftPressed: lane.setLength(lane.length - 1)
            Accessible.role: Accessible.SpinBox
            Accessible.name: lane.spec.name + " length"
            Accessible.description: String(lane.length)
            Row {
                anchors.centerIn: parent
                spacing: lane.panel.theme.spaceXs
                Text {
                    objectName: "novaArpShorter:" + lane.spec.id
                    text: "−"
                    color: lane.panel.theme.dim
                    font.pixelSize: lane.panel.theme.typeBody
                    MouseArea {
                        // A least-hit square on the glyph, clear of the other's.
                        anchors.centerIn: parent
                        width: Math.max(parent.width, lane.panel.theme.minHitSize)
                        height: Math.max(parent.height, lane.panel.theme.minHitSize)
                        enabled: lane.controller.editable
                        onClicked: lane.setLength(lane.length - 1)
                    }
                }
                Text {
                    width: lane.panel.theme.arpLengthDigits
                    horizontalAlignment: Text.AlignHCenter
                    text: lane.length
                    color: lane.panel.theme.text
                    font.family: lane.panel.theme.mono
                    font.pixelSize: lane.panel.theme.typeSmall
                }
                Text {
                    objectName: "novaArpLonger:" + lane.spec.id
                    text: "+"
                    color: lane.panel.theme.dim
                    font.pixelSize: lane.panel.theme.typeBody
                    MouseArea {
                        // A least-hit square on the glyph, clear of the other's.
                        anchors.centerIn: parent
                        width: Math.max(parent.width, lane.panel.theme.minHitSize)
                        height: Math.max(parent.height, lane.panel.theme.minHitSize)
                        enabled: lane.controller.editable
                        onClicked: lane.setLength(lane.length + 1)
                    }
                }
            }
            WheelHandler {
                enabled: lane.controller.editable
                // A zero (horizontal or scroll-phase) delta leaves the length alone.
                onWheel: event => {
                    if (event.angleDelta.y !== 0)
                        lane.setLength(lane.length + (event.angleDelta.y > 0 ? 1 : -1))
                }
            }
        }
    }

    // ---- The steps ----
    Item {
        id: cells
        x: lane.headerWidth
        width: lane.width - x
        height: lane.height
        activeFocusOnTab: true
        Accessible.role: Accessible.Table
        Accessible.name: lane.spec.name + " steps"
        Accessible.description: "Step " + (lane.cursor + 1) + ": " + (lane.label(lane.valueAt(lane.cursor)) || String(lane.valueAt(lane.cursor)))
            + ". Left and Right move, Up and Down change, Space flips or steps on, Delete resets"
        Keys.onLeftPressed: lane.cursor = Math.max(0, lane.cursor - 1)
        Keys.onRightPressed: lane.cursor = Math.min(lane.maxSteps - 1, lane.cursor + 1)
        Keys.onUpPressed: lane.keyStep(lane.cursor, 1)
        Keys.onDownPressed: lane.keyStep(lane.cursor, -1)
        Keys.onSpacePressed: lane.keyStep(lane.cursor, 1)
        Keys.onReturnPressed: lane.keyStep(lane.cursor, 1)
        Keys.onDeletePressed: {
            if (!lane.controller.editable)
                return
            lane.controller.set_value(lane.stepId(lane.cursor), lane.spec.fallback)
            lane.touchStep(lane.cursor)
        }
        Repeater {
            model: lane.maxSteps
            delegate: Rectangle {
                id: cell
                required property int index
                readonly property real value: lane.valueAt(index)
                readonly property bool inside: index < lane.length
                readonly property bool here: index === lane.playing
                readonly property bool keyed: cells.activeFocus && index === lane.cursor
                objectName: "novaArpCell:" + lane.stepId(index)
                x: index * (lane.cellWidth + lane.cellGap)
                width: lane.cellWidth
                height: cells.height
                radius: lane.panel.theme.radiusSm
                border.width: keyed ? lane.panel.theme.strokeBold : lane.panel.theme.hairline
                color: lane.panel.theme.wellFill
                border.color: keyed ? lane.accent : here ? lane.panel.theme.am
                    : index % 4 === 0 && inside ? lane.panel.theme.line2 : lane.panel.theme.line
                opacity: inside ? 1 : lane.panel.theme.arpCellOutsideOpacity
                // Levels: a bar from the floor. Numbers: a bar from zero.
                Rectangle {
                    visible: lane.spec.kind === "level" || lane.spec.kind === "integer"
                    readonly property real range: lane.spec.maximum - lane.spec.minimum
                    readonly property real zero: lane.spec.kind === "level" ? 0
                        : Math.max(0, Math.min(1, (0 - lane.spec.minimum) / range))
                    readonly property real level: (cell.value - lane.spec.minimum) / range
                    x: lane.panel.theme.arpBarInsetX
                    width: parent.width - 2 * lane.panel.theme.arpBarInsetX
                    y: (parent.height - 2 * lane.panel.theme.arpBarInsetY) * (1 - Math.max(level, zero)) + lane.panel.theme.arpBarInsetY
                    height: Math.max(lane.spec.kind === "level" && cell.value <= 0 ? 0 : lane.panel.theme.arpBarMinHeight,
                                     (parent.height - 2 * lane.panel.theme.arpBarInsetY) * Math.abs(level - zero))
                    radius: lane.panel.theme.arpBarRadius
                    color: lane.accent
                    opacity: lane.spec.kind === "level" ? lane.panel.theme.arpLevelBarOpacity : lane.panel.theme.arpNumberBarOpacity
                }
                // Toggles: a lit dot.
                Rectangle {
                    visible: lane.spec.kind === "toggle"
                    anchors.centerIn: parent
                    width: Math.min(parent.width, parent.height) * lane.toggleDotShare
                    height: width
                    radius: width / 2
                    color: cell.value >= 0.5 ? lane.accent : "transparent"
                    border.color: cell.value >= 0.5 ? lane.accent : lane.panel.theme.line2
                }
                Text {
                    visible: lane.spec.kind === "integer" || lane.spec.kind === "choice"
                    anchors.centerIn: parent
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: lane.label(cell.value)
                    color: cell.value === lane.spec.fallback ? lane.panel.theme.faint : lane.panel.theme.text
                    elide: Text.ElideRight
                    font.family: lane.panel.theme.mono
                    font.pixelSize: lane.panel.theme.typeMicro
                }
            }
        }
        MouseArea {
            id: paint
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            preventStealing: true
            enabled: lane.controller.editable
            property string gesture: ""
            property real toggleTo: 1
            function paintAt(x, y) {
                const i = lane.cellAt(x)
                if (lane.spec.kind === "toggle")
                    lane.controller.set_value(lane.stepId(i), toggleTo)
                else
                    lane.controller.set_value(lane.stepId(i), lane.valueFromY(y))
                lane.panel.touchId(lane.stepId(i), lane.spec.name.toUpperCase() + " " + (i + 1))
            }
            onPressed: mouse => {
                const i = lane.cellAt(mouse.x)
                lane.cursor = i
                if (lane.spec.kind === "choice") {
                    const count = lane.spec.choices.length
                    const step = mouse.button === Qt.RightButton || (mouse.modifiers & Qt.ShiftModifier) ? -1 : 1
                    lane.controller.set_value(lane.stepId(i), (Math.round(lane.valueAt(i)) + step + count) % count)
                    lane.panel.touchId(lane.stepId(i), lane.spec.name.toUpperCase() + " " + (i + 1))
                    return
                }
                gesture = lane.stepId(i)
                lane.controller.begin_gesture(gesture)
                toggleTo = lane.valueAt(i) >= 0.5 ? 0 : 1
                paintAt(mouse.x, mouse.y)
            }
            onPositionChanged: mouse => {
                if (pressed && gesture !== "")
                    paintAt(mouse.x, mouse.y)
            }
            onReleased: {
                if (gesture !== "")
                    lane.controller.end_gesture(gesture)
                gesture = ""
            }
            onCanceled: {
                if (gesture !== "")
                    lane.controller.cancel_gesture(gesture)
                gesture = ""
            }
            onDoubleClicked: mouse => lane.controller.set_value(lane.stepId(lane.cellAt(mouse.x)), lane.spec.fallback)
        }
    }
}
