pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The track list's header row: TRACKS and the + Track button, whose menu
// adds an instrument, imports audio or MIDI, or adds a group or return bus.
Rectangle {
    id: row
    objectName: "trackCreationRow"
    signal addInstrument()
    signal importAudio()
    signal importMidi()
    signal addGroup()
    signal addReturn()
    signal addAudio()
    property bool menuOpen: false
    property bool dropHover: false
    readonly property color accent: Theme.accent
    readonly property color dim: Theme.textDim
    height: Theme.timelineHeaderHeight
    color: dropHover ? Theme.trackCreationDropHover : Theme.trackCreationGround

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: Theme.line
    }
    Text {
        x: Theme.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        text: row.dropHover ? "DROP TO CREATE A TRACK" : "TRACKS"
        color: row.dropHover ? row.accent : row.dim
        font.pixelSize: Theme.typeBody
        font.letterSpacing: Theme.trackListHeaderLetterSpacing
    }
    Rectangle {
        id: addButton
        objectName: "addTrackButton"
        anchors.right: parent.right
        anchors.rightMargin: Theme.trackCreationButtonMargin
        anchors.verticalCenter: parent.verticalCenter
        width: addLabel.implicitWidth + Theme.spaceXl
        height: Theme.trackCreationButtonHeight
        radius: Theme.trackCreationButtonRadius
        color: row.menuOpen ? Theme.trackCreationButtonOpen : Theme.panelRaised
        border.color: activeFocus ? row.accent : Theme.trackCreationButtonEdge
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Add track"
        Accessible.description: "Choose an instrument, audio or MIDI import, group or return track"
        function activate() { row.menuOpen = !row.menuOpen }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text {
            id: addLabel
            anchors.centerIn: parent
            text: "+ Track"
            color: Theme.trackCreationButtonText
            font.pixelSize: Theme.typeBody
        }
        MouseArea {
            anchors.fill: parent
            onClicked: addButton.activate()
            Tip {}
        }
    }
    // A press anywhere outside the menu (and its button) closes it; Escape
    // too. Presses on the menu pass through to it.
    MouseArea {
        parent: row.Window.window ? row.Window.window.contentItem : row
        anchors.fill: parent
        z: 1000
        visible: row.menuOpen
        onPressed: mouse => {
            const inMenu = addMenu.contains(mapToItem(addMenu, mouse.x, mouse.y))
            const onButton = addButton.contains(mapToItem(addButton, mouse.x, mouse.y))
            if (inMenu || onButton) {
                mouse.accepted = false
                return
            }
            row.menuOpen = false
        }
    }
    Rectangle {
        id: addMenu
        objectName: "addTrackMenu"
        visible: row.menuOpen
        onVisibleChanged: if (visible) forceActiveFocus()
        Keys.onEscapePressed: row.menuOpen = false
        anchors.right: parent.right
        anchors.rightMargin: Theme.trackCreationButtonMargin
        y: row.height + Theme.trackCreationMenuGap
        z: 10
        width: Theme.trackCreationMenuWidth
        height: optionRepeater.count * Theme.trackCreationOptionHeight + Theme.spaceMd
        radius: Theme.radiusXl
        color: Theme.panelRaised
        border.color: Theme.lineStrong
        Repeater {
            id: optionRepeater
            model: ["Instrument track", "Import audio…", "Import MIDI file…", "Group bus",
                    "Return bus", "Empty audio track"]
            delegate: Rectangle {
                id: option
                objectName: index === 2 ? "importMidiOption" : "addTrackOption"
                required property string modelData
                required property int index
                x: Theme.spaceXs
                y: Theme.spaceXs + index * Theme.trackCreationOptionHeight
                width: parent.width - Theme.spaceMd
                height: Theme.trackCreationOptionHeight
                radius: Theme.trackCreationOptionRadius
                color: activeFocus || optionArea.containsMouse ? Theme.trackCreationOptionHover : "transparent"
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: modelData
                function activate() {
                    row.menuOpen = false
                    if (index === 0) row.addInstrument()
                    else if (index === 1) row.importAudio()
                    else if (index === 2) row.importMidi()
                    else if (index === 3) row.addGroup()
                    else if (index === 4) row.addReturn()
                    else row.addAudio()
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Text {
                    x: Theme.trackCreationOptionTextX
                    anchors.verticalCenter: parent.verticalCenter
                    text: option.modelData
                    color: Theme.textMain
                    font.pixelSize: Theme.typeLabel
                }
                MouseArea {
                    Tip {}
                    id: optionArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: option.activate()
                }
            }
        }
    }
}
