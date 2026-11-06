pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// A labelled numeric control. Arrow keys step and commit; Page Up/Down take
// larger steps; dragging previews locally and commits once on release, so
// one gesture is one undo step. Inactive values stay visible but are labelled.
Item {
    id: stepper
    required property string label
    required property real value
    required property real minimum
    required property real maximum
    property real step: 1
    property int decimals: 0
    property string unit: ""
    property bool active: true
    property string inactiveNote: ""
    property bool editable: true
    property color textMain: Theme.warpTextMain
    property color textDim: Theme.warpTextDim
    property color accent: Theme.warpAccent
    property color lineColor: Theme.warpLine
    property real draft: value
    property bool dragging: false
    signal committed(real value)

    implicitHeight: Theme.warpStepperHeight
    onValueChanged: if (!dragging) draft = value

    function clamp(v) { return Math.max(minimum, Math.min(maximum, v)) }
    function commit(v) {
        const next = clamp(Math.round(v / step) * step)
        draft = next
        if (Math.abs(next - value) > 1e-9) committed(next)
    }
    function shown(v) { return v.toFixed(decimals) + (unit.length > 0 ? " " + unit : "") }

    Text {
        id: caption
        x: 0; y: 0
        width: Theme.warpLabelWidth
        text: stepper.label
        color: stepper.active ? stepper.textMain : stepper.textDim
        font.pixelSize: Theme.typeSmall
        elide: Text.ElideRight
    }
    Text {
        x: 0; y: Theme.warpStepperNoteY
        width: Theme.warpLabelWidth
        visible: !stepper.active && stepper.inactiveNote.length > 0
        text: stepper.inactiveNote
        color: stepper.textDim
        font.pixelSize: Theme.typeMicro
        font.italic: true
        elide: Text.ElideRight
    }
    Rectangle {
        id: track
        objectName: "warpStepper" + stepper.label.replace(/[^A-Za-z]/g, "")
        x: Theme.warpStepperTrackX; y: Theme.warpStepperTrackY
        width: Math.max(Theme.warpStepperTrackMinWidth, stepper.width - Theme.warpStepperTrackX - Theme.warpStepperValueReserve)
        height: Theme.warpStepperTrackHeight
        radius: height / 2
        color: Theme.warpControlFill
        border.color: activeFocus ? stepper.accent : stepper.lineColor
        opacity: stepper.active ? 1 : Theme.warpUnavailableOpacity
        enabled: stepper.editable
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Slider
        Accessible.name: stepper.label
        Accessible.description: stepper.shown(stepper.value)
                                + (stepper.active ? "" : ". Inactive: " + stepper.inactiveNote)
                                + ". Arrow keys step, Page Up and Page Down step by ten"
        Keys.onLeftPressed: stepper.commit(stepper.value - stepper.step)
        Keys.onRightPressed: stepper.commit(stepper.value + stepper.step)
        Keys.onDownPressed: stepper.commit(stepper.value - stepper.step)
        Keys.onUpPressed: stepper.commit(stepper.value + stepper.step)
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_PageUp) { stepper.commit(stepper.value + 10 * stepper.step); event.accepted = true }
            else if (event.key === Qt.Key_PageDown) { stepper.commit(stepper.value - 10 * stepper.step); event.accepted = true }
        }
        Rectangle {
            width: Math.max(Theme.warpStepperFillMinWidth, track.width * (stepper.draft - stepper.minimum) / Math.max(1e-9, stepper.maximum - stepper.minimum))
            height: parent.height
            radius: parent.radius
            color: stepper.active ? stepper.accent : stepper.textDim
        }
        MouseArea {
            anchors.fill: parent
            enabled: track.enabled
            function at(x) {
                return stepper.minimum + Math.max(0, Math.min(1, x / track.width)) * (stepper.maximum - stepper.minimum)
            }
            onPressed: (mouse) => { stepper.dragging = true; track.forceActiveFocus(); stepper.draft = stepper.clamp(at(mouse.x)) }
            onPositionChanged: (mouse) => { if (pressed) stepper.draft = stepper.clamp(at(mouse.x)) }
            onReleased: { stepper.dragging = false; stepper.commit(stepper.draft) }
            onCanceled: { stepper.dragging = false; stepper.draft = stepper.value }
        }
    }
    Text {
        x: stepper.width - Theme.warpStepperValueWidth; y: Theme.warpStepperValueY
        width: Theme.warpStepperValueWidth
        horizontalAlignment: Text.AlignRight
        text: stepper.shown(stepper.draft)
        color: stepper.active ? stepper.textMain : stepper.textDim
        font.pixelSize: Theme.typeSmall
    }
}
