import QtQuick

// A step button beside the preset browser: the previous (direction -1) or
// next (+1) preset. Click, Return or Space steps; a panel may step its own
// way (a filtered browser list) by setting `stepper(direction)`.
Rectangle {
    id: arrow
    required property var controller
    required property KitTheme theme
    required property int direction
    readonly property bool pressed: arrowArea.pressed
    readonly property bool hovered: arrowArea.containsMouse
    property var stepper: null
    function stepNow() {
        if (stepper)
            stepper(direction)
        else
            controller.step_preset(direction)
    }

    width: theme.arrowWidth
    height: theme.arrowHeight
    radius: theme.arrowRadius
    gradient: Gradient {
        GradientStop { position: 0; color: arrow.theme.arrowFill(arrow, 0) }
        GradientStop { position: 1; color: arrow.theme.arrowFill(arrow, 1) }
    }
    border.color: theme.arrowEdge(arrow)
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: (direction < 0 ? "Previous " : "Next ") + theme.arrowNoun
    Keys.onReturnPressed: stepNow()
    Keys.onSpacePressed: stepNow()
    Text {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: arrow.theme.arrowGlyphOffset
        text: arrow.direction < 0 ? arrow.theme.arrowPrevious : arrow.theme.arrowNext
        color: arrow.theme.arrowInk
        font.pixelSize: arrow.theme.arrowGlyphSize
    }
    Tip {
        theme: arrow.theme
        shown: arrowArea.containsMouse && !arrowArea.pressed
        text: arrow.Accessible.name
    }
    MouseArea {
        id: arrowArea
        anchors.fill: parent
        enabled: arrow.controller.editable
        hoverEnabled: true
        onClicked: arrow.stepNow()
    }
}
