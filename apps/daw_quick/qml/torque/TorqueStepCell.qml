pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts
import Oscillate.Format
import "../devicekit" as Kit

// One sequencer step: its note (an interval from the held root, drawn as a
// note name from C), octave, gate (off, on or tie), accent and slide. The
// cell takes focus: Up/Down change the note (Shift: octave), Return or Space
// cycle the gate, A toggles accent, S toggles slide, Delete resets the step,
// Left/Right move between steps.
Rectangle {
    id: cell
    required property var controller
    required property TorqueTheme theme
    required property int step
    // Steps past the pattern length are dimmed; they keep their values.
    property bool inPattern: true
    // The live playhead: this step is sounding, and whether its gate is open.
    property bool playing: false
    property bool gateOpen: false
    readonly property string base: "torque.seq.s" + step + "."
    readonly property int note: controller.revision >= 0 ? Math.round(controller.value(base + "note")) : 0
    readonly property int octave: controller.revision >= 0 ? Math.round(controller.value(base + "octave")) : 0
    readonly property int gate: controller.revision >= 0 ? controller.choice(base + "gate") : 1
    readonly property bool accented: controller.revision >= 0 && controller.choice(base + "accent") === 1
    readonly property bool sliding: controller.revision >= 0 && controller.choice(base + "slide") === 1
    readonly property bool editable: controller.editable
    readonly property string noteName: Contracts.pitch_class_name(note, Contracts.sharp)
        + (octave > 0 ? "↑" : octave < 0 ? "↓" : "")
    property real dragStartY: 0
    property int dragOrigin: 0
    // Drag distance per note step, coarse and fine (Shift).
    readonly property real dragPixelsPerStep: 9
    readonly property real fineDragPixelsPerStep: 24
    // Accent and slide share a row with a narrow gap between them.
    readonly property real flagWidth: (width - 2 * theme.stepInset - theme.spaceXxs) / 2

    objectName: "torqueStep:" + step
    radius: theme.stepRadius
    color: activeFocus ? theme.buttonHi : theme.button
    border.color: playing || activeFocus ? theme.accent : gate === 0 ? theme.wellEdge
        : Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, theme.stepEdgeAlpha)
    border.width: playing ? theme.strokeBold : theme.hairline
    opacity: !editable ? theme.stepDisabledOpacity : inPattern ? 1 : theme.stepOutsideOpacity
    activeFocusOnTab: editable
    Accessible.role: Accessible.Grouping
    Accessible.name: "Step " + step + (playing ? ", playing" : "")
    Accessible.description: (gate === 0 ? "rest" : gate === 2 ? "tie" : noteName + " " + Format.signed_number(note, 0))
        + (accented ? ", accent" : "") + (sliding ? ", slide" : "")

    function touch(id) {
        theme.lastTouched = base + id
    }
    function setNote(value) {
        controller.set_value(base + "note", Math.max(-12, Math.min(12, value)))
        touch("note")
    }
    function setOctave(value) {
        controller.set_value(base + "octave", Math.max(-1, Math.min(1, value)))
        touch("octave")
    }
    function cycleGate() {
        controller.set_choice(base + "gate", (gate + 1) % 3)
        touch("gate")
    }
    function toggle(id) {
        controller.set_choice(base + id, controller.choice(base + id) === 1 ? 0 : 1)
        touch(id)
    }
    function resetStep() {
        for (const id of ["note", "octave", "gate", "accent", "slide"])
            controller.reset_to_default(base + id)
        touch("note")
    }

    Keys.onPressed: event => {
        if (!editable)
            return
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Up) {
            shift ? setOctave(octave + 1) : setNote(note + 1)
        } else if (event.key === Qt.Key_Down) {
            shift ? setOctave(octave - 1) : setNote(note - 1)
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            cycleGate()
        } else if (event.key === Qt.Key_A) {
            toggle("accent")
        } else if (event.key === Qt.Key_S) {
            toggle("slide")
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            resetStep()
        } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            const next = cell.nextItemInFocusChain(event.key === Qt.Key_Right)
            if (next && next.objectName.startsWith("torqueStep:"))
                next.forceActiveFocus()
        } else {
            return
        }
        event.accepted = true
    }

    // The playhead: a lit bar along the top, brighter while the gate is open.
    Rectangle {
        objectName: "torquePlayhead:" + cell.step
        visible: cell.playing
        x: cell.theme.playheadInset
        y: cell.theme.spaceXxs
        width: cell.width - 2 * cell.theme.playheadInset
        height: cell.theme.playheadHeight
        radius: height / 2
        color: cell.theme.accent
        opacity: cell.gateOpen ? 1 : cell.theme.playheadClosedOpacity
    }
    Rectangle {
        anchors.fill: parent
        radius: cell.radius
        visible: cell.playing
        color: Qt.rgba(cell.theme.accent.r, cell.theme.accent.g, cell.theme.accent.b,
                       cell.gateOpen ? cell.theme.playWashOpenAlpha : cell.theme.playWashClosedAlpha)
    }

    // Step number and gate LED.
    Text {
        x: cell.theme.stepEdgeInset
        y: cell.theme.stepNumberY
        text: String(cell.step)
        color: cell.theme.inkDim
        font.family: cell.theme.mono
        font.pixelSize: cell.theme.typeSmall
        font.bold: true
    }
    Rectangle {
        anchors.right: parent.right
        anchors.rightMargin: cell.theme.stepEdgeInset
        y: cell.theme.stepLedY
        width: cell.theme.stepLedSize
        height: cell.theme.stepLedSize
        radius: width / 2
        color: cell.gate === 0 ? cell.theme.ledOff : cell.theme.accent
        opacity: cell.gate === 2 ? cell.theme.tieLedOpacity : 1
    }

    // Note: drag vertically or scroll; double-click resets.
    Rectangle {
        id: noteWell
        objectName: "torqueStepControl:" + cell.base + "note"
        x: cell.theme.stepInset
        y: cell.theme.noteWellY
        width: cell.width - 2 * cell.theme.stepInset
        height: cell.theme.noteWellHeight
        radius: cell.theme.radiusMd
        color: cell.theme.well
        border.color: cell.theme.wellEdge
        Accessible.role: Accessible.SpinBox
        Accessible.name: "Step " + cell.step + " note"
        Accessible.description: cell.noteName
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: cell.theme.noteNameY
            text: cell.gate === 0 ? "—" : cell.gate === 2 ? "TIE" : cell.noteName
            color: cell.gate === 1 ? cell.theme.ink : cell.theme.inkFaint
            font.pixelSize: cell.theme.typeTitle
            font.bold: true
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: cell.theme.intervalY
            // The interval from the held root, octave included.
            readonly property int semitones: cell.note + 12 * cell.octave
            text: semitones === 0 ? "ROOT" : Format.signed_number(semitones, 0)
            color: cell.theme.accent
            opacity: cell.gate === 1 ? 1 : cell.theme.intervalRestOpacity
            font.family: cell.theme.mono
            font.pixelSize: cell.theme.typeSmall
        }
        MouseArea {
            anchors.fill: parent
            enabled: cell.editable
            preventStealing: true
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => {
                cell.forceActiveFocus()
                cell.dragStartY = mouse.y
                cell.dragOrigin = cell.note
                cell.controller.begin_gesture(cell.base + "note")
                cell.touch("note")
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
                const steps = Math.round((cell.dragStartY - mouse.y) / (fine ? cell.fineDragPixelsPerStep : cell.dragPixelsPerStep))
                const value = Math.max(-12, Math.min(12, cell.dragOrigin + steps))
                if (value !== cell.note)
                    cell.controller.set_value(cell.base + "note", value)
            }
            onReleased: cell.controller.end_gesture(cell.base + "note")
            onCanceled: cell.controller.cancel_gesture(cell.base + "note")
            onDoubleClicked: {
                cell.controller.reset_to_default(cell.base + "note")
                cell.touch("note")
            }
            onWheel: wheel => {
                if (wheel.angleDelta.y !== 0)
                    cell.setNote(cell.note + (wheel.angleDelta.y > 0 ? 1 : -1))
                wheel.accepted = true
            }
        }
    }

    // Octave: down, centre, up.
    Row {
        id: octaves
        objectName: "torqueStepControl:" + cell.base + "octave"
        x: cell.theme.stepInset
        y: noteWell.y + noteWell.height + cell.theme.stepRowGap
        spacing: cell.theme.spaceXxs
        Accessible.role: Accessible.Grouping
        Accessible.name: "Step " + cell.step + " octave"
        Repeater {
            model: [-1, 0, 1]
            delegate: Rectangle {
                id: octaveKey
                required property int modelData
                readonly property bool on: cell.octave === modelData
                width: (cell.width - 2 * cell.theme.stepInset - 2 * octaves.spacing) / 3
                height: cell.theme.octaveKeyHeight
                radius: cell.theme.radiusSm
                color: on ? Qt.rgba(cell.theme.accent.r, cell.theme.accent.g, cell.theme.accent.b, cell.theme.octaveOnAlpha)
                          : cell.theme.well
                border.color: on ? cell.theme.accent : cell.theme.wellEdge
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData < 0 ? "Octave down" : modelData > 0 ? "Octave up" : "No octave"
                Accessible.checked: on
                Text {
                    anchors.centerIn: parent
                    text: octaveKey.modelData < 0 ? "−" : octaveKey.modelData > 0 ? "+" : "0"
                    color: octaveKey.on ? cell.theme.accent : cell.theme.inkFaint
                    font.pixelSize: cell.theme.typeBody
                    font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: cell.editable
                    onClicked: {
                        cell.forceActiveFocus()
                        cell.setOctave(octaveKey.modelData)
                    }
                }
            }
        }
    }

    component Toggle: Rectangle {
        id: toggle
        required property string parameter
        required property string label
        property bool on: false
        property bool outline: false
        height: cell.theme.stepToggleHeight
        radius: cell.theme.radiusSm
        color: on && !outline ? cell.theme.accent : cell.theme.well
        border.color: on ? cell.theme.accent : cell.theme.wellEdge
        Accessible.role: Accessible.CheckBox
        Kit.HoverTip { theme: cell.theme }
        Accessible.name: "Step " + cell.step + " " + label
        Accessible.checked: on
        Text {
            anchors.centerIn: parent
            text: toggle.label
            color: toggle.on ? (toggle.outline ? cell.theme.accent : cell.theme.body) : cell.theme.inkFaint
            font.pixelSize: cell.theme.typeCaption
            font.bold: true
            font.letterSpacing: cell.theme.stepToggleSpacing
        }
        MouseArea {
            anchors.fill: parent
            enabled: cell.editable
            onClicked: {
                cell.forceActiveFocus()
                if (toggle.parameter === "gate")
                    cell.cycleGate()
                else
                    cell.toggle(toggle.parameter)
            }
        }
    }

    Toggle {
        objectName: "torqueStepControl:" + cell.base + "gate"
        x: cell.theme.stepInset
        y: octaves.y + cell.theme.stepGateOffset
        width: cell.width - 2 * cell.theme.stepInset
        parameter: "gate"
        label: cell.gate === 0 ? "REST" : cell.gate === 2 ? "TIE" : "GATE"
        on: cell.gate !== 0
        outline: cell.gate === 2
    }
    Toggle {
        objectName: "torqueStepControl:" + cell.base + "accent"
        x: cell.theme.stepInset
        y: octaves.y + cell.theme.stepFlagsOffset
        width: cell.flagWidth
        parameter: "accent"
        label: "ACC"
        on: cell.accented
    }
    Toggle {
        objectName: "torqueStepControl:" + cell.base + "slide"
        x: cell.theme.stepInset + cell.theme.spaceXxs + cell.flagWidth
        y: octaves.y + cell.theme.stepFlagsOffset
        width: cell.flagWidth
        parameter: "slide"
        label: "SLD"
        on: cell.sliding
    }
    // A slide links this step to the next.
    Rectangle {
        visible: cell.sliding && cell.gate !== 0
        anchors.left: parent.right
        anchors.verticalCenter: noteWell.verticalCenter
        width: cell.theme.slideLinkWidth
        height: cell.theme.strokeBold
        color: cell.theme.accent
    }
}
