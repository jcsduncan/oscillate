pragma ComponentBehavior: Bound
import QtQuick
import "MacroPalette.js" as MacroPalette

// A knob's macro menu (ADR-0059): map it to one of the track's eight macros,
// or take it off the one that drives it. It covers the whole window while
// open, so a click anywhere else closes it.
Item {
    id: menu
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property string label: ""
    readonly property int current: menu.controller.macro_revision >= 0
        ? menu.controller.macro_of(menu.parameterId) : -1
    property int highlighted: 0
    signal closed()

    objectName: "knobMacroMenu"
    visible: false
    z: 1000

    readonly property var entries: {
        const items = []
        for (let index = 0; index < MacroPalette.colors.length; ++index)
            items.push({ label: (index === menu.current ? "✓ " : "") + "Map to Macro " + (index + 1),
                         macro: index })
        if (menu.current >= 0)
            items.push({ label: "Remove from Macro " + (menu.current + 1), macro: -1 })
        return items
    }

    function openAt(anchor) {
        const window = anchor.Window.window
        parent = window ? window.contentItem : anchor
        width = parent.width
        height = parent.height
        const point = anchor.mapToItem(menu, anchor.width / 2, anchor.height)
        const margin = theme.macroMenuMargin
        list.x = Math.max(margin, Math.min(point.x - list.width / 2, width - list.width - margin))
        list.y = point.y + list.height > height - margin ? Math.max(margin, point.y - anchor.height - list.height)
                                                     : point.y
        highlighted = menu.current >= 0 ? menu.current : 0
        visible = true
        list.forceActiveFocus()
    }
    function close() {
        visible = false
        closed()
    }
    function choose(index) {
        const entry = entries[index]
        if (entry === undefined)
            return
        if (entry.macro < 0)
            menu.controller.unmap_macro(menu.parameterId)
        else
            menu.controller.map_to_macro(menu.parameterId, entry.macro)
        close()
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: menu.close()
        onWheel: wheel => { wheel.accepted = true }
    }

    Rectangle {
        id: list
        width: menu.theme.macroMenuWidth
        height: column.implicitHeight + 2 * menu.theme.macroMenuPad
        radius: menu.theme.macroMenuRadius
        color: menu.theme.macroMenuFill
        border.color: menu.theme.macroMenuEdge
        activeFocusOnTab: true
        Accessible.role: Accessible.PopupMenu
        Accessible.name: "Macro menu for " + menu.label
        Keys.onUpPressed: menu.highlighted = Math.max(0, menu.highlighted - 1)
        Keys.onDownPressed: menu.highlighted = Math.min(menu.entries.length - 1, menu.highlighted + 1)
        Keys.onReturnPressed: menu.choose(menu.highlighted)
        Keys.onEnterPressed: menu.choose(menu.highlighted)
        Keys.onEscapePressed: menu.close()
        MouseArea { anchors.fill: parent }
        Column {
            id: column
            x: menu.theme.macroMenuPad; y: menu.theme.macroMenuPad
            width: parent.width - 2 * menu.theme.macroMenuPad
            Text {
                width: parent.width
                text: menu.label
                color: menu.theme.macroMenuTitleInk; font.pixelSize: menu.theme.typeSmall; font.bold: true
                elide: Text.ElideRight
                bottomPadding: menu.theme.spaceXs
            }
            Repeater {
                model: menu.entries
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    objectName: "knobMacroMenuItem:" + index
                    width: column.width; height: menu.theme.macroMenuRowHeight; radius: menu.theme.radiusMd
                    color: menu.highlighted === index ? menu.theme.macroMenuHighlight : "transparent"
                    Rectangle {
                        visible: row.modelData.macro >= 0
                        x: menu.theme.macroMenuSwatchX; anchors.verticalCenter: parent.verticalCenter
                        width: menu.theme.macroMenuSwatchSize; height: width; radius: width / 2
                        color: MacroPalette.color(row.modelData.macro)
                    }
                    Text {
                        x: menu.theme.macroMenuTextX; anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.label
                        color: row.modelData.macro < 0 ? menu.theme.macroMenuRemoveInk : menu.theme.macroMenuInk
                        font.pixelSize: menu.theme.typeBody
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: menu.highlighted = row.index
                        onClicked: menu.choose(row.index)
                    }
                }
            }
        }
    }
}
