import QtQuick
import "MacroPalette.js" as MacroPalette

// A choice shown as a readout that opens the panel's shared menu. Up and
// Down step, Return or Space opens, Delete resets; each pick is one undo
// step.
Rectangle {
    id: dropdown
    required property var controller
    required property KitTheme theme
    required property string parameterId
    required property Menu menu
    property color accent: theme.dropdownAccent
    // Mode-inactive (see Knob): still editable, with the reason given.
    property bool inactive: false
    property string inactiveReason: ""
    readonly property bool modeInactive: inactive
    readonly property string controlState: !controller.editable ? "disabled"
        : inactive ? "inactive" : "normal"
    property int fontSize: theme.dropdownTextSize
    readonly property var info: controller.parameter(parameterId)
    readonly property var choices: info.choices !== undefined ? info.choices : []
    readonly property int current: controller.revision >= 0 ? controller.choice(parameterId) : 0
    readonly property bool nextNote: info.next_note === true
    readonly property bool hovered: area.containsMouse

    // The width that shows the longest choice whole beside the chevron:
    // what a dropdown is sized from.
    readonly property real fitWidth: choices.reduce((widest, choice) =>
        Math.max(widest, choiceMetrics.advanceWidth(String(choice))), 0)
        + theme.dropdownTextX + theme.dropdownTextRight
    FontMetrics {
        id: choiceMetrics
        font.family: dropdown.theme.dropdownFont
        font.pixelSize: dropdown.fontSize
        font.bold: dropdown.theme.dropdownBold
    }

    objectName: theme.prefix + "Dropdown:" + parameterId
    implicitWidth: Math.max(theme.dropdownWidth, Math.ceil(fitWidth))
    implicitHeight: theme.dropdownHeight
    radius: theme.dropdownRadius
    color: theme.dropdownFill(dropdown)
    border.color: activeFocus ? accent : theme.dropdownEdge
    opacity: !controller.editable ? theme.dropdownDisabledOpacity : inactive ? theme.dropdownInactiveOpacity : 1
    activeFocusOnTab: controller.editable
    Accessible.role: Accessible.ComboBox
    Accessible.name: (theme.dropdownSpeaksGroup && info.group !== undefined ? info.group + " " : "")
        + (info.name !== undefined ? info.name : "")
    Accessible.description: (choices[current] !== undefined ? choices[current] : "")
        + (theme.knobNextNoteMark && nextNote ? ". Applies to new notes" : "")
        + (inactive && inactiveReason !== "" ? ". Inactive: " + inactiveReason : "")

    function openMenu() {
        if (!controller.editable)
            return
        theme.lastTouched = parameterId
        const items = []
        for (let i = 0; i < choices.length; ++i)
            items.push({ label: choices[i], value: i })
        menu.open(dropdown, items, current, accent,
                  value => dropdown.controller.set_choice(dropdown.parameterId, value))
    }
    function stepBy(direction) {
        if (!controller.editable)
            return
        controller.step(parameterId, direction, false)
        theme.lastTouched = parameterId
    }
    Keys.onUpPressed: stepBy(-1)
    Keys.onDownPressed: stepBy(1)
    Keys.onReturnPressed: openMenu()
    Keys.onSpacePressed: openMenu()
    Keys.onDeletePressed: { controller.reset_to_default(parameterId); theme.lastTouched = parameterId }

    Text {
        x: dropdown.theme.dropdownTextX
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - dropdown.theme.dropdownTextRight
        text: dropdown.choices[dropdown.current] !== undefined ? dropdown.choices[dropdown.current] : ""
        color: dropdown.theme.dropdownInk(dropdown)
        font.family: dropdown.theme.dropdownFont
        font.pixelSize: dropdown.fontSize
        font.bold: dropdown.theme.dropdownBold
        elide: Text.ElideRight
    }
    // The chevron: a glyph, or a drawn caret.
    Text {
        visible: !dropdown.theme.dropdownCaret
        anchors.right: parent.right
        anchors.rightMargin: dropdown.theme.dropdownChevronMargin
        anchors.verticalCenter: parent.verticalCenter
        text: "▾"
        color: dropdown.theme.dropdownChevronInk !== undefined ? dropdown.theme.dropdownChevronInk
                                                                : dropdown.accent
        font.pixelSize: dropdown.theme.dropdownChevronSize
    }
    Canvas {
        visible: dropdown.theme.dropdownCaret
        x: parent.width - dropdown.theme.dropdownCaretRight
        anchors.verticalCenter: parent.verticalCenter
        width: dropdown.theme.dropdownCaretWidth
        height: dropdown.theme.dropdownCaretHeight
        // Strokes sit on pixel centres.
        readonly property real pixelCentre: 0.5
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = dropdown.theme.dropdownChevronInk !== undefined
                ? dropdown.theme.dropdownChevronInk : dropdown.accent
            ctx.lineWidth = dropdown.theme.dropdownCaretStroke
            ctx.beginPath()
            ctx.moveTo(pixelCentre, pixelCentre)
            ctx.lineTo(width / 2, height - 1)
            ctx.lineTo(width - pixelCentre, pixelCentre)
            ctx.stroke()
        }
    }
    Tip {
        theme: dropdown.theme
        shown: area.containsMouse && !area.pressed && !dropdown.menu.visible
        text: dropdown.Accessible.name + "  " + (dropdown.choices[dropdown.current] || "")
        detail: MacroPalette.tipLine(dropdown.controller, dropdown.parameterId)
            + "Click to choose · Up and Down step · Delete resets"
            + (dropdown.nextNote ? "\nApplies to new notes" : "")
    }
    MouseArea {
        id: area
        anchors.fill: parent
        // At least the kit's least hit size, however small the control draws.
        anchors.leftMargin: -dropdown.theme.hitOutset(parent.width)
        anchors.rightMargin: -dropdown.theme.hitOutset(parent.width)
        anchors.topMargin: -dropdown.theme.hitOutset(parent.height)
        anchors.bottomMargin: -dropdown.theme.hitOutset(parent.height)
        hoverEnabled: true
        enabled: dropdown.controller.editable
        onClicked: {
            dropdown.forceActiveFocus()
            dropdown.openMenu()
        }
    }
}
