pragma ComponentBehavior: Bound
import QtQuick

// A choice parameter as tiles: a name and an optional hint under it, the
// chosen tile edged in its accent on a tint, or, as a segment (`segment`),
// joined buttons. A chosen button or name-only tile is a segmented choice:
// filled with the accent, its name set in accentInk. Click a tile, or use the arrow
// keys; Delete resets. Every pick is one undo step. `order` shows the
// choices in another order than the table's, `names` renames a tile. An
// empty parameterId leaves the tiles inert. The theme's choice tokens give
// the tiles their look.
Item {
    id: choice
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property color accent: theme.accent
    // A chosen segmented choice's name, on the accent fill.
    property color accentInk: theme.accentInk
    property color tint: theme.choiceTileOn
    property int columns: theme.choiceColumns
    property int tileHeight: theme.choiceTileHeight
    property int spacing: theme.choiceSpacing
    // Joined buttons rather than tiles.
    property bool segment: false
    // A segment button's width; tiles share the width.
    property int segmentWidth: theme.choiceSegmentWidth
    property int nameSize: theme.choiceNameSize
    // A hint per choice index, under its name; none when shorter.
    property var hints: []
    property var order: []
    // Overrides a choice's name on its tile, by index.
    property var names: ({})
    readonly property bool bound: parameterId !== ""
    readonly property var info: bound ? controller.parameter(parameterId) : ({})
    readonly property var choices: info.choices !== undefined ? info.choices : []
    readonly property var shown: {
        if (order.length > 0)
            return order
        const all = []
        for (let i = 0; i < choices.length; ++i)
            all.push(i)
        return all
    }
    readonly property int current: bound && controller.revision >= 0 ? controller.choice(parameterId) : -1
    readonly property int cellColumns: segment ? Math.max(1, shown.length) : Math.max(1, columns)
    readonly property int rows: segment ? 1 : Math.ceil(shown.length / cellColumns)
    readonly property bool editable: bound && controller.editable
    // Fires after the user picks a tile, so the panel can play the change.
    signal picked(int index)

    objectName: theme.prefix + "Choice:" + parameterId
    implicitWidth: segment ? choices.length * segmentWidth : theme.choiceWidth
    implicitHeight: rows * tileHeight + Math.max(0, rows - 1) * spacing
    activeFocusOnTab: editable
    opacity: editable ? 1 : theme.choiceDisabledOpacity
    Accessible.role: Accessible.ComboBox
    Accessible.name: (info.group !== undefined
                      && (theme.choiceLongName === "group" || info.group !== info.name) ? info.group + " " : "")
        + (info.name !== undefined ? info.name : "")
    Accessible.description: choices[current] !== undefined ? choices[current] : ""

    function pick(index) {
        if (!editable)
            return
        theme.lastTouched = parameterId
        controller.set_choice(parameterId, index)
        picked(index)
    }
    function stepBy(direction) {
        if (!editable)
            return
        controller.step(parameterId, direction, false)
        theme.lastTouched = parameterId
    }
    Keys.onLeftPressed: stepBy(-1)
    Keys.onUpPressed: stepBy(-1)
    Keys.onRightPressed: stepBy(1)
    Keys.onDownPressed: stepBy(1)
    Keys.onDeletePressed: {
        if (!editable)
            return
        controller.reset_to_default(parameterId)
        theme.lastTouched = parameterId
    }

    // The focus ring, just outside the tiles.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -choice.theme.choiceFocusMargin
        radius: choice.theme.choiceFocusRadius
        color: "transparent"
        border.width: choice.theme.choiceFocusWidth
        border.color: !choice.activeFocus ? "transparent"
            : choice.theme.choiceFocusInk !== undefined ? choice.theme.choiceFocusInk : choice.accent
    }

    // A segment's frame: one rounded edge around the joined buttons.
    Rectangle {
        anchors.fill: parent
        visible: choice.segment
        radius: choice.theme.choiceRadius
        color: choice.theme.choiceTile
        border.color: choice.theme.choiceTileEdge
    }

    Repeater {
        model: choice.shown
        delegate: Rectangle {
            id: tile
            required property int index
            required property int modelData
            readonly property int choiceIndex: modelData
            readonly property bool on: choiceIndex === choice.current
            readonly property bool hovered: tileArea.containsMouse
            readonly property bool segment: choice.segment
            readonly property color accent: choice.accent
            readonly property color tint: choice.tint
            readonly property int nameSize: choice.nameSize
            readonly property string hint: choice.hints[choiceIndex] !== undefined ? choice.hints[choiceIndex] : ""
            // A joined button or a tile with no hint picks like a segmented
            // control: its chosen state is the accent fill and inverse ink.
            readonly property bool segmented: segment || hint === ""
            readonly property color selectionFill: on && segmented ? accent : choice.theme.choiceTileFill(tile)
            readonly property color selectionInk: on && segmented ? choice.accentInk : choice.theme.choiceNameInk(tile)
            readonly property int column: index % choice.cellColumns
            readonly property int row: Math.floor(index / choice.cellColumns)
            // Whole pixels when the theme asks, so a row's tiles never sum
            // past the width.
            readonly property real cell: {
                const share = (choice.width - choice.spacing * (choice.cellColumns - 1)) / choice.cellColumns
                return choice.theme.choiceWholePixels ? Math.floor(share) : share
            }
            readonly property real segmentCell: Math.floor(choice.width / choice.cellColumns)
            // With choiceSpanLast the last tile of a short row spans what is
            // left of it.
            readonly property int spare: choice.cellColumns - (choice.shown.length % choice.cellColumns)
            readonly property bool spans: choice.theme.choiceSpanLast && !choice.segment
                && index === choice.shown.length - 1 && spare < choice.cellColumns
            objectName: choice.theme.prefix + "Choice:" + choice.parameterId + ":" + choiceIndex
            x: choice.segment ? column * segmentCell : column * (cell + choice.spacing)
            y: row * (choice.tileHeight + choice.spacing)
            width: choice.segment ? segmentCell : spans ? cell * (spare + 1) + choice.spacing * spare : cell
            height: choice.tileHeight
            radius: choice.segment ? choice.theme.choiceRadius - choice.theme.choiceEdgeWidth : choice.theme.choiceRadius
            color: selectionFill
            border.width: choice.segment ? 0 : choice.theme.choiceEdgeWidth
            border.color: on ? choice.accent : choice.theme.choiceTileEdge
            Accessible.role: Accessible.RadioButton
            Accessible.name: choice.choices[tile.choiceIndex]
            Accessible.checked: tile.on
            // A segment's divider before each button after the first.
            Rectangle {
                visible: choice.segment && tile.index > 0
                width: choice.theme.choiceEdgeWidth
                height: parent.height
                color: choice.theme.choiceTileEdge
            }
            Column {
                x: tile.hint !== "" ? choice.theme.choiceTextPad : choice.theme.choiceTextInset
                width: tile.width - 2 * x
                anchors.verticalCenter: parent.verticalCenter
                spacing: choice.theme.choiceHintGap
                Text {
                    width: parent.width
                    horizontalAlignment: tile.hint !== "" ? Text.AlignLeft : Text.AlignHCenter
                    text: choice.names[tile.choiceIndex] !== undefined ? choice.names[tile.choiceIndex]
                                                                       : choice.choices[tile.choiceIndex]
                    color: tile.selectionInk
                    font.family: choice.theme.choiceFont
                    font.pixelSize: choice.nameSize
                    font.weight: choice.theme.choiceNameWeight(tile)
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: tile.hint !== ""
                    text: tile.hint
                    color: choice.theme.choiceHintInk
                    font.family: choice.theme.choiceFont
                    font.pixelSize: choice.nameSize >= choice.theme.choiceNameSize
                        ? choice.theme.choiceHintSize : choice.theme.choiceHintSizeSmall
                    elide: Text.ElideRight
                }
            }
            MouseArea {
                id: tileArea
                anchors.fill: parent
                enabled: choice.editable
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    choice.forceActiveFocus()
                    choice.pick(tile.choiceIndex)
                }
                HoverTip {
                    theme: choice.theme
                    text: choice.Accessible.name + "  " + choice.choices[tile.choiceIndex]
                }
            }
        }
    }
}
