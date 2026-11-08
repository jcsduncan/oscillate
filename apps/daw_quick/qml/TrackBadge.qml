import QtQuick
import "shell"

// A small rounded label in a colour: a bus's member count, a return's
// effect (filled), or a send pill naming the return it feeds (outlined).
Rectangle {
    id: badge
    required property string text
    required property color tone
    property bool outlined: false
    width: label.implicitWidth + (outlined ? Theme.trackSendPillPadding : Theme.trackBusBadgePadding)
    height: outlined ? Theme.trackSendPillHeight : Theme.trackBadgeHeight
    radius: height / 2
    color: outlined ? "transparent" : Qt.rgba(tone.r, tone.g, tone.b, Theme.trackBusBadgeAlpha)
    border.width: outlined ? Theme.hairline : 0
    border.color: Qt.rgba(tone.r, tone.g, tone.b, Theme.trackSendPillEdgeAlpha)
    Accessible.role: Accessible.StaticText
    Accessible.name: badge.text
    Text {
        id: label
        anchors.centerIn: parent
        text: badge.text
        color: badge.tone
        font.pixelSize: Theme.typeMicro
        font.bold: true
    }
}
