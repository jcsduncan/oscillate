pragma ComponentBehavior: Bound
import QtQuick
import "shell"
import "devicekit" as Kit

// The foot of a clip's context menu: a colour row (the track palette, and
// "follow the track") and, for audio clips, a name field.
Item {
    id: extras
    required property var bridge
    required property string clipId
    required property string clipColor // "" follows the track.
    required property string clipName
    required property bool nameable
    required property color mainTextColor
    required property color dimTextColor
    required property color lineColor
    signal done()
    readonly property Kit.ShellTheme kit: Kit.ShellTheme {}
    // Palette colours offered after "follow the track", as fit the menu.
    readonly property int paletteShown: 9
    width: parent ? parent.width : Theme.clipMenuWidth
    implicitHeight: (nameable ? Theme.clipMenuRowHeight : 0) + Theme.clipMenuRowHeight

    Rectangle {
        visible: extras.nameable
        x: Theme.spaceMd
        y: Theme.spaceXs
        width: parent.width - 2 * Theme.spaceMd
        height: Theme.clipMenuFieldHeight
        radius: Theme.clipMenuFieldRadius
        color: Theme.clipMenuFieldGround
        border.color: nameInput.activeFocus ? extras.kit.accent : extras.lineColor
        TextInput {
            id: nameInput
            objectName: "clipNameInput"
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceMd
            anchors.rightMargin: Theme.spaceMd
            verticalAlignment: TextInput.AlignVCenter
            text: extras.clipName
            color: extras.mainTextColor
            font.pixelSize: Theme.typeBody
            selectByMouse: true
            clip: true
            Accessible.role: Accessible.EditableText
            Accessible.name: "Clip name"
            Keys.onReturnPressed: commit()
            Keys.onEnterPressed: commit()
            Keys.onEscapePressed: extras.done()
            function commit() {
                if (text.trim() !== extras.clipName)
                    extras.bridge.set_audio_clip_name(extras.clipId, text.trim())
                extras.done()
            }
        }
        Text {
            visible: nameInput.text.length === 0 && !nameInput.activeFocus
            x: Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            text: "Name this clip…"
            color: extras.dimTextColor
            font.pixelSize: Theme.typeBody
        }
    }
    Row {
        x: Theme.clipMenuSwatchX
        y: (extras.nameable ? Theme.clipMenuRowHeight : 0) + Theme.spaceMd
        spacing: Theme.clipMenuSwatchSpacing
        Repeater {
            model: [""].concat(extras.kit.trackPalette.slice(0, extras.paletteShown))
            delegate: Rectangle {
                id: swatch
                required property string modelData
                readonly property bool chosen: extras.clipColor.toLowerCase() === modelData.toLowerCase()
                width: Theme.clipMenuSwatchSize
                height: Theme.clipMenuSwatchSize
                radius: width / 2
                color: modelData.length > 0 ? modelData : "transparent"
                border.color: chosen ? extras.mainTextColor : modelData.length > 0 ? "transparent" : extras.dimTextColor
                border.width: chosen ? Theme.strokeBold : Theme.hairline
                Accessible.role: Accessible.Button
                Accessible.name: modelData.length > 0 ? "Clip colour " + modelData : "Use the track's colour"
                Text {
                    visible: swatch.modelData.length === 0
                    anchors.centerIn: parent
                    text: "∅"
                    color: extras.dimTextColor
                    font.pixelSize: Theme.typeSmall
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        extras.bridge.set_clip_color(extras.clipId, swatch.modelData)
                        extras.done()
                    }
                }
            }
        }
    }
}
