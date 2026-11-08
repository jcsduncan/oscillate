pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Every keyboard shortcut, grouped. DawShell binds them; keep this list and
// its Shortcut objects in step.
Item {
    id: overlay
    Reveal { target: overlay }
    required property color panelColor
    required property color raisedColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    property string monoFamily: Theme.monoFamily
    signal closeRequested()

    // The card, centred, with two columns of groups when it is wide enough.
    readonly property int windowInset: 48
    readonly property int cardMaxWidth: 880
    readonly property int cardChrome: 72
    readonly property real cardRadius: 14
    readonly property int titleX: 28
    readonly property int titleY: 22
    readonly property int closeInset: 18
    readonly property int closeWidth: 64
    readonly property int closeHeight: 30
    readonly property real closeRadius: 7
    readonly property int listY: 64
    readonly property int listBottomInset: 16
    readonly property int twoColumnWidth: 760
    readonly property int columnGap: 32
    readonly property int groupGap: 22
    readonly property int entryHeight: 28
    readonly property int keyPadding: 14
    readonly property int keyHeight: 22

    readonly property var groups: [
        { title: "Editing", items: [
            ["Ctrl+D", "Duplicate the selected notes, clips or track"],
            ["Ctrl+C / Ctrl+X / Ctrl+V", "Copy, cut, paste at the playhead"],
            ["Delete / Backspace", "Delete the selected notes, or else the selected clips"],
            ["Ctrl+A", "Select every clip on every track, or every note in the piano roll"],
            ["Shift/Ctrl-click", "Add or remove a note or clip from the selection"],
            ["Drag on empty lanes", "Select every clip in the box"],
            ["Drag a selected clip", "Move every selected clip by the same snapped amount"],
            ["Left / Right", "Move the selected clips a beat earlier / later"],
            ["Ctrl+J", "Merge the selected MIDI clips on one track"],
            ["Shift+Alt-drag", "Slip an audio clip's source under it"],
            ["Escape", "Let go of the selected notes or clips"],
            ["Ctrl+E", "Split the selected clip at the playhead"],
            ["Ctrl+U", "Quantize the selected MIDI clip"],
            ["A / B", "Show automation on the selected track's clips / draw it"],
            ["Ctrl+Z / Ctrl+Shift+Z", "Undo / redo"],
            ["Ctrl+H", "History: go back to any step"]
        ] },
        { title: "Transport", items: [
            ["Space", "Play / pause"],
            ["Ctrl+R", "Record on / off (from stop: count-in, then play)"],
            ["Ctrl+L", "Loop on / off"],
            ["Ctrl+M", "Add a marker at the playhead"],
            ["[ / ]", "Previous / next marker"],
            ["Ctrl+Shift+K", "Computer keyboard on / off"],
            ["A W S E D F T G Y H U J K", "Play from C (keyboard on)"],
            ["A S D F G H J K L ; ' · W E R T Y U I O P", "In key: the scale up from its root, and an octave up"],
            ["Z / X · C / V", "Octave down / up · velocity down / up"]
        ] },
        { title: "Tracks and view", items: [
            ["Ctrl+T", "New instrument track"],
            ["M / S", "Mute / solo the selected track"],
            ["+ / −", "Zoom in / out"],
            ["Ctrl+Shift+P", "Open the VST3 plug-ins"]
        ] },
        { title: "Project", items: [
            ["Ctrl+S", "Save"],
            ["Ctrl+I / Ctrl+Shift+I", "Import audio / MIDI"],
            ["Ctrl+Shift+E", "Export: choose the file, format and rate"],
            ["Ctrl+Shift+M", "Mix Check: what pokes out in the mix, and a fix"],
            ["Ctrl+K", "Command palette: run anything by name"],
            ["? or F1", "Show these shortcuts"]
        ] }
    ]

    Rectangle {
        anchors.fill: parent
        color: Theme.shortcutsScrim
        MouseArea { anchors.fill: parent; onClicked: overlay.closeRequested() }
    }
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width - overlay.windowInset, overlay.cardMaxWidth)
        height: Math.min(parent.height - overlay.windowInset, content.implicitHeight + overlay.cardChrome)
        radius: overlay.cardRadius
        color: overlay.panelColor
        border.color: overlay.lineColor
        MouseArea { anchors.fill: parent } // Clicks inside do not close.
        Accessible.role: Accessible.Dialog
        Accessible.name: "Keyboard shortcuts"
        Text {
            x: overlay.titleX
            y: overlay.titleY
            text: "Keyboard shortcuts"
            color: overlay.textColor
            font.pixelSize: Theme.typeDisplay
            font.weight: Font.DemiBold
        }
        Rectangle {
            objectName: "shortcutsCloseButton"
            anchors.right: parent.right
            anchors.rightMargin: overlay.closeInset
            y: overlay.closeInset
            width: overlay.closeWidth
            height: overlay.closeHeight
            radius: overlay.closeRadius
            color: overlay.raisedColor
            border.color: activeFocus ? overlay.accentColor : overlay.lineColor
            activeFocusOnTab: overlay.visible
            Accessible.role: Accessible.Button
            Accessible.name: "Close shortcuts"
            Keys.onReturnPressed: overlay.closeRequested()
            Keys.onSpacePressed: overlay.closeRequested()
            Text { anchors.centerIn: parent; text: "Close"; color: overlay.textColor; font.pixelSize: Theme.typeLabel }
            MouseArea {
                anchors.fill: parent
                onClicked: overlay.closeRequested()
                Tip {}
            }
        }
        Flickable {
            x: overlay.titleX
            y: overlay.listY
            width: card.width - 2 * overlay.titleX
            height: card.height - overlay.listY - overlay.listBottomInset
            contentHeight: content.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Grid {
                id: content
                width: parent.width
                columns: card.width >= overlay.twoColumnWidth ? 2 : 1
                columnSpacing: overlay.columnGap
                rowSpacing: overlay.groupGap
                Repeater {
                    model: overlay.groups
                    delegate: Column {
                        id: group
                        required property var modelData
                        width: (content.width - (content.columns - 1) * content.columnSpacing) / content.columns
                        spacing: Theme.spaceSm
                        Text {
                            text: group.modelData.title.toUpperCase()
                            color: overlay.dimColor
                            font.pixelSize: Theme.typeSmall
                            font.letterSpacing: Theme.trackingMd
                        }
                        Repeater {
                            model: group.modelData.items
                            delegate: Item {
                                id: entry
                                required property var modelData
                                width: group.width
                                height: overlay.entryHeight
                                Accessible.role: Accessible.StaticText
                                Accessible.name: entry.modelData[1] + ": " + entry.modelData[0]
                                Rectangle {
                                    id: keys
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: keyText.implicitWidth + overlay.keyPadding
                                    height: overlay.keyHeight
                                    radius: Theme.controlRadius
                                    color: overlay.raisedColor
                                    border.color: overlay.lineColor
                                    Text {
                                        id: keyText
                                        anchors.centerIn: parent
                                        text: entry.modelData[0]
                                        color: overlay.accentColor
                                        font.pixelSize: Theme.typeBody
                                        font.family: overlay.monoFamily
                                    }
                                }
                                Text {
                                    anchors.left: keys.right
                                    anchors.leftMargin: Theme.spaceLg
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    elide: Text.ElideRight
                                    text: entry.modelData[1]
                                    color: overlay.textColor
                                    font.pixelSize: Theme.typeLabel
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
