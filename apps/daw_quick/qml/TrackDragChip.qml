import QtQuick
import "shell"

// Follows a dragged track header: the track (or how many), an arrow, and
// where it would land, in that destination's colour.
Rectangle {
    id: chip
    required property color swatch
    required property string subject
    required property string destination
    required property color tone
    required property color textColor
    required property real maxWidth
    width: Math.min(maxWidth, 2 * Theme.trackDragChipPadding + Theme.trackSwatchSize + subjectText.implicitWidth
                    + Theme.trackDragArrowSize + destinationText.implicitWidth + 3 * content.spacing)
    height: Theme.trackDragChipHeight
    radius: Theme.radiusXl
    color: Theme.trackDragChipGround
    border.color: tone
    Accessible.role: Accessible.StaticText
    Accessible.name: subject + " to " + destination
    Row {
        id: content
        x: Theme.trackDragChipPadding
        width: chip.width - 2 * Theme.trackDragChipPadding
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceSm
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.trackSwatchSize
            height: Theme.trackSwatchSize
            radius: Theme.radiusSm
            color: chip.swatch
        }
        Text {
            id: subjectText
            anchors.verticalCenter: parent.verticalCenter
            text: chip.subject
            color: chip.textColor
            font.pixelSize: Theme.typeEmphasis
            font.weight: Font.DemiBold
        }
        Canvas {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.trackDragArrowSize
            height: Theme.trackDragArrowSize
            readonly property color stroke: chip.textColor
            onStrokeChanged: requestPaint()
            onPaint: {
                const context = getContext("2d")
                context.clearRect(0, 0, width, height)
                const head = width * Theme.trackBusChevronInset
                const middle = height / 2
                context.strokeStyle = stroke
                context.lineWidth = Theme.trackBusChevronStroke
                context.lineCap = "round"
                context.lineJoin = "round"
                context.beginPath()
                context.moveTo(Theme.trackBusChevronStroke, middle)
                context.lineTo(width - Theme.trackBusChevronStroke, middle)
                context.moveTo(width - Theme.trackBusChevronStroke - head, middle - head)
                context.lineTo(width - Theme.trackBusChevronStroke, middle)
                context.lineTo(width - Theme.trackBusChevronStroke - head, middle + head)
                context.stroke()
            }
        }
        Text {
            id: destinationText
            objectName: "trackDragChipDestination"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, content.width - x)
            elide: Text.ElideRight
            text: chip.destination
            color: chip.tone
            font.pixelSize: Theme.typeEmphasis
            font.weight: Font.DemiBold
        }
    }
}
