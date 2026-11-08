pragma ComponentBehavior: Bound
import QtQuick

// A vertical fader over a parameter. Drag the cap (Shift for fine), scroll,
// use the arrow keys (Shift for fine), or double-click / Delete to return to
// the default; the theme's faderJump lets a press jump to the pointer. Every
// drag is one gesture, so one undo step. The look comes from the theme's
// fader tokens: the track, a lit bar, the cap and its grip, optional scales,
// a detent, a foot line (the label or the value) and a value bubble.
Item {
    id: fader
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property color accent: theme.faderAccent
    property string label: info.name !== undefined ? info.name : ""
    // Spoken and tooltip name when the label alone is ambiguous ("Kick level").
    property string longName: (theme.faderSpeaksGroup && info.group !== undefined ? info.group + " " : "") + label
    // Dimmed when the control has no effect.
    property bool active: true
    // Mode-inactive (active false, see Knob): the reason it is not in play.
    property string inactiveReason: ""
    readonly property string controlState: !controller.editable ? "disabled"
        : !active ? "inactive" : "normal"
    // A position marked across the track (-1: none), such as unity gain.
    property real detent: -1
    property string tipText: longName + "  " + valueText
    readonly property var info: controller.parameter(parameterId)
    readonly property real position: controller.revision >= 0 ? controller.normalised(parameterId) : 0
    readonly property string valueText: controller.revision >= 0 ? controller.display(parameterId) : ""
    readonly property bool bipolar: info.bipolar === true
    readonly property bool nextNote: info.next_note === true
    readonly property int steps: info.scale === "stepped" ? Math.round(info.maximum - info.minimum) : 0
    readonly property bool editable: controller.editable
    readonly property bool pressed: area.pressed
    readonly property bool showValue: area.pressed || area.containsMouse || activeFocus
    readonly property real capWidth: theme.faderCapWidth > 0 ? theme.faderCapWidth : width - theme.faderCapInset
    readonly property real travel: Math.max(1, height - theme.faderCapHeight - theme.faderTop - theme.faderBottom)
    // The left scale marks each step of a short stepped range.
    readonly property bool stepped: theme.faderStepScale && steps > 0 && steps <= theme.faderStepLimit
    readonly property int leftTicks: stepped ? steps : theme.faderTicks
    property bool dragging: false
    // Shift at the press (a fine drag where faderJump is "always").
    property bool fine: false
    property real dragOrigin: 0
    property real dragStartY: 0

    implicitWidth: theme.faderWidth
    implicitHeight: theme.faderHeight
    objectName: theme.prefix + "Fader:" + parameterId
    activeFocusOnTab: editable
    opacity: !editable ? theme.faderDisabledOpacity : active ? 1 : theme.faderInactiveOpacity
    Accessible.role: Accessible.Slider
    Accessible.name: longName
    Accessible.description: valueText + (theme.knobNextNoteMark && nextNote ? ". Applies to new notes" : "")
        + (!active && inactiveReason !== "" ? ". Inactive: " + inactiveReason : "")
        + ". Drag, scroll or use arrow keys; Delete resets"

    // The y of the cap's centre at a position.
    function centreOf(at) {
        return theme.faderTop + theme.faderCapHeight / 2 + travel * (1 - at)
    }
    // The position whose cap centre lies at a y.
    function at(y) {
        return Math.max(0, Math.min(1, 1 - (y - theme.faderTop - theme.faderCapHeight / 2) / travel))
    }
    function tickY(at) {
        return centreOf(at) - (theme.faderTickCentred ? theme.hairline / 2 : 0)
    }
    function touch() {
        theme.lastTouched = parameterId
    }

    Keys.onPressed: event => {
        if (!editable)
            return
        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Up || (event.key === Qt.Key_Right && theme.faderSideKeys)) {
            controller.step(parameterId, 1, fine)
            touch()
            event.accepted = true
        } else if (event.key === Qt.Key_Down || (event.key === Qt.Key_Left && theme.faderSideKeys)) {
            controller.step(parameterId, -1, fine)
            touch()
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || (event.key === Qt.Key_Backspace && theme.resetOnBackspace)) {
            controller.reset_to_default(parameterId)
            touch()
            event.accepted = true
        }
    }

    Tip {
        theme: fader.theme
        shown: fader.theme.faderTip && area.containsMouse && !area.pressed
        text: fader.tipText
    }

    // The scales: beside the track (or at the fader's edges), the left one
    // marking steps where the theme asks and the right one then hidden.
    Repeater {
        model: fader.theme.faderTicks > 0 ? fader.leftTicks + 1 : 0
        delegate: Rectangle {
            required property int index
            x: fader.theme.faderTickAnchor === "edges" ? fader.theme.faderTickGap
                : track.x - fader.theme.faderTickGap - fader.theme.faderTickMajor
            y: fader.tickY(index / fader.leftTicks)
            width: index % fader.theme.faderTickMajorEvery === 0
                   || (fader.theme.faderStepScale && fader.leftTicks <= fader.theme.faderStepLimit)
                ? fader.theme.faderTickMajor : fader.theme.faderTickMinor
            height: fader.theme.hairline
            color: fader.theme.faderTickInk
        }
    }
    Repeater {
        model: fader.theme.faderTicks > 0 && !fader.stepped ? fader.theme.faderTicks + 1 : 0
        delegate: Rectangle {
            required property int index
            x: fader.theme.faderTickAnchor === "edges" ? fader.width - width - fader.theme.faderTickGap
                : track.x + track.width + fader.theme.faderTickGap
            y: fader.tickY(index / fader.theme.faderTicks)
            width: index % fader.theme.faderTickMajorEvery === 0 ? fader.theme.faderTickMajor : fader.theme.faderTickMinor
            height: fader.theme.hairline
            color: fader.theme.faderTickInk
        }
    }

    Rectangle {
        id: track
        x: (fader.width - width) / 2
        y: fader.theme.faderTrackFull ? fader.theme.faderTop : fader.theme.faderTop + fader.theme.faderCapHeight / 2
        width: fader.theme.faderTrackWidth
        height: fader.theme.faderTrackFull ? fader.travel + fader.theme.faderCapHeight : fader.travel
        radius: fader.theme.faderTrackRadius
        color: fader.theme.faderTrackFill
        border.color: !fader.activeFocus ? fader.theme.faderTrackEdge
            : fader.theme.faderTrackFocusEdge !== undefined ? fader.theme.faderTrackFocusEdge : fader.accent
    }
    // The lit bar: from the range's origin (the centre of a bipolar range)
    // to the value, or from the value to the track's end.
    Rectangle {
        readonly property bool toEnd: fader.theme.faderLit === "end"
        readonly property real origin: fader.bipolar && fader.theme.faderLitFromCentre ? 0.5 : 0
        readonly property real litTop: fader.centreOf(toEnd ? fader.position : Math.max(fader.position, origin))
        readonly property real litBottom: toEnd ? track.y + track.height - fader.theme.faderLitInset
                                             : fader.centreOf(Math.min(fader.position, origin))
        visible: fader.theme.faderLit !== "none"
        x: track.x + (track.width - width) / 2
        y: litTop
        width: fader.theme.faderLitWidth
        height: Math.max(0, litBottom - litTop)
        radius: fader.theme.faderLitRadius
        color: fader.accent
        opacity: fader.theme.faderLitOpacity
    }
    Rectangle {
        visible: fader.detent >= 0
        x: (fader.width - width) / 2
        y: fader.centreOf(fader.detent)
        width: fader.theme.faderDetentWidth
        height: fader.theme.hairline
        color: fader.theme.faderDetentInk
    }
    Rectangle {
        id: cap
        objectName: fader.theme.prefix + "FaderCap"
        x: (fader.width - width) / 2
        y: fader.theme.faderTop + fader.travel * (1 - fader.position)
        width: fader.capWidth
        height: fader.theme.faderCapHeight
        radius: fader.theme.faderCapRadius
        gradient: Gradient {
            GradientStop { position: 0; color: fader.theme.faderCapFill(fader, 0) }
            GradientStop { position: 1; color: fader.theme.faderCapFill(fader, 1) }
        }
        border.color: fader.theme.faderCapEdge(fader)
        Rectangle {
            anchors.centerIn: parent
            width: parent.width - fader.theme.faderGripInset
            height: fader.theme.faderGripHeight
            color: fader.theme.faderGripInk !== undefined ? fader.theme.faderGripInk : fader.accent
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        anchors.bottomMargin: fader.theme.faderAreaBottom
        enabled: fader.editable
        hoverEnabled: true
        preventStealing: true
        cursorShape: fader.theme.faderCursor
        onPressed: mouse => {
            fader.forceActiveFocus()
            fader.touch()
            fader.fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
            fader.dragStartY = mouse.y
            fader.dragging = true
            fader.controller.begin_gesture(fader.parameterId)
            const onCap = mouse.y >= cap.y && mouse.y <= cap.y + cap.height
            const jump = fader.theme.faderJump === "always" ? !fader.fine
                : fader.theme.faderJump === "offCap" ? !onCap : false
            fader.dragOrigin = jump ? fader.at(mouse.y) : fader.position
            if (jump || fader.theme.faderPressSets)
                fader.controller.set_normalised(fader.parameterId, fader.dragOrigin)
        }
        onPositionChanged: mouse => {
            if (!pressed || !fader.dragging)
                return
            const follows = fader.theme.faderJump === "always"
            const fine = follows ? fader.fine : (mouse.modifiers & Qt.ShiftModifier) !== 0
            const moved = fader.dragStartY - mouse.y
            let next = 0
            if (follows && !fine)
                next = fader.at(mouse.y)
            else if (fine && fader.theme.faderFinePixels > 0)
                next = fader.dragOrigin + moved / fader.theme.faderFinePixels
            else
                next = fader.dragOrigin + moved / fader.travel * (fine ? fader.theme.faderFineScale : 1)
            fader.controller.set_normalised(fader.parameterId, Math.max(0, Math.min(1, next)))
        }
        onReleased: {
            if (fader.dragging)
                fader.controller.end_gesture(fader.parameterId)
            fader.dragging = false
        }
        onCanceled: {
            if (fader.dragging)
                fader.controller.cancel_gesture(fader.parameterId)
            fader.dragging = false
        }
        onDoubleClicked: {
            fader.controller.reset_to_default(fader.parameterId)
            fader.touch()
        }
        onWheel: wheel => {
            if (wheel.angleDelta.y !== 0) {
                fader.controller.step(fader.parameterId, wheel.angleDelta.y > 0 ? 1 : -1,
                    (wheel.modifiers & Qt.ShiftModifier) !== 0)
                fader.touch()
            }
            wheel.accepted = true
        }
    }

    // The foot line: the label, or the value (in the accent while dragged).
    Text {
        visible: fader.theme.faderFoot === "label"
        y: fader.height - fader.theme.faderAreaBottom + fader.theme.faderFootGap
        width: parent.width + 2 * fader.theme.faderLabelInset
        x: -fader.theme.faderLabelInset
        horizontalAlignment: Text.AlignHCenter
        text: fader.label
        color: fader.theme.faderLabelInk
        font.pixelSize: fader.theme.faderLabelSize
        font.bold: fader.theme.faderLabelBold
        font.letterSpacing: fader.theme.faderLabelSpacing
        elide: Text.ElideRight
    }
    Text {
        objectName: fader.theme.faderFoot === "value" ? fader.theme.prefix + "FaderValue:" + fader.parameterId : ""
        visible: fader.theme.faderFoot === "value"
        anchors.horizontalCenter: parent.horizontalCenter
        y: fader.height - fader.theme.faderAreaBottom + fader.theme.faderFootGap
        text: fader.valueText.replace(fader.theme.faderValueSuffix, "")
        color: fader.dragging ? fader.accent : fader.theme.faderValueInk
        font.family: fader.theme.mono
        font.pixelSize: fader.theme.faderValueSize
    }

    // The value over the cap while hovered, dragged or focused.
    Rectangle {
        visible: fader.theme.faderBubble && fader.showValue
        z: 1
        x: (parent.width - width) / 2
        y: cap.y - height - fader.theme.faderBubbleGap
        width: bubble.implicitWidth + fader.theme.faderBubblePad
        height: fader.theme.faderBubbleHeight
        radius: fader.theme.faderBubbleRadius
        color: fader.theme.faderBubbleFill
        border.color: fader.accent
        Text {
            id: bubble
            anchors.centerIn: parent
            text: fader.valueText
            color: fader.theme.faderBubbleInk
            font.pixelSize: fader.theme.faderBubbleSize
            font.bold: true
        }
    }

    // Parameters that apply to new notes carry the knobs' small hollow mark.
    Rectangle {
        visible: fader.theme.knobNextNoteMark && fader.nextNote
        x: fader.width - fader.theme.knobNextNoteInset
        width: fader.theme.knobNextNoteSize
        height: fader.theme.knobNextNoteSize
        radius: fader.theme.knobNextNoteRound ? width / 2 : 0
        rotation: fader.theme.knobNextNoteRound ? 0 : fader.theme.knobNextNoteTurn
        color: "transparent"
        border.color: fader.theme.knobNextNoteInk
        border.width: fader.theme.hairline
    }
}
