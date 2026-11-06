pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: card
    required property string trackName
    required property string engineName
    required property string engineId
    required property var engines
    signal openRequested()
    signal changeRequested(string engineId)
    objectName: "nativeSynthSourceCard"
    // Rows from the top: title, track, about, switch buttons, open button.
    readonly property int titleY: 18
    readonly property int trackY: 56
    readonly property int aboutY: 89
    readonly property int switchY: 137
    readonly property int openY: 188
    readonly property int switchHeight: 35
    readonly property int openHeight: 42
    readonly property int openRadius: 7
    readonly property int switchTextPadding: 10
    radius: Theme.panelRadius
    color: Theme.synthCardFill
    border.color: Theme.synthCardEdge

    Text {
        x: Theme.spaceXl; y: card.titleY
        text: card.engineName.toUpperCase()
        color: Theme.synthCardTitle
        font.pixelSize: Theme.synthCardTitleSize
        font.bold: true
        font.letterSpacing: Theme.tracking3xl
    }
    Text {
        x: Theme.spaceXl; y: card.trackY; width: parent.width - 2 * Theme.spaceXl
        text: card.trackName + " · built-in synthesizer"
        color: Theme.synthCardText
        font.pixelSize: Theme.typeLabel
        elide: Text.ElideRight
    }
    Text {
        x: Theme.spaceXl; y: card.aboutY; width: parent.width - 2 * Theme.spaceXl
        text: "Oscillators, filter, envelopes and modulation. Edits can be made during playback."
        color: Theme.synthCardTextDim
        font.pixelSize: Theme.typeBody
        wrapMode: Text.WordWrap
    }
    Row {
        id: switchChoices
        x: Theme.spaceXl; y: card.switchY; width: parent.width - 2 * Theme.spaceXl; height: card.switchHeight
        spacing: Theme.spaceSm
        Repeater {
            model: card.engines
            delegate: Rectangle {
                required property var modelData
                visible: modelData.id !== card.engineId
                width: visible
                       ? (switchChoices.width - switchChoices.spacing * (card.engines.length - 1))
                         / Math.max(1, card.engines.length - 1)
                       : 0
                height: card.switchHeight
                radius: Theme.radiusLg
                color: switchMouse.containsMouse ? Theme.synthCardButtonHover : Theme.synthCardButton
                border.color: Theme.synthCardButtonEdge
                Text {
                    anchors.centerIn: parent
                    width: parent.width - card.switchTextPadding
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: "SWITCH TO " + modelData.name.toUpperCase()
                    color: Theme.synthCardText
                    font.pixelSize: Theme.typeBody
                    font.bold: true
                }
                MouseArea {
                    id: switchMouse
                    objectName: "switchNativeSynthButton"
                    anchors.fill: parent
                    hoverEnabled: true
                    activeFocusOnTab: true
                    cursorShape: Qt.PointingHandCursor
                    Accessible.role: Accessible.Button
                    Accessible.name: "Switch synthesizer to " + parent.modelData.name
                    onClicked: card.changeRequested(parent.modelData.id)
                    Keys.onReturnPressed: card.changeRequested(parent.modelData.id)
                    Keys.onSpacePressed: card.changeRequested(parent.modelData.id)
                }
            }
        }
    }
    Rectangle {
        x: Theme.spaceXl; y: card.openY; width: parent.width - 2 * Theme.spaceXl; height: card.openHeight
        radius: card.openRadius
        color: openMouse.containsMouse ? Theme.synthCardOpenHover : Theme.synthCardOpen
        border.color: Theme.synthCardOpenEdge
        Text {
            anchors.centerIn: parent
            text: "OPEN " + card.engineName.toUpperCase()
            color: Theme.synthCardOpenText
            font.bold: true
            font.pixelSize: Theme.typeLabel
        }
        MouseArea {
            id: openMouse
            objectName: "openNativeSynthButton"
            anchors.fill: parent
            hoverEnabled: true
            activeFocusOnTab: true
            cursorShape: Qt.PointingHandCursor
            Accessible.role: Accessible.Button
            Accessible.name: "Open " + card.engineName + " instrument editor"
            onClicked: card.openRequested()
            Keys.onReturnPressed: card.openRequested()
            Keys.onSpacePressed: card.openRequested()
        }
    }
}
