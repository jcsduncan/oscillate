pragma ComponentBehavior: Bound
import QtQuick
import "../shell" as Shell

// One shared popup list for a panel's dropdowns and preset browser. Entries
// are {label, value, header?}; picked(value) goes to the handler given to
// open().
Item {
    id: menu
    required property KitTheme theme
    property var entries: []
    property var handler: null
    property int highlighted: -1
    property color accent: theme.accent
    readonly property bool opened: visible

    objectName: theme.prefix + "Menu"
    visible: false
    z: 100

    function open(anchor, items, currentValue, color, onPicked) {
        entries = items
        handler = onPicked
        accent = color
        highlighted = -1
        for (let i = 0; i < items.length; ++i) {
            if (!items[i].header && items[i].value === currentValue)
                highlighted = i
        }
        const gap = theme.menuGap
        const point = anchor.mapToItem(menu, 0, anchor.height + gap)
        list.width = Math.max(anchor.width, theme.menuMinWidth)
        const margin = theme.menuMargin
        const height = Math.min(list.contentHeight + 2 * theme.menuPad, menu.height - 2 * margin)
        list.height = height
        list.x = Math.max(margin, Math.min(point.x, menu.width - list.width - margin))
        list.y = point.y + height > menu.height - margin ? Math.max(margin, point.y - anchor.height - 2 * gap - height)
                                                     : point.y
        visible = true
        if (Shell.Motion.enabled)
            appear.restart()
        list.forceActiveFocus()
    }
    // The list unfolds from its anchor: a quick fade and a short drop.
    ParallelAnimation {
        id: appear
        NumberAnimation { target: list; property: "opacity"; from: 0; to: 1
                          duration: Shell.Motion.fast; easing.type: Easing.OutQuad }
        NumberAnimation { target: list; property: "scale"; from: menu.theme.menuAppearScale; to: 1
                          duration: Shell.Motion.fast; easing.type: Easing.OutQuad }
    }
    // Opens the preset browser: presets grouped under their categories.
    function openPresets(anchor, controller, color) {
        const items = []
        const categories = controller.preset_categories
        for (let c = 0; c < categories.length; ++c) {
            items.push({ label: categories[c], value: -1, header: true })
            const presets = controller.presets
            for (let i = 0; i < presets.length; ++i) {
                if (presets[i].category === categories[c])
                    items.push({ label: presets[i].name, value: presets[i].index })
            }
        }
        open(anchor, items, controller.preset_index, color, index => controller.apply_preset(index))
    }
    function close() {
        visible = false
    }
    function choose(index) {
        const entry = entries[index]
        if (entry === undefined || entry.header)
            return
        const pick = handler
        close()
        if (pick)
            pick(entry.value)
    }
    function move(direction) {
        let next = highlighted
        for (let tries = 0; tries < entries.length; ++tries) {
            next = (next + direction + entries.length) % entries.length
            if (!entries[next].header) {
                highlighted = next
                return
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: menu.close()
        onWheel: wheel => { wheel.accepted = true }
    }

    Rectangle {
        id: list
        readonly property real contentHeight: column.implicitHeight
        radius: menu.theme.menuRadius
        color: menu.theme.menuFill
        border.color: menu.theme.menuEdge
        clip: true
        Accessible.role: Accessible.PopupMenu
        Keys.onEscapePressed: menu.close()
        Keys.onUpPressed: menu.move(-1)
        Keys.onDownPressed: menu.move(1)
        Keys.onReturnPressed: menu.choose(menu.highlighted)
        Keys.onSpacePressed: menu.choose(menu.highlighted)

        Flickable {
            anchors.fill: parent
            anchors.margins: menu.theme.menuPad
            contentHeight: column.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: column
                width: parent.width
                Repeater {
                    model: menu.entries.length
                    delegate: Rectangle {
                        id: row
                        required property int index
                        readonly property var entry: menu.entries[index]
                        readonly property bool header: entry.header === true
                        readonly property bool lit: index === menu.highlighted
                        width: column.width
                        height: header ? menu.theme.menuHeaderHeight : menu.theme.menuRowHeight
                        radius: menu.theme.menuRowRadius
                        color: !header && (lit || rowArea.containsMouse)
                            ? Qt.rgba(menu.accent.r, menu.accent.g, menu.accent.b, menu.theme.menuHighlightAlpha)
                            : "transparent"
                        Accessible.role: header ? Accessible.StaticText : Accessible.MenuItem
                        Accessible.name: entry.label
                        Text {
                            x: row.header ? menu.theme.menuHeaderX : menu.theme.menuRowX
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: row.header ? menu.theme.menuHeaderOffset : 0
                            width: parent.width - menu.theme.menuTextRight
                            text: row.header ? row.entry.label.toUpperCase() : row.entry.label
                            color: row.header ? menu.theme.menuHeaderInk
                                : row.lit && !menu.theme.menuBoldHighlight ? menu.accent : menu.theme.menuInk
                            font.pixelSize: row.header ? menu.theme.menuHeaderSize : menu.theme.menuRowSize
                            font.bold: row.header || (row.lit && menu.theme.menuBoldHighlight)
                            font.letterSpacing: row.header ? menu.theme.menuHeaderSpacing : 0
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            enabled: !row.header
                            hoverEnabled: true
                            onClicked: menu.choose(row.index)
                        }
                    }
                }
            }
        }
    }
}
