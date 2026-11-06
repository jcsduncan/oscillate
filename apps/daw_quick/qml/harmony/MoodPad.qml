pragma ComponentBehavior: Bound
import QtQuick

// The mood pad: dark to bright across (the mode on the song's tonic), still
// to restless up (how hard the chords pull). Dragging rewrites the clip's
// chord notes as it crosses into a new mode or tenth of restlessness; release
// plays the first chord. Arrow keys move the dot by a twentieth.
Item {
    id: pad
    required property HarmonyTheme style
    required property var controller
    // Where the dot rests with no controller.
    readonly property real restingBrightness: 0.3
    readonly property real restingRestlessness: 0.4
    readonly property real brightness: controller ? controller.brightness : restingBrightness
    readonly property real restlessness: controller ? controller.restlessness : restingRestlessness
    readonly property real keyStep: 0.05
    // Brightness stays below 1 so the brightest mode is still a mode.
    readonly property real brightnessCeiling: 0.999
    // The grid divides the field into this many columns.
    readonly property int gridColumns: 6
    // Mode names under the field are abbreviated to this many letters.
    readonly property int modeAbbreviation: 3
    readonly property var corners: [["Tense", 0, 0], ["Uplifting", 1, 0], ["Hypnotic", 0, 1], ["Dreamy", 1, 1]]

    implicitHeight: width + ticks.height + style.spaceXs
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: "Mood: dark to bright across, still to restless up. "
        + (controller ? controller.feel + ", " + controller.modeName : "")
    Keys.onPressed: function(event) {
        if (!pad.controller)
            return
        let dx = 0
        let dy = 0
        if (event.key === Qt.Key_Left) dx = -keyStep
        else if (event.key === Qt.Key_Right) dx = keyStep
        else if (event.key === Qt.Key_Up) dy = keyStep
        else if (event.key === Qt.Key_Down) dy = -keyStep
        else return
        event.accepted = true
        pad.controller.set_mood(Math.min(brightnessCeiling, Math.max(0, brightness + dx)),
                                Math.min(1, Math.max(0, restlessness + dy)))
        pad.controller.finish_mood()
    }

    Rectangle {
        id: field
        width: pad.width
        height: pad.width
        radius: pad.style.radiusXl
        clip: true
        border.color: pad.activeFocus ? pad.style.violet : pad.style.line
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: pad.style.moodDark }
            GradientStop { position: 1; color: pad.style.moodBright }
        }
        Rectangle {
            anchors.fill: parent
            radius: pad.style.radiusXl
            gradient: Gradient {
                GradientStop { position: 0; color: pad.style.alpha(pad.style.pink, pad.style.moodGlowAlpha) }
                GradientStop { position: 1; color: "transparent" }
            }
        }
        Repeater {
            model: pad.gridColumns - 1
            delegate: Rectangle {
                required property int index
                x: (index + 1) * field.width / pad.gridColumns
                width: pad.style.hairline
                height: field.height
                color: pad.style.alpha(pad.style.ink, pad.style.moodGridAlpha)
            }
        }
        Repeater {
            model: pad.corners
            delegate: Text {
                required property var modelData
                x: modelData[1] === 0 ? pad.style.spaceMd : field.width - width - pad.style.spaceMd
                y: modelData[2] === 0 ? pad.style.spaceMd : field.height - height - pad.style.spaceMd
                text: modelData[0]
                color: pad.style.dim
                font.family: pad.style.sans
                font.pixelSize: pad.style.typeBody
                font.weight: Font.DemiBold
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: pad.style.spaceSm
            text: "RESTLESS"
            color: pad.style.faint
            font.family: pad.style.mono
            font.pixelSize: pad.style.typeMicro
            font.letterSpacing: pad.style.captionSpacing
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: pad.style.spaceSm
            text: "STILL"
            color: pad.style.faint
            font.family: pad.style.mono
            font.pixelSize: pad.style.typeMicro
            font.letterSpacing: pad.style.captionSpacing
        }
        Rectangle {
            id: halo
            width: pad.style.moodHaloSize; height: width; radius: width / 2
            x: pad.brightness * field.width - width / 2
            y: (1 - pad.restlessness) * field.height - height / 2
            color: pad.style.alpha(pad.style.violet, pad.style.moodHaloAlpha)
            Rectangle {
                anchors.centerIn: parent
                width: pad.style.moodDotSize; height: width; radius: width / 2
                color: pad.style.violet
                border.color: pad.style.ink
                border.width: pad.style.strokeBold
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.CrossCursor
            preventStealing: true
            function move(mouse) {
                if (!pad.controller)
                    return
                pad.controller.set_mood(Math.min(pad.brightnessCeiling, Math.max(0, mouse.x / width)),
                                        Math.min(1, Math.max(0, 1 - mouse.y / height)))
            }
            onPressed: function(mouse) { pad.forceActiveFocus(); move(mouse) }
            onPositionChanged: function(mouse) { if (pressed) move(mouse) }
            onReleased: if (pad.controller) pad.controller.finish_mood()
        }
    }
    Row {
        id: ticks
        y: field.height + pad.style.spaceXs
        width: field.width
        Repeater {
            model: pad.controller ? pad.controller.modeNames : []
            delegate: Text {
                required property var modelData
                required property int index
                width: ticks.width / Math.max(1, pad.controller.modeNames.length)
                horizontalAlignment: Text.AlignHCenter
                text: String(modelData).slice(0, pad.modeAbbreviation)
                color: pad.controller && index === pad.controller.modeIndex ? pad.style.violet : pad.style.faint
                font.family: pad.style.mono
                font.pixelSize: pad.style.typeCaption
                font.weight: Font.DemiBold
            }
        }
    }
}
