import QtQuick
import "shell"

// The gap a dragged track header opens where it would land: dashed, in the
// bus's colour inside a bus's bracket (its rail running through, lit), or
// the accent colour on the master.
Item {
    id: slot
    required property bool nested
    required property color tone
    required property string text
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(slot.tone.r, slot.tone.g, slot.tone.b, Theme.trackDropSlotAlpha)
    }
    Rectangle {
        visible: slot.nested
        x: Theme.trackBusRailX + (Theme.trackBusRailWidth - Theme.trackBusRailGlowWidth) / 2
        width: Theme.trackBusRailGlowWidth
        height: parent.height
        radius: width / 2
        color: Qt.rgba(slot.tone.r, slot.tone.g, slot.tone.b, Theme.trackBusRailGlowAlpha)
    }
    Rectangle {
        visible: slot.nested
        x: Theme.trackBusRailX
        width: Theme.trackBusRailWidth
        height: parent.height
        color: slot.tone
    }
    Canvas {
        id: outline
        x: (slot.nested ? Theme.trackMemberIndent : 0) + Theme.trackDropSlotInset
        y: Theme.trackDropSlotInset
        width: parent.width - x - Theme.trackDropSlotInset
        height: parent.height - 2 * Theme.trackDropSlotInset
        readonly property color stroke: slot.tone
        onStrokeChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            const half = Theme.trackDropSlotStroke / 2
            context.strokeStyle = stroke
            context.lineWidth = Theme.trackDropSlotStroke
            context.setLineDash(Theme.trackDropSlotDash)
            context.beginPath()
            context.roundedRect(half, half, width - 2 * half, height - 2 * half, Theme.radiusLg, Theme.radiusLg)
            context.stroke()
        }
        Text {
            objectName: "trackDropSlotText"
            x: Theme.spaceMd
            width: parent.width - 2 * Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: slot.text
            color: slot.tone
            font.pixelSize: Theme.typeLabel
            font.weight: Font.DemiBold
        }
    }
}
