pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// A small pill button. Bound to an [Off, On] choice parameter it toggles it;
// otherwise it is lit by `on` and reports `clicked`.
Rectangle {
    id: pill
    required property var panel
    property string text: ""
    property string parameterId: ""
    // The choice that lights a bound pill, and the one a click away from it picks.
    property int onIndex: 1
    property int offIndex: 0
    property bool localOn: false
    readonly property bool on: parameterId !== ""
        ? panel.controller.revision >= 0 && panel.controller.choice(parameterId) === onIndex
        : localOn
    property color accent: parameterId !== "" ? panel.accentFor(parameterId) : panel.theme.cy
    property real fontSize: panel.theme.typeSmall
    signal clicked()

    objectName: "novaPill:" + (parameterId !== "" ? parameterId : text)
    implicitWidth: label.implicitWidth + panel.theme.pillPad
    implicitHeight: panel.theme.pillHeight
    radius: panel.theme.radiusLg
    color: on ? panel.theme.soft(accent) : area.containsMouse ? panel.theme.hover : panel.theme.wellFill
    border.color: on ? accent : panel.theme.line2
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Kit.HoverTip { theme: pill.panel.theme }
    Accessible.name: text
    Accessible.checked: on
    function activate() {
        if (parameterId !== "") {
            if (!panel.controller.editable)
                return
            panel.controller.set_choice(parameterId, on ? offIndex : onIndex)
            panel.touchId(parameterId, pill.text)
        }
        pill.clicked()
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()

    Text {
        id: label
        anchors.centerIn: parent
        text: pill.text
        color: pill.on ? pill.accent : pill.panel.theme.dim
        font.family: pill.panel.theme.sans
        font.pixelSize: pill.fontSize
        font.weight: Font.Bold
        font.letterSpacing: pill.panel.theme.pillLetterSpacing
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: pill.activate()
    }
}
