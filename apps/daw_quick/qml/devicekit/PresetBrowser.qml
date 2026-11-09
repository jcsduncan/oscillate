import QtQuick

// The header's preset browser: ◂ [ CATEGORY · EDITED / Preset name ▾ ] ▸.
// Click, Return or Space on the display opens the preset list (`menu`'s
// grouped list, or the panel's own `opener`); the arrows step. The control
// last touched is not shown here but in the header's Readout.
Row {
    id: browser
    required property var controller
    required property KitTheme theme
    // The kit Menu that lists the presets, or a panel's own way to open its
    // list: opener(display).
    property var menu: null
    property var opener: null
    // A panel may step its own way (a filtered list): stepper(direction).
    property var stepper: null
    property string tag: controller.preset_category.toUpperCase()
        + (controller.preset_modified ? " · EDITED" : "")
    property string name: controller.preset_name
    property real displayWidth: theme.presetDisplayWidth
    readonly property alias display: display

    function openList() {
        if (!controller.editable)
            return
        if (opener)
            opener(display)
        else if (menu)
            menu.openPresets(display, controller, theme.accent)
    }

    spacing: theme.presetArrowGap
    PresetArrow {
        objectName: browser.theme.prefix + "PresetPrevious"
        controller: browser.controller
        theme: browser.theme
        direction: -1
        stepper: browser.stepper
        anchors.verticalCenter: parent.verticalCenter
    }
    Rectangle {
        id: display
        objectName: browser.theme.prefix + "PresetDisplay"
        width: browser.displayWidth
        height: browser.theme.presetDisplayHeight
        radius: browser.theme.presetDisplayRadius
        color: browser.theme.presetFill
        border.color: activeFocus ? browser.theme.accent : browser.theme.presetEdge
        border.width: activeFocus ? browser.theme.presetFocusStroke : browser.theme.hairline
        activeFocusOnTab: true
        Accessible.role: Accessible.ComboBox
        Accessible.name: "Preset"
        Accessible.description: browser.name + (browser.controller.preset_modified ? ", edited" : "")
        Keys.onReturnPressed: browser.openList()
        Keys.onSpacePressed: browser.openList()
        Text {
            objectName: browser.theme.prefix + "PresetCategory"
            x: browser.theme.presetTextX
            y: browser.theme.presetTagY
            width: chevron.x - x - browser.theme.presetChevronMargin
            text: browser.tag
            color: browser.theme.presetTagInk
            font.pixelSize: browser.theme.presetTagSize
            font.bold: true
            font.letterSpacing: browser.theme.presetTagSpacing
            elide: Text.ElideRight
        }
        Text {
            objectName: browser.theme.prefix + "PresetName"
            // A user's preset name may be any length: elided, whole in the tip.
            readonly property bool tipShowsWhole: true
            x: browser.theme.presetTextX
            y: browser.theme.presetNameY
            width: chevron.x - x - browser.theme.presetChevronMargin
            text: browser.name
            color: browser.theme.presetNameInk
            font.family: browser.theme.presetNameFont !== "" ? browser.theme.presetNameFont : Qt.application.font.family
            font.pixelSize: browser.theme.presetNameSize
            font.bold: browser.theme.presetNameBold
            font.letterSpacing: browser.theme.presetNameSpacing
            elide: Text.ElideRight
        }
        Text {
            id: chevron
            anchors.right: parent.right
            anchors.rightMargin: browser.theme.presetChevronMargin
            anchors.verticalCenter: parent.verticalCenter
            text: browser.theme.presetChevron
            color: browser.theme.presetChevronInk
            font.pixelSize: browser.theme.presetChevronSize
        }
        Tip {
            theme: browser.theme
            shown: displayArea.containsMouse && !displayArea.pressed
            text: browser.name
            detail: "Click for the preset list"
        }
        MouseArea {
            id: displayArea
            anchors.fill: parent
            enabled: browser.controller.editable
            hoverEnabled: true
            onClicked: {
                display.forceActiveFocus()
                browser.openList()
            }
        }
    }
    PresetArrow {
        objectName: browser.theme.prefix + "PresetNext"
        controller: browser.controller
        theme: browser.theme
        direction: 1
        stepper: browser.stepper
        anchors.verticalCenter: parent.verticalCenter
    }
}
