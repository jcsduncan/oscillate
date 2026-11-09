import QtQuick

// The panel's one status badge, in its header: whether edits are live,
// updating, unsaved, deferred to new notes, not applied or read only. It
// says so in words with a lamp in the matching tone (never colour alone),
// and its tooltip gives the detail. It is a status, not a switch: it takes
// no focus and no clicks.
Rectangle {
    id: status
    required property var controller
    required property KitTheme theme
    // Marks the badge for the panel contracts: one per panel.
    readonly property bool deviceStatus: true
    readonly property bool problem: !controller.editable || controller.status_text === "Not applied"
    readonly property color tone: problem || controller.deferred ? theme.warning
        : controller.pending ? theme.statusPendingInk : theme.live

    objectName: theme.prefix + "Status"
    width: statusText.implicitWidth + theme.statusPad
    height: theme.statusHeight
    radius: theme.statusHeight / 2
    color: theme.statusFill(tone)
    border.color: Qt.rgba(tone.r, tone.g, tone.b, theme.statusEdgeAlpha)
    Accessible.role: Accessible.StaticText
    Accessible.name: "Status: " + controller.status_text
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
        objectName: status.theme.prefix + "StatusText"
        x: status.theme.statusTextX
        anchors.verticalCenter: parent.verticalCenter
        text: status.controller.status_text
        color: status.theme.statusInk(status.tone)
        font.pixelSize: status.theme.statusTextSize
        font.bold: true
    }
    Tip {
        theme: status.theme
        shown: hover.hovered
        text: "Status: " + status.controller.status_text
        detail: status.controller.status_detail
    }
    HoverHandler {
        id: hover
    }
}
