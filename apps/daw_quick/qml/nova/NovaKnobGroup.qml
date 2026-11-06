pragma ComponentBehavior: Bound
import QtQuick

// A group of secondary controls on an overview card (ADR-0076 revision 2):
// a faint label turned on its side ("UNISON", "PITCH"), then the group's
// small knobs and any dropdowns in a row. A group without a label shows only
// the row. Groups sit side by side in a card's Flow and wrap with it.
Row {
    id: group
    required property var panel
    property string label: ""
    default property alias content: controls.data
    readonly property real labelSize: panel.theme.typeMicro
    readonly property real labelSpacing: panel.theme.knobGroupLabelSpacing
    readonly property real labelGap: panel.theme.spaceXs
    readonly property real controlGap: panel.theme.spaceXxs

    objectName: "novaKnobGroup:" + label
    spacing: labelGap

    // The label reads bottom to top beside the controls.
    Item {
        visible: group.label !== ""
        width: caption.implicitHeight
        height: controls.height
        Text {
            id: caption
            anchors.centerIn: parent
            rotation: -90
            text: group.label
            color: group.panel.theme.faint
            font.family: group.panel.theme.mono
            font.pixelSize: group.labelSize
            font.weight: Font.DemiBold
            font.letterSpacing: group.labelSpacing
        }
    }
    Row {
        id: controls
        spacing: group.controlGap
    }
}
