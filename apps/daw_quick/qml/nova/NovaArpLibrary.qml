pragma ComponentBehavior: Bound
import QtQuick

// The arp's pattern library (ADR-0078): about sixty factory patterns in
// eight categories, each loaded into the arp as one edit (locked lanes
// stay). Like the preset browser, the list keeps the keys so the typing
// keyboard goes on playing the held chord: ↑/↓ move and load (a browse is
// an audition), "/" or a click enters the search and Escape returns to the
// list. The loaded pattern is marked.
Rectangle {
    id: library
    required property var panel
    readonly property var arp: panel.controller.arp
    readonly property var theme: panel.theme
    property string category: ""
    property string search: ""
    property int selected: -1
    readonly property var entries: compute(arp !== null ? arp.patterns : [], category, search)
    readonly property string loaded: panel.doc.labels_revision >= 0 ? panel.doc.label("arp.pattern", "") : ""
    readonly property bool searching: searchInput.activeFocus
    readonly property real pad: theme.arpLibraryPad
    readonly property real rowHeight: theme.arpLibraryRowHeight
    readonly property real chipHeight: theme.arpChipHeight

    objectName: "novaArpLibrary"
    radius: theme.moduleCardRadius
    color: theme.card
    // The list's focus shows on the library's edge.
    border.color: list.activeFocus ? theme.am : theme.line
    border.width: list.activeFocus ? theme.strokeBold : theme.hairline
    Accessible.role: Accessible.Pane
    Accessible.name: "Arp patterns"
    onSearchingChanged: panel.arpSearching = searching

    function compute(patterns, category, search) {
        const needle = search.toLowerCase()
        return patterns.filter(p => (category === "" || p.category === category)
                               && (needle === "" || p.name.toLowerCase().indexOf(needle) >= 0
                                   || p.category.toLowerCase().indexOf(needle) >= 0
                                   || p.mode.toLowerCase().indexOf(needle) >= 0))
    }
    function load(i) {
        if (i < 0 || i >= entries.length)
            return
        selected = i
        list.positionViewAtIndex(i, ListView.Contain)
        arp.apply_pattern(entries[i].index)
    }
    function move(direction) {
        if (entries.length === 0)
            return
        load(Math.max(0, Math.min(entries.length - 1, (selected < 0 ? -1 : selected) + direction)))
    }
    function focusList() {
        list.forceActiveFocus()
    }

    Text {
        id: title
        x: library.pad
        y: library.theme.arpLibraryTitleTop
        text: "PATTERNS"
        color: library.theme.text
        font.family: library.theme.sans
        font.pixelSize: library.theme.cardTitleSize
        font.weight: Font.ExtraBold
        font.letterSpacing: library.theme.cardTitleSpacing
    }
    Text {
        anchors.left: title.right
        anchors.leftMargin: library.theme.spaceMd
        anchors.baseline: title.baseline
        text: library.entries.length + " · ↑↓ LOAD · / SEARCH"
        color: library.theme.faint
        font.family: library.theme.sans
        font.pixelSize: library.theme.typeCaption
    }

    // The search: amber while it has the keys.
    Rectangle {
        id: searchBox
        objectName: "novaArpSearch"
        x: library.pad
        y: library.theme.arpSearchTop
        width: library.width - 2 * library.pad
        height: library.theme.arpSearchHeight
        radius: library.theme.radiusLg
        color: library.theme.wellFill
        border.color: searchInput.activeFocus ? library.theme.am : library.theme.line2
        TextInput {
            id: searchInput
            anchors.fill: parent
            anchors.leftMargin: library.theme.spaceMd
            anchors.rightMargin: library.theme.spaceMd
            verticalAlignment: TextInput.AlignVCenter
            color: library.theme.text
            font.family: library.theme.sans
            font.pixelSize: library.theme.typeBody
            clip: true
            Accessible.role: Accessible.EditableText
            Accessible.name: "Search patterns"
            onTextEdited: {
                library.search = text
                library.selected = -1
            }
            Keys.onEscapePressed: library.focusList()
            Keys.onReturnPressed: library.focusList()
            Keys.onUpPressed: library.move(-1)
            Keys.onDownPressed: library.move(1)
        }
        Text {
            anchors.fill: searchInput
            verticalAlignment: Text.AlignVCenter
            visible: searchInput.text === ""
            text: "Search patterns, modes…"
            color: library.theme.faint
            font.family: library.theme.sans
            font.pixelSize: library.theme.typeBody
        }
    }

    Flow {
        id: chips
        x: library.pad
        y: searchBox.y + searchBox.height + library.theme.spaceSm
        width: library.width - 2 * library.pad
        spacing: library.theme.spaceXs
        Repeater {
            model: [""].concat(library.arp !== null ? library.arp.categories : [])
            delegate: Rectangle {
                id: chip
                required property string modelData
                readonly property bool on: library.category === modelData
                readonly property color tone: modelData === "" ? library.theme.text
                                                                 : library.theme.arpCategoryColour(modelData)
                objectName: "novaArpCategory:" + (modelData === "" ? "All" : modelData)
                width: chipText.implicitWidth + library.theme.arpChipPad
                height: library.chipHeight
                radius: height / 2
                color: on ? library.theme.soft(tone) : "transparent"
                border.color: activeFocus || on ? tone : library.theme.line2
                border.width: activeFocus ? library.theme.strokeBold : library.theme.hairline
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: chipText.text
                Accessible.checked: on
                function activate() {
                    library.category = on ? "" : modelData
                    library.selected = -1
                }
                Keys.onSpacePressed: activate()
                Keys.onReturnPressed: activate()
                Text {
                    id: chipText
                    anchors.centerIn: parent
                    text: chip.modelData === "" ? "ALL" : chip.modelData.toUpperCase()
                    color: chip.on ? chip.tone : library.theme.dim
                    font.family: library.theme.sans
                    font.pixelSize: library.theme.typeMicro
                    font.weight: Font.Bold
                    font.letterSpacing: library.theme.arpChipSpacing
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        chip.activate()
                        library.focusList()
                    }
                }
            }
        }
    }

    ListView {
        id: list
        objectName: "novaArpPatternList"
        x: library.theme.hairline
        y: chips.y + chips.height + library.theme.spaceSm
        width: library.width - 2 * library.theme.hairline
        height: library.height - y - library.theme.spaceSm
        clip: true
        model: library.entries.length
        boundsBehavior: Flickable.StopAtBounds
        // The library's own keys; letters fall through to the typing keyboard.
        keyNavigationEnabled: false
        activeFocusOnTab: true
        Accessible.role: Accessible.List
        Accessible.name: "Patterns"
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Slash) {
                searchInput.forceActiveFocus()
                event.accepted = true
            }
        }
        Keys.onUpPressed: library.move(-1)
        Keys.onDownPressed: library.move(1)
        delegate: Rectangle {
            id: row
            required property int index
            readonly property var entry: library.entries[index]
            readonly property bool lit: index === library.selected
            readonly property bool isLoaded: entry.name === library.loaded
            readonly property color tone: library.theme.arpCategoryColour(entry.category)
            objectName: "novaArpPattern:" + entry.name
            width: ListView.view.width
            height: library.rowHeight
            color: lit ? library.theme.selectedFill : rowArea.containsMouse ? library.theme.hover : "transparent"
            Accessible.role: Accessible.ListItem
            Accessible.name: entry.name + ", " + entry.category
            Accessible.selected: lit
            Rectangle {
                visible: row.lit
                width: library.theme.strokeBold
                height: parent.height
                color: row.tone
            }
            Rectangle {
                x: library.pad
                anchors.verticalCenter: parent.verticalCenter
                width: library.theme.arpDotSize
                height: width
                radius: width / 2
                color: row.tone
            }
            Text {
                x: library.pad + library.theme.arpNameIndent
                anchors.verticalCenter: parent.verticalCenter
                width: row.width - x - modeText.implicitWidth - 2 * library.pad
                text: row.entry.name + (row.isLoaded ? "  ✓" : "")
                color: row.isLoaded ? row.tone : library.theme.text
                elide: Text.ElideRight
                font.family: library.theme.sans
                font.pixelSize: library.theme.typeBody
                font.weight: row.isLoaded ? Font.DemiBold : Font.Normal
            }
            Text {
                id: modeText
                anchors.right: parent.right
                anchors.rightMargin: library.pad
                anchors.verticalCenter: parent.verticalCenter
                text: row.entry.mode.toUpperCase() + " · " + row.entry.rate
                color: library.theme.faint
                font.family: library.theme.mono
                font.pixelSize: library.theme.typeMicro
            }
            MouseArea {
                id: rowArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    library.load(row.index)
                    library.focusList()
                }
            }
        }
    }
}
