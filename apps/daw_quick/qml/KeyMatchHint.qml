pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Smart drops (ADR-0059): a clip just dropped into a song with a key is
// offered a move into that key. A quiet pill over the arrangement, never a
// dialog: "Move" applies one undoable pitch edit, × lets it be. It shows
// only while bridge.keyMatch.hint has text.
Rectangle {
    id: hint
    objectName: "keyMatchHint"
    required property var bridge
    readonly property var controller: bridge.keyMatch || null
    readonly property var offer: controller ? controller.hint : ({})
    readonly property bool shown: Boolean(offer && offer.text)

    visible: shown
    transformOrigin: Item.Bottom
    Reveal { target: hint; fromScale: Motion.popScale }
    width: Math.min(Theme.keyMatchMaxWidth, row.implicitWidth + Theme.space2xl)
    height: Theme.keyMatchHeight
    radius: Theme.keyMatchRadius
    color: Theme.panelRaised
    border.color: Theme.keyMatchEdge
    Accessible.role: Accessible.AlertMessage
    Accessible.name: shown ? offer.text : ""

    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        x: Theme.keyMatchInset
        spacing: Theme.keyMatchSpacing
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Theme.keyMatchTextMaxWidth)
            elide: Text.ElideRight
            text: hint.shown ? hint.offer.text : ""
            color: Theme.keyMatchText
            font.pixelSize: Theme.typeBody
        }
        Rectangle {
            objectName: "keyMatchApply"
            anchors.verticalCenter: parent.verticalCenter
            width: applyText.implicitWidth + Theme.keyMatchButtonPadding
            height: Theme.keyMatchButtonHeight
            radius: Theme.keyMatchButtonRadius
            color: applyArea.containsMouse || activeFocus ? Theme.accent : Theme.keyMatchButtonFill
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            activeFocusOnTab: hint.shown
            Accessible.role: Accessible.Button
            Accessible.name: "Move the clip to " + (hint.shown ? hint.offer.to : "the song's key")
            Keys.onReturnPressed: hint.controller.apply()
            Keys.onSpacePressed: hint.controller.apply()
            Text {
                id: applyText
                anchors.centerIn: parent
                text: "Move"
                color: applyArea.containsMouse || parent.activeFocus ? Theme.canvas : Theme.accent
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                font.pixelSize: Theme.typeBody
                font.bold: true
            }
            MouseArea {
                Tip {}
                id: applyArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: hint.controller.apply()
            }
        }
        Text {
            objectName: "keyMatchDismiss"
            anchors.verticalCenter: parent.verticalCenter
            text: "×"
            color: dismissArea.containsMouse ? Theme.textMain : Theme.textFaint
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            font.pixelSize: Theme.typeHeading
            Accessible.role: Accessible.Button
            Accessible.name: "Keep the clip's key"
            MouseArea {
                Tip {}
                id: dismissArea
                anchors.fill: parent
                anchors.margins: -Theme.spaceSm
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: hint.controller.dismiss()
            }
        }
    }
}
