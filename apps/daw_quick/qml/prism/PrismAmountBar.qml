pragma ComponentBehavior: Bound
import QtQuick

// Horizontal bipolar amount, filled from the centre. Drag, scroll or arrows.
Rectangle {
    id: bar
    required property var controller
    required property PrismTheme theme
    required property string parameterId
    property color accent: theme.mod
    property bool active: true
    readonly property real position: controller.revision >= 0 ? controller.normalised(parameterId) : 0.5
    property real originPosition: 0
    property real originX: 0

    objectName: "prismAmount:" + parameterId
    implicitWidth: theme.amountWidth
    implicitHeight: theme.amountHeight
    radius: theme.radiusLg
    color: theme.well
    border.color: activeFocus ? accent : theme.line
    activeFocusOnTab: controller.editable
    opacity: active ? 1 : theme.amountInactiveOpacity
    Accessible.role: Accessible.Slider
    Accessible.name: "Amount"
    Accessible.description: controller.display(parameterId)

    Keys.onPressed: event => {
        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Right || event.key === Qt.Key_Up) {
            controller.step(parameterId, 1, fine)
            event.accepted = true
        } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Down) {
            controller.step(parameterId, -1, fine)
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            controller.reset_to_default(parameterId)
            event.accepted = true
        }
    }

    Rectangle {
        readonly property real travel: bar.width - 2 * bar.theme.amountInset
        readonly property real centre: travel / 2 + bar.theme.amountInset
        readonly property real end: bar.theme.amountInset + bar.position * travel
        x: Math.min(centre, end)
        y: bar.theme.amountFillInset
        width: Math.abs(end - centre)
        height: bar.height - 2 * bar.theme.amountFillInset
        radius: bar.theme.radiusSm
        color: Qt.rgba(bar.accent.r, bar.accent.g, bar.accent.b, bar.theme.amountFillAlpha)
    }
    Rectangle {
        x: bar.width / 2
        y: bar.theme.amountFillInset
        width: bar.theme.hairline
        height: bar.height - 2 * bar.theme.amountFillInset
        color: bar.theme.faint
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: bar.theme.amountTextRight
        anchors.verticalCenter: parent.verticalCenter
        text: bar.controller.revision >= 0 ? bar.controller.display(bar.parameterId) : ""
        color: bar.active ? bar.theme.text : bar.theme.dim
        font.pixelSize: bar.theme.typeSmall
        font.bold: true
    }
    MouseArea {
        anchors.fill: parent
        // At least the kit's least hit size, however thin the bar draws.
        anchors.leftMargin: -bar.theme.hitOutset(parent.width)
        anchors.rightMargin: -bar.theme.hitOutset(parent.width)
        anchors.topMargin: -bar.theme.hitOutset(parent.height)
        anchors.bottomMargin: -bar.theme.hitOutset(parent.height)
        enabled: bar.controller.editable
        preventStealing: true
        cursorShape: Qt.SizeHorCursor
        onPressed: mouse => {
            bar.forceActiveFocus()
            bar.originPosition = bar.position
            bar.originX = mouse.x
            bar.controller.begin_gesture(bar.parameterId)
        }
        onPositionChanged: mouse => {
            if (pressed)
                bar.controller.set_normalised(bar.parameterId, Math.max(0, Math.min(1,
                    bar.originPosition + (mouse.x - bar.originX) / bar.width)))
        }
        onReleased: bar.controller.end_gesture(bar.parameterId)
        onCanceled: bar.controller.cancel_gesture(bar.parameterId)
        onDoubleClicked: bar.controller.reset_to_default(bar.parameterId)
        onWheel: wheel => {
            bar.controller.step(bar.parameterId, wheel.angleDelta.y > 0 ? 1 : -1,
                (wheel.modifiers & Qt.ShiftModifier) !== 0)
            wheel.accepted = true
        }
    }
}
