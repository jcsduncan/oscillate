pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The transport's Loop button. The range is whole bars, set by dragging on
// the arrangement ruler; the button turns looping on and off and names the
// range. A late or seeking wrap tints its border amber, with the reason in
// its accessible description.
Rectangle {
    id: loop
    objectName: "loopToggle"
    required property var transport
    property color raisedColor: Theme.panelRaised
    property color lineColor: Theme.loopEdge
    property color mainTextColor: Theme.textMain
    property color dimTextColor: Theme.textDim
    property color accentColor: Theme.accent
    property color amberColor: Theme.amber
    // Bars before bar 1 (an intro); the range is named as the ruler names
    // bars. The transport's end bar is exclusive, so the last looped bar is
    // one before it.
    property int leadBars: 0
    readonly property int firstShown: transport.startBar - leadBars
    readonly property int lastShown: transport.endBar - 1 - leadBars
    readonly property bool troubled: transport.lateWraps > 0
        || transport.status.indexOf("seek") >= 0 || transport.status.indexOf("hold") >= 0
    readonly property color tint: transport.enabled ? amberColor : dimTextColor

    width: row.implicitWidth + Theme.space2xl
    height: Theme.loopHeight
    radius: Theme.radiusXl
    color: transport.enabled ? Qt.rgba(amberColor.r, amberColor.g, amberColor.b, Theme.loopActiveWashAlpha) : raisedColor
    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
    border.color: activeFocus ? mainTextColor : troubled ? amberColor : lineColor
    activeFocusOnTab: true
    Accessible.role: Accessible.CheckBox
    Accessible.name: "Loop bars " + firstShown + " to " + lastShown
    Accessible.checked: transport.enabled
    Accessible.description: transport.lateWraps > 0
        ? transport.status + " · " + transport.lateWraps + " late wrap(s)" : transport.status
    Keys.onReturnPressed: transport.set_enabled(!transport.enabled)
    Keys.onSpacePressed: transport.set_enabled(!transport.enabled)

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceSm
        Canvas {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.loopIconWidth
            height: Theme.loopIconHeight
            readonly property color stroke: loop.tint
            // The two arrows of the loop glyph, as [x, y] points in the canvas.
            readonly property var upperArrow: [[3, 3], [12, 3], [10, 1]]
            readonly property var lowerArrow: [[13, 9], [4, 9], [6, 11]]
            onStrokeChanged: requestPaint()
            onPaint: {
                const context = getContext("2d")
                context.clearRect(0, 0, width, height)
                context.strokeStyle = stroke
                context.lineWidth = Theme.loopIconStroke
                context.lineCap = "round"
                context.lineJoin = "round"
                context.beginPath()
                for (const arrow of [upperArrow, lowerArrow]) {
                    context.moveTo(arrow[0][0], arrow[0][1])
                    for (let point = 1; point < arrow.length; ++point)
                        context.lineTo(arrow[point][0], arrow[point][1])
                }
                context.stroke()
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Loop"
            color: loop.tint
            font.pixelSize: Theme.typeLabel
            font.weight: Font.DemiBold
        }
        Text {
            objectName: "loopRange"
            anchors.verticalCenter: parent.verticalCenter
            visible: loop.transport.enabled
            text: loop.firstShown === loop.lastShown ? String(loop.firstShown)
                : loop.firstShown + "–" + loop.lastShown
            color: loop.tint
            opacity: Theme.loopRangeOpacity
            font.pixelSize: Theme.typeBody
        }
    }
    MouseArea {
        Tip {
            text: (loop.transport.enabled ? "Stop looping" : "Loop") + " bars " + loop.firstShown
                  + " to " + loop.lastShown
            shortcut: "Ctrl+L"
        }
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: loop.transport.set_enabled(!loop.transport.enabled)
    }
}
