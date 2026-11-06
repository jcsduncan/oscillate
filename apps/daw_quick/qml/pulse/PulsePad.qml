pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// One big instrument pad with its select LED. A click (or Space / Return)
// selects the pad for editing, flashes it and asks for a trigger.
Item {
    id: pad
    required property PulseTheme theme
    required property int index
    required property string name
    required property string keyName
    required property bool selected
    // The pad's live envelope 0..1 while the synth runs (ADR-0037).
    property real level: 0
    signal pressed()

    readonly property color tint: theme.padColor(index)

    // Every press flashes the pad, whatever sent it.
    onPressed: fade.restart()

    objectName: "pulsePad:" + index
    implicitWidth: theme.padWidth
    implicitHeight: theme.padHeight
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Kit.HoverTip { theme: pad.theme }
    Accessible.name: name + " pad, " + keyName
    Accessible.description: (selected ? "Selected. " : "") + "Click to select and play"
    Accessible.onPressAction: pressed()
    Keys.onSpacePressed: pressed()
    Keys.onReturnPressed: pressed()

    // Select LED, as on a step sequencer.
    Rectangle {
        objectName: "pulsePadLed:" + pad.index
        x: (parent.width - width) / 2
        y: 0
        width: pad.theme.padLedSize
        height: pad.theme.padLedSize
        radius: width / 2
        color: pad.selected ? pad.theme.led : pad.theme.ledOff
        border.color: Qt.darker(color, pad.theme.padLedRimDarken)
        Rectangle {
            visible: pad.selected
            anchors.centerIn: parent
            width: pad.theme.padLedHaloSize
            height: pad.theme.padLedHaloSize
            radius: width / 2
            z: -1
            color: pad.theme.padLedHalo
        }
    }

    Rectangle {
        id: shadow
        x: face.x + pad.theme.padShadowX
        y: face.y + pad.theme.padShadowDrop
        width: face.width
        height: face.height
        radius: face.radius
        color: pad.theme.padShadow
    }
    // A ring in the LED colour while the pad sounds, readable on every tint.
    Rectangle {
        objectName: "pulsePadRing:" + pad.index
        x: face.x - pad.theme.padRingOutset
        y: face.y - pad.theme.padRingOutset
        width: face.width + 2 * pad.theme.padRingOutset
        height: face.height + 2 * pad.theme.padRingOutset
        radius: face.radius + pad.theme.padRingOutset
        color: "transparent"
        border.width: pad.theme.strokeBold
        border.color: pad.theme.led
        opacity: Math.max(0, Math.min(1, pad.level))
    }
    Rectangle {
        id: face
        x: pad.theme.padFaceInset
        y: pad.theme.padFaceTop + (area.pressed ? pad.theme.padPressTravel : 0)
        width: parent.width - 2 * pad.theme.padFaceInset
        height: parent.height - pad.theme.padFaceTrim
        radius: pad.theme.padRadius
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(pad.tint, pad.theme.padSheen) }
            GradientStop { position: pad.theme.padGradientMid; color: pad.tint }
            GradientStop { position: 1; color: Qt.darker(pad.tint, pad.theme.padShade) }
        }
        border.width: pad.activeFocus ? pad.theme.strokeBold : pad.theme.hairline
        border.color: pad.activeFocus ? pad.theme.ink : Qt.darker(pad.tint, pad.theme.padRimDarken)
        // Highlight lip along the top edge.
        Rectangle {
            x: pad.theme.padLipInset
            y: pad.theme.padLipTop
            width: parent.width - 2 * pad.theme.padLipInset
            height: pad.theme.padLipHeight
            radius: height / 2
            color: pad.theme.padLip
        }
        // Lights with the pad's voice as it sounds.
        Rectangle {
            objectName: "pulsePadLive:" + pad.index
            anchors.fill: parent
            radius: parent.radius
            color: pad.theme.padFlash
            opacity: pad.theme.padLiveOpacity * Math.max(0, Math.min(1, pad.level))
        }
        Rectangle {
            id: glow
            objectName: "pulsePadGlow"
            anchors.fill: parent
            radius: parent.radius
            color: pad.theme.padFlash
            opacity: 0
            NumberAnimation on opacity {
                id: fade
                running: false
                from: pad.theme.padFlashOpacity
                to: 0
                duration: pad.theme.padFlashMs
                easing.type: Easing.OutQuad
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: pad.theme.spaceSm
            text: pad.index + 1
            color: pad.theme.padNumberInk
            font.pixelSize: pad.theme.typeEmphasis
            font.bold: true
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onPressed: {
            pad.forceActiveFocus()
            pad.pressed()
        }
    }
}
