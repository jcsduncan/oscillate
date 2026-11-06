pragma ComponentBehavior: Bound
import QtQuick

// A dropdown readout that opens the panel's menu. Bound to a choice
// parameter it lists the choices, under group headers when `groups`
// ([{name, first, last}]) is set; unbound it shows `text` and lists
// `entries` ([{label, value, header?}]), reporting `picked(value)`.
Rectangle {
    id: dropdown
    required property var panel
    property string parameterId: ""
    property var groups: []
    property var entries: []
    property var currentValue: undefined
    property string text: ""
    property bool accentStyle: false
    // Its module's accent: the accent-style text, the focus edge and the menu.
    property color accent: parameterId !== "" ? panel.accentFor(parameterId) : panel.theme.cy
    property bool inactive: false
    property real fontSize: panel.theme.typeBody
    readonly property var info: parameterId !== "" ? panel.controller.parameter(parameterId) : ({})
    readonly property var choices: info.choices !== undefined ? info.choices : []
    readonly property int current: parameterId !== "" && panel.controller.revision >= 0
        ? panel.controller.choice(parameterId) : -1
    readonly property string shownText: parameterId !== ""
        ? (choices[current] !== undefined ? choices[current] : "") : text
    signal picked(var value)

    objectName: "novaDropdown:" + (parameterId !== "" ? parameterId : text)
    implicitWidth: panel.theme.dropdownWidth
    implicitHeight: panel.theme.dropdownHeight
    radius: panel.theme.dropdownRadius
    color: area.containsMouse ? panel.theme.hoverDeep : panel.theme.wellFill
    border.color: activeFocus ? accent : accentStyle ? panel.theme.accentSoft : panel.theme.line2
    opacity: !panel.controller.editable ? panel.theme.dropdownDisabledOpacity : inactive ? panel.theme.dropdownInactiveOpacity : 1
    activeFocusOnTab: panel.controller.editable
    Accessible.role: Accessible.ComboBox
    Accessible.name: parameterId !== "" ? (info.group + " " + info.name) : text
    Accessible.description: shownText

    function open() {
        if (!panel.controller.editable)
            return
        if (parameterId === "") {
            panel.menu.open(dropdown, entries, currentValue, dropdown.accent, value => dropdown.picked(value))
            return
        }
        const items = []
        if (groups.length > 0) {
            for (let g = 0; g < groups.length; ++g) {
                items.push({ label: groups[g].name, value: -1, header: true })
                for (let i = groups[g].first; i <= groups[g].last && i < choices.length; ++i)
                    items.push({ label: choices[i], value: i })
            }
        } else {
            for (let i = 0; i < choices.length; ++i)
                items.push({ label: choices[i], value: i })
        }
        panel.menu.open(dropdown, items, current, dropdown.accent, value => {
            dropdown.panel.controller.set_choice(dropdown.parameterId, value)
            dropdown.panel.touchId(dropdown.parameterId, (dropdown.info.name || "").toUpperCase())
            dropdown.picked(value)
        })
    }
    Keys.onReturnPressed: open()
    Keys.onSpacePressed: open()
    Keys.onUpPressed: if (parameterId !== "") panel.controller.step(parameterId, -1, false)
    Keys.onDownPressed: if (parameterId !== "") panel.controller.step(parameterId, 1, false)

    Text {
        x: dropdown.panel.theme.dropdownTextInset
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - dropdown.panel.theme.dropdownTextRoom
        text: dropdown.shownText
        color: dropdown.accentStyle ? dropdown.accent : dropdown.panel.theme.text
        font.family: dropdown.panel.theme.sans
        font.pixelSize: dropdown.fontSize
        font.weight: Font.Medium
        elide: Text.ElideRight
    }
    Canvas {
        x: parent.width - dropdown.panel.theme.dropdownChevronRight
        anchors.verticalCenter: parent.verticalCenter
        width: dropdown.panel.theme.dropdownChevronWidth
        height: dropdown.panel.theme.dropdownChevronHeight
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = dropdown.panel.theme.dim
            ctx.beginPath()
            ctx.moveTo(0, 0)
            ctx.lineTo(width / 2, height)
            ctx.lineTo(width, 0)
            ctx.closePath()
            ctx.fill()
        }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: {
            dropdown.forceActiveFocus()
            dropdown.open()
        }
    }
}
