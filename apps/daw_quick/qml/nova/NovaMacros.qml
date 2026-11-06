pragma ComponentBehavior: Bound
import QtQuick

// The macros (NOVA-UI-10), on the MACROS overview card: eight knobs named
// by the preset in two rows, each with a ✥ handle to drag onto any knob.
// Double-click a name to rename it (a document label). Compact (the
// overview card), the knobs are full size and the rows sit at the top, as
// tall as they need; otherwise the knobs are big and the rows share the
// height.
Item {
    id: card
    required property var panel
    property bool compact: false
    readonly property string knobSize: compact ? "" : "big"
    readonly property int columns: 4
    readonly property int rows: 2
    readonly property real compactRowSpacing: panel.theme.spaceSm
    readonly property real minimumRowSpacing: panel.theme.spaceMd
    readonly property real nameHeight: 14
    readonly property real nameSize: panel.theme.typeSmall
    readonly property int nameLength: 32
    readonly property real handleGlyph: panel.theme.typeSmall

    objectName: "novaMacros"
    implicitHeight: grid.implicitHeight

    Grid {
        id: grid
        anchors.centerIn: card.compact ? undefined : parent
        anchors.horizontalCenter: card.compact ? parent.horizontalCenter : undefined
        columns: card.columns
        rowSpacing: card.compact ? card.compactRowSpacing
            : Math.max(card.minimumRowSpacing, (card.height - card.rows * macroHeight) / (card.rows + 1))
        // A big knob with its caption and value, the name and the handle.
        readonly property real macroHeight: 116
        Repeater {
            model: card.columns * card.rows
            delegate: Column {
                id: macro
                required property int index
                readonly property string source: "macro." + (index + 1)
                readonly property string name: card.panel.doc.labels_revision >= 0
                    ? card.panel.doc.label(source, "Macro " + (index + 1)) : ""
                width: card.width / card.columns
                spacing: card.panel.theme.spaceXxs
                NovaKnob {
                    anchors.horizontalCenter: parent.horizontalCenter
                    panel: card.panel
                    size: card.knobSize
                    parameterId: "nova.macro." + (macro.index + 1)
                    label: "M" + (macro.index + 1)
                }
                Item {
                    width: parent.width
                    height: card.nameHeight
                    Text {
                        id: nameText
                        objectName: "novaMacroName:" + (macro.index + 1)
                        visible: !nameEdit.visible
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.min(implicitWidth, parent.width - card.panel.theme.macroNameRoom)
                        text: macro.name
                        color: card.panel.theme.text
                        font.family: card.panel.theme.sans
                        font.pixelSize: card.nameSize
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -card.panel.theme.macroNameHit
                            enabled: card.panel.controller.editable
                            onDoubleClicked: {
                                nameEdit.text = macro.name
                                nameEdit.visible = true
                                nameEdit.forceActiveFocus()
                                nameEdit.selectAll()
                            }
                        }
                    }
                    Rectangle {
                        visible: nameEdit.visible
                        anchors.fill: nameEdit
                        anchors.margins: -card.panel.theme.macroEditOutset
                        radius: card.panel.theme.radiusSm
                        color: card.panel.theme.wellFill
                        border.color: card.panel.theme.vi
                    }
                    TextInput {
                        id: nameEdit
                        objectName: "novaMacroEdit:" + (macro.index + 1)
                        visible: false
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width - card.panel.theme.macroEditRoom
                        horizontalAlignment: TextInput.AlignHCenter
                        color: card.panel.theme.text
                        font.family: card.panel.theme.sans
                        font.pixelSize: card.nameSize
                        maximumLength: card.nameLength
                        selectByMouse: true
                        onAccepted: {
                            card.panel.doc.set_label(macro.source, text)
                            visible = false
                        }
                        onActiveFocusChanged: if (!activeFocus) visible = false
                        Keys.onEscapePressed: visible = false
                    }
                }
                NovaHandle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    panel: card.panel
                    source: macro.source
                    glyphSize: card.handleGlyph
                }
            }
        }
    }
}
