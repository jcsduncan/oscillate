pragma ComponentBehavior: Bound
import QtQuick
import "../shell"

// A row of choices, one lit: `model` holds {id, name} (or plain names when
// `ids` is false). Wraps onto further lines when the row is too narrow.
Flow {
    id: chips
    required property AssistantTheme style
    property var model: []
    property string current: ""
    property string tip: ""
    property string prefix: ""
    signal chosen(string id)

    spacing: style.chipGap

    Repeater {
        model: chips.model
        delegate: Rectangle {
            id: chip
            required property var modelData
            readonly property string chipId: typeof modelData === "string" ? modelData : modelData.id
            readonly property string chipName: typeof modelData === "string" ? modelData : modelData.name
            readonly property bool lit: chips.current === chipId
            objectName: chips.prefix + chipId
            width: label.implicitWidth + chips.style.chipPadding
            height: chips.style.chipHeight
            radius: height / 2
            color: lit ? chips.style.accent : (area.containsMouse ? chips.style.buttonFace : chips.style.panelRaised)
            border.color: lit ? chips.style.accent : chips.style.lineStrong
            Accessible.role: Accessible.RadioButton
            Accessible.name: chipName
            Accessible.checked: lit
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Text {
                id: label
                anchors.centerIn: parent
                text: chip.chipName
                color: chip.lit ? chips.style.onAccentText : chips.style.textMain
                font.pixelSize: chips.style.typeBody
            }
            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: chips.chosen(chip.chipId)
                Tip { text: chips.tip }
            }
        }
    }
}
