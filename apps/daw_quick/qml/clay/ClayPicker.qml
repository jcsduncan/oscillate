import QtQuick
import "../shell"

// A compact chooser: ‹ current ›, stepping through `options`
// ([{value, label}]). Clicking the name steps forward too.
Rectangle {
    id: picker
    required property ClayTheme style
    property var options: []
    property var value: options.length > 0 ? options[0].value : ""
    property string tip: ""
    signal chosen(var value)

    readonly property int index: {
        for (let i = 0; i < options.length; ++i) {
            if (options[i].value === value)
                return i
        }
        return -1
    }
    readonly property string shown: index >= 0 ? options[index].label : "—"
    function stepBy(direction) {
        if (options.length === 0)
            return
        const next = ((index < 0 ? 0 : index + direction) % options.length + options.length) % options.length
        chosen(options[next].value)
    }

    implicitWidth: style.pickerWidth
    implicitHeight: style.controlHeight
    radius: style.radius
    color: style.well
    border.color: activeFocus ? style.clay2 : style.line2
    opacity: enabled ? 1 : style.disabledOpacity
    activeFocusOnTab: enabled && visible
    Accessible.role: Accessible.ComboBox
    Accessible.name: (tip.length > 0 ? tip + ": " : "") + shown
    Keys.onLeftPressed: stepBy(-1)
    Keys.onRightPressed: stepBy(1)
    Keys.onUpPressed: stepBy(-1)
    Keys.onDownPressed: stepBy(1)

    Text {
        id: back
        x: picker.style.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        text: "‹"
        color: backArea.containsMouse ? picker.style.ink : picker.style.faint
        font.pixelSize: picker.style.typeHeading
        MouseArea { id: backArea; anchors.fill: parent; anchors.margins: -picker.style.spaceSm; hoverEnabled: true
                    onClicked: picker.stepBy(-1) }
    }
    Text {
        anchors.left: back.right; anchors.leftMargin: picker.style.spaceMd
        anchors.right: forward.left; anchors.rightMargin: picker.style.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        horizontalAlignment: Text.AlignHCenter
        text: picker.shown
        elide: Text.ElideRight
        color: picker.style.ink
        font.family: picker.style.sans
        font.pixelSize: picker.style.textSize
        font.weight: Font.DemiBold
        MouseArea { anchors.fill: parent; onClicked: picker.stepBy(1)
                    Tip { text: picker.tip.length > 0 ? picker.tip : picker.shown } }
    }
    Text {
        id: forward
        anchors.right: parent.right; anchors.rightMargin: picker.style.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        text: "›"
        color: forwardArea.containsMouse ? picker.style.ink : picker.style.faint
        font.pixelSize: picker.style.typeHeading
        MouseArea { id: forwardArea; anchors.fill: parent; anchors.margins: -picker.style.spaceSm; hoverEnabled: true
                    onClicked: picker.stepBy(1) }
    }
}
