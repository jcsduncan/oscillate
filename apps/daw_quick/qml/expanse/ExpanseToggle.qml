import QtQuick
import "../devicekit" as Kit

// A large on/off pill for a two-way choice (Freeze): frosted ice when on.
// Click, Space or Return toggles; Delete resets.
Rectangle {
    id: toggle
    required property var controller
    required property ExpanseTheme theme
    required property string parameterId
    property string glyph: "❄"
    property string onCaption: ""
    property string offCaption: ""
    readonly property var info: controller.parameter(parameterId)
    readonly property bool on: controller.revision >= 0 && controller.choice(parameterId) === 1

    objectName: "expanseToggle:" + parameterId
    implicitWidth: theme.toggleWidth
    implicitHeight: theme.toggleHeight
    radius: height / 2
    activeFocusOnTab: controller.editable
    opacity: controller.editable ? 1 : theme.switchDisabledOpacity
    gradient: Gradient {
        orientation: Gradient.Horizontal
        GradientStop {
            position: 0
            color: toggle.on ? toggle.theme.toggleOnStart
                             : toggleArea.pressed ? toggle.theme.button : toggle.theme.buttonHi
        }
        GradientStop {
            position: 1
            color: toggle.on ? toggle.theme.toggleOnEnd : toggle.theme.button
        }
    }
    border.color: activeFocus ? theme.aurora : on ? theme.ice : toggleArea.containsMouse ? theme.wellEdge
                                                                                         : theme.seam
    border.width: on || activeFocus ? theme.focusStroke : theme.hairline
    Accessible.role: Accessible.CheckBox
    Kit.HoverTip { theme: toggle.theme }
    Accessible.name: info.name !== undefined ? info.name : ""
    Accessible.checked: on

    function flip() {
        theme.lastTouched = parameterId
        controller.set_choice(parameterId, on ? 0 : 1)
    }
    Keys.onSpacePressed: flip()
    Keys.onReturnPressed: flip()
    Keys.onDeletePressed: { controller.reset_to_default(parameterId); theme.lastTouched = parameterId }

    // A halo of frost while it holds.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -toggle.theme.frostHaloSpread
        radius: height / 2
        color: "transparent"
        border.width: toggle.theme.frostHaloWidth
        border.color: toggle.theme.frostHalo
        visible: toggle.on
    }
    Text {
        id: glyphText
        x: toggle.theme.toggleGlyphX
        anchors.verticalCenter: parent.verticalCenter
        text: toggle.glyph
        color: toggle.on ? toggle.theme.ice : toggle.theme.inkFaint
        font.pixelSize: toggle.theme.toggleGlyphSize
    }
    Column {
        x: glyphText.x + glyphText.width + toggle.theme.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        spacing: toggle.theme.spaceXxs
        Text {
            text: (toggle.info.name !== undefined ? toggle.info.name : "").toUpperCase()
            color: toggle.on ? toggle.theme.ink : toggle.theme.inkDim
            font.family: toggle.theme.sans
            font.pixelSize: toggle.theme.typeEmphasis
            font.bold: true
            font.letterSpacing: toggle.theme.toggleLabelSpacing
        }
        Text {
            text: toggle.on ? toggle.onCaption : toggle.offCaption
            color: toggle.on ? toggle.theme.ice : toggle.theme.inkFaint
            font.pixelSize: toggle.theme.typeMicro
            font.letterSpacing: toggle.theme.toggleCaptionSpacing
        }
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
