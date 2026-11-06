pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// One of Knock's eight pads, named by its sound ("Wood · Stick") with its
// key. A click (or Space / Return) selects it and plays it; every hit,
// played here or arriving from the running track, flashes it in the strike
// colour, and it glows with its own output while it sounds.
Item {
    id: pad
    required property KnockTheme theme
    required property int index
    required property string label
    required property string keyName
    required property bool selected
    // The pad's output peak 0..1 while the synth runs (knock.hpp telemetry).
    property real level: 0
    // Hits flashed since the panel opened.
    property int hits: 0
    // Pads per row, so Up and Down walk a column.
    property int columns: 4
    signal pressed()
    signal walked(int direction)

    function flash() {
        hits += 1
        fade.restart()
    }

    objectName: "knockPad:" + index
    implicitWidth: theme.padWidth
    implicitHeight: theme.padHeight
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: "Pad " + (index + 1) + ", " + label + ", " + keyName
    Accessible.description: (selected ? "Selected. " : "") + "Click to select and play"
    Accessible.onPressAction: pressed()
    Keys.onSpacePressed: pressed()
    Keys.onReturnPressed: pressed()
    Keys.onLeftPressed: walked(-1)
    Keys.onRightPressed: walked(1)
    Keys.onUpPressed: walked(-columns)
    Keys.onDownPressed: walked(columns)

    Rectangle {
        id: face
        anchors.fill: parent
        radius: pad.theme.padRadius
        color: area.containsMouse ? pad.theme.padHover : pad.theme.panel2
        border.width: pad.selected || pad.activeFocus ? pad.theme.strokeBold : pad.theme.hairline
        border.color: pad.selected ? pad.theme.strike : pad.activeFocus ? pad.theme.line2 : pad.theme.line
        clip: true

        // Lights with the pad's own output as it sounds.
        Rectangle {
            objectName: "knockPadLive:" + pad.index
            anchors.fill: parent
            radius: parent.radius
            color: pad.theme.strike
            opacity: pad.theme.padLiveOpacity * Math.max(0, Math.min(1, pad.level))
        }
        // A hit's flash.
        Rectangle {
            objectName: "knockPadFlash:" + pad.index
            anchors.fill: parent
            radius: parent.radius
            color: pad.theme.strike
            opacity: 0
            NumberAnimation on opacity {
                id: fade
                running: false
                from: pad.theme.flashOpacity
                to: 0
                duration: pad.theme.flashMs
                easing.type: Easing.OutQuad
            }
        }
        Text {
            objectName: "knockPadLabel:" + pad.index
            x: pad.theme.padInset
            y: pad.theme.spaceMd
            width: parent.width - 2 * pad.theme.padInset
            text: pad.label
            color: pad.theme.ink
            font.family: pad.theme.sans
            font.pixelSize: pad.theme.typeEmphasis
            font.bold: true
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
        Text {
            x: pad.theme.padInset
            anchors.bottom: parent.bottom
            anchors.bottomMargin: pad.theme.padFootInset
            text: pad.keyName
            color: pad.theme.inkFaint
            font.family: pad.theme.mono
            font.pixelSize: pad.theme.typeSmall
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: pad.theme.padInset
            anchors.bottom: parent.bottom
            anchors.bottomMargin: pad.theme.padFootInset
            text: pad.index + 1
            color: pad.selected ? pad.theme.strike : pad.theme.inkFaint
            font.family: pad.theme.mono
            font.pixelSize: pad.theme.typeSmall
            font.bold: true
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: {
            pad.forceActiveFocus()
            pad.pressed()
        }
        Kit.HoverTip { theme: pad.theme }
    }
}
