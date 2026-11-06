import QtQuick

// The panel's status pill: whether edits are live, pending, deferred to new
// notes, or not applied, with a lamp in the matching tone.
Rectangle {
    id: status
    required property var controller
    required property KitTheme theme
    readonly property bool problem: !controller.editable || controller.status_text === "Not applied"
    readonly property color tone: problem || controller.deferred ? theme.warning
        : controller.pending ? theme.statusPendingInk : theme.live

    width: statusText.implicitWidth + theme.statusPad
    height: theme.statusHeight
    radius: theme.statusHeight / 2
    color: theme.statusFill(tone)
    border.color: Qt.rgba(tone.r, tone.g, tone.b, theme.statusEdgeAlpha)
    Accessible.role: Accessible.StaticText
    Accessible.name: controller.status_text
    Accessible.description: controller.status_detail
    Rectangle {
        x: status.theme.statusLampX
        anchors.verticalCenter: parent.verticalCenter
        width: status.theme.statusLampSize
        height: status.theme.statusLampSize
        radius: width / 2
        color: status.tone
    }
    Text {
        id: statusText
        x: status.theme.statusTextX
        anchors.verticalCenter: parent.verticalCenter
        text: status.controller.status_text
        color: status.theme.statusInk(status.tone)
        font.pixelSize: status.theme.statusTextSize
        font.bold: true
    }
}
