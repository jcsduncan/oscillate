import QtQuick
import "MacroPalette.js" as MacroPalette

// A switch for a two-way choice: an LED that lights in its colour when on,
// the name, and a caption for each state. Click, Space or Return toggles;
// Delete resets. Each flip is one undo step.
Rectangle {
    id: toggle
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property string label: info.name !== undefined ? info.name : ""
    property string onCaption: ""
    property string offCaption: ""
    property color lit: theme.accent
    property string hint: "Click, Space or Return toggles · Delete resets"
    readonly property var info: controller.parameter(parameterId)
    readonly property bool on: controller.revision >= 0 && controller.choice(parameterId) === 1

    objectName: theme.prefix + "Toggle:" + parameterId
    implicitWidth: theme.toggleTileWidth
    implicitHeight: theme.toggleTileHeight
    radius: theme.radiusLg
    activeFocusOnTab: controller.editable
    opacity: controller.editable ? 1 : theme.toggleTileDisabledOpacity
    gradient: Gradient {
        GradientStop { position: 0; color: toggleArea.pressed ? toggle.theme.button : toggle.theme.buttonHi }
        GradientStop { position: 1; color: toggle.theme.button }
    }
    border.color: activeFocus ? theme.accent
        : on ? Qt.rgba(lit.r, lit.g, lit.b, theme.toggleTileLitEdgeAlpha) : toggleArea.containsMouse ? theme.wellEdge : theme.seam
    border.width: activeFocus ? theme.toggleTileFocusEdgeWidth : theme.hairline
    Accessible.role: Accessible.CheckBox
    Accessible.name: label
    Accessible.checked: on

    function flip() {
        theme.lastTouched = parameterId
        controller.set_choice(parameterId, on ? 0 : 1)
    }
    Keys.onSpacePressed: flip()
    Keys.onReturnPressed: flip()
    Keys.onDeletePressed: { controller.reset_to_default(parameterId); theme.lastTouched = parameterId }

    // The LED, with a soft bloom when lit.
    Rectangle {
        id: led
        x: toggle.theme.toggleTileLedX
        anchors.verticalCenter: parent.verticalCenter
        width: toggle.theme.toggleTileLedSize
        height: width
        radius: width / 2
        color: toggle.on ? toggle.lit : toggle.theme.well
        border.color: toggle.on ? Qt.lighter(toggle.lit, toggle.theme.toggleTileLedLift) : toggle.theme.wellEdge
        Rectangle {
            anchors.centerIn: parent
            width: toggle.theme.toggleTileBloomSize
            height: width
            radius: width / 2
            color: Qt.rgba(toggle.lit.r, toggle.lit.g, toggle.lit.b, toggle.theme.toggleTileBloomAlpha)
            visible: toggle.on
        }
    }
    Column {
        x: led.x + led.width + toggle.theme.toggleTileTextGap
        anchors.verticalCenter: parent.verticalCenter
        width: toggle.width - x - toggle.theme.toggleTileTextRight
        spacing: toggle.theme.toggleTileCaptionGap
        Text {
            width: parent.width
            text: toggle.label.toUpperCase()
            color: toggle.on ? toggle.theme.ink : toggle.theme.inkDim
            font.family: toggle.theme.sans
            font.pixelSize: toggle.theme.typeSmall
            font.bold: true
            font.letterSpacing: toggle.theme.toggleTileNameSpacing
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: text !== ""
            text: toggle.on ? toggle.onCaption : toggle.offCaption
            color: toggle.on ? toggle.lit : toggle.theme.inkFaint
            font.pixelSize: toggle.theme.typeMicro
            font.letterSpacing: toggle.theme.toggleTileCaptionSpacing
            elide: Text.ElideRight
        }
    }
    Tip {
        theme: toggle.theme
        shown: toggleArea.containsMouse && !toggleArea.pressed
        text: toggle.label + "  " + (toggle.on ? (toggle.onCaption || "On") : (toggle.offCaption || "Off"))
        detail: MacroPalette.tipLine(toggle.controller, toggle.parameterId) + toggle.hint
    }
    MouseArea {
        id: toggleArea
        anchors.fill: parent
        enabled: toggle.controller.editable
        hoverEnabled: true
        onClicked: {
            toggle.forceActiveFocus()
            toggle.flip()
        }
    }
}
