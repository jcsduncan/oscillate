import QtQuick
import "shell"

// A group bus's fold: points down while its members show, right while
// they hide under it.
Item {
    id: chevron
    required property bool collapsed
    required property string busName
    required property color lineColor
    required property color accentColor
    signal toggled()
    width: Theme.trackBusChevronSize
    height: Theme.trackBusChevronSize
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: (collapsed ? "Expand " : "Collapse ") + busName + " bus"
    Keys.onReturnPressed: toggled()
    Keys.onSpacePressed: toggled()
    Canvas {
        id: glyph
        anchors.fill: parent
        readonly property bool down: !chevron.collapsed
        readonly property color stroke: area.containsMouse || chevron.activeFocus ? chevron.accentColor
                                                                                   : chevron.lineColor
        onDownChanged: requestPaint()
        onStrokeChanged: requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            const inset = width * Theme.trackBusChevronInset
            const middle = width / 2
            context.strokeStyle = stroke
            context.lineWidth = Theme.trackBusChevronStroke
            context.lineCap = "round"
            context.lineJoin = "round"
            context.beginPath()
            if (down) {
                context.moveTo(inset, middle - inset / 2)
                context.lineTo(middle, middle + inset / 2)
                context.lineTo(width - inset, middle - inset / 2)
            } else {
                context.moveTo(middle - inset / 2, inset)
                context.lineTo(middle + inset / 2, middle)
                context.lineTo(middle - inset / 2, height - inset)
            }
            context.stroke()
        }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: chevron.toggled()
    }
}
