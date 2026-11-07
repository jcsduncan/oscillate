pragma ComponentBehavior: Bound
import QtQuick
import "../shell" as Shell

// A row of joined, mutually exclusive choices in a frame, one lit. Three
// sources: a parameter (`controller` and `parameterId`, with optional
// shorter `labels`; the left and right arrows change it), a list of labels
// (`labels`, `currentIndex`, `chosen(index)`), or options
// ({ id, label, tip }) chosen by id (`options`, `current`, `picked(id)`).
// In the last two each segment takes focus and Return or Space picks it,
// and a segment named `<namePrefix>:<id>` has `activate()`. `icons` with an
// `icon` component draw an icon per choice in place of its label: the
// component's item has `kind` (the icon name) and `ink` properties, bound to
// the cell's. The chosen cell is filled with the accent and its text set in
// accentInk; the theme's segmented tokens and its segmentedFill/Edge/Ink
// functions style the frame and the other cells.
Rectangle {
    id: control
    required property KitTheme theme
    property var controller: null
    property string parameterId: ""
    property var labels: []
    property int currentIndex: 0
    property var options: []
    property string current: ""
    property string namePrefix: ""
    property string accessibleName: ""
    property var icons: []
    property Component icon: null
    property color accent: theme.accent
    // The text on the accent fill.
    property color accentInk: theme.accentInk
    signal chosen(int index)
    signal picked(string id)

    readonly property bool bound: controller !== null
    readonly property bool keyed: options.length > 0
    readonly property var info: bound ? controller.parameter(parameterId) : ({})
    readonly property var names: {
        if (keyed)
            return options.map(option => option.label)
        if (labels.length > 0 || !bound)
            return labels
        return info.choices !== undefined ? info.choices : []
    }
    readonly property int chosenIndex: bound ? (controller.revision >= 0 ? controller.choice(parameterId) : 0)
                                             : currentIndex
    readonly property bool nextNote: info.next_note === true
    readonly property bool editable: !bound || controller.editable
    readonly property bool equalCells: theme.segmentedEqualCells

    function isOn(index) {
        return keyed ? String(options[index].id) === current : index === chosenIndex
    }
    function choose(index) {
        if (bound) {
            control.forceActiveFocus()
            controller.set_choice(parameterId, index)
            return
        }
        chosen(index)
        if (keyed)
            picked(String(options[index].id))
    }

    objectName: bound ? theme.prefix + "Segmented:" + parameterId : ""
    implicitWidth: equalCells ? names.length * theme.segmentedCellWidth : row.implicitWidth + 2 * theme.segmentedMargin
    implicitHeight: theme.segmentedHeight
    radius: theme.segmentedRadius
    color: theme.segmentedFrame
    border.color: activeFocus ? accent : theme.segmentedFrameEdge
    clip: theme.segmentedClip
    activeFocusOnTab: bound && controller.editable
    opacity: editable ? 1 : theme.segmentedDisabledOpacity
    Accessible.role: Accessible.PageTabList
    Accessible.name: bound ? (info.name !== undefined ? info.name : "") : accessibleName
    Accessible.description: !bound ? ""
        : (names[chosenIndex] !== undefined ? names[chosenIndex] : "")
          + (nextNote ? ". Applies to new notes" : "") + ". Left and right arrows change it"

    Keys.onPressed: event => {
        if (!bound || !controller.editable)
            return
        if (event.key === Qt.Key_Left && chosenIndex > 0) {
            controller.set_choice(parameterId, chosenIndex - 1)
            event.accepted = true
        } else if (event.key === Qt.Key_Right && chosenIndex < names.length - 1) {
            controller.set_choice(parameterId, chosenIndex + 1)
            event.accepted = true
        }
    }

    Row {
        id: row
        x: control.theme.segmentedMargin
        y: control.theme.segmentedMargin
        height: control.height - 2 * control.theme.segmentedMargin
        spacing: control.theme.segmentedSpacing
        Repeater {
            model: control.names.length
            delegate: Rectangle {
                id: cell
                required property int index
                readonly property bool on: control.isOn(index)
                readonly property bool hovered: cellArea.containsMouse
                readonly property bool pressed: cellArea.pressed
                readonly property color accent: control.accent
                readonly property color accentInk: control.accentInk
                readonly property string label: String(control.names[index])
                readonly property string tip: control.keyed && control.options[index].tip ? String(control.options[index].tip) : ""
                readonly property string optionId: control.keyed ? String(control.options[index].id) : String(index)
                readonly property bool iconShown: control.icons.length > 0 && control.icon !== null
                // The chosen cell's fill and text; the theme styles the others.
                readonly property color selectionFill: on ? accent : control.theme.segmentedFill(cell, 0)
                readonly property color selectionInk: on ? accentInk : control.theme.segmentedInk(cell)
                function activate() {
                    control.choose(cell.index)
                }
                objectName: control.namePrefix.length > 0 ? control.namePrefix + ":" + optionId : ""
                width: control.equalCells
                    ? (control.width - 2 * control.theme.segmentedMargin
                       - control.theme.segmentedSpacing * (control.names.length - 1)) / Math.max(1, control.names.length)
                    : cellText.implicitWidth + control.theme.segmentedCellPad
                height: row.height
                radius: control.theme.segmentedCellRadius
                gradient: control.theme.segmentedGradient ? cellGradient : undefined
                color: selectionFill
                Behavior on color {
                    enabled: control.theme.segmentedFade && Shell.Motion.enabled
                    ColorAnimation { duration: Shell.Motion.fast }
                }
                border.color: control.theme.segmentedEdge(cell)
                activeFocusOnTab: !control.bound && control.visible && control.enabled
                Accessible.role: control.theme.segmentedRadios ? Accessible.RadioButton : Accessible.PageTab
                Accessible.name: control.theme.tipIsName && tip.length > 0 ? tip : label
                Accessible.checked: on
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                property Gradient cellGradient: Gradient {
                    GradientStop { position: 0; color: cell.selectionFill }
                    GradientStop { position: 1; color: cell.on ? cell.accent : control.theme.segmentedFill(cell, 1) }
                }
                HoverTip {
                    theme: control.theme
                    enabled: control.theme.segmentedTips
                    text: cell.tip.length > 0 ? cell.tip : cell.label
                }
                // The divider before each segment after the first.
                Rectangle {
                    visible: control.theme.segmentedDividers && cell.index > 0
                    y: control.theme.segmentedDividerInset
                    width: control.theme.hairline
                    height: parent.height - 2 * control.theme.segmentedDividerInset
                    color: control.theme.segmentedDivider
                }
                Text {
                    id: cellText
                    visible: !cell.iconShown
                    anchors.centerIn: parent
                    // Equal cells can be narrower than a long label: it
                    // stays inside its cell, elided.
                    width: Math.min(implicitWidth, parent.width - control.theme.segmentedIconInset)
                    horizontalAlignment: Text.AlignHCenter
                    text: cell.label
                    color: cell.selectionInk
                    font: control.theme.segmentedFont(cell)
                    elide: Text.ElideRight
                }
                Loader {
                    readonly property string kind: control.icons.length > cell.index ? control.icons[cell.index] : ""
                    readonly property color ink: cell.selectionInk
                    active: cell.iconShown
                    anchors.centerIn: parent
                    width: Math.min(control.theme.segmentedIconWidth, cell.width - control.theme.segmentedIconInset)
                    height: control.theme.segmentedIconHeight
                    sourceComponent: control.icon
                    onLoaded: {
                        item.kind = Qt.binding(() => kind)
                        item.ink = Qt.binding(() => ink)
                    }
                }
                MouseArea {
                    id: cellArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: control.editable
                    cursorShape: control.theme.segmentedPointer ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: cell.activate()
                }
            }
        }
    }
}
