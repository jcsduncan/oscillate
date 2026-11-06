pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// The playable key strip along the bottom of the panel (ADR-0076): ten
// white keys labelled with the typing keyboard's letters (A S D F G H J K L
// ;) and the black keys between them (W E T Y U O P), from `base` (the
// typing keyboard's octave). Press a key to play it, held until released;
// drag across to play the keys passed; a press nearer a key's foot plays
// louder. The panel cannot play notes itself: it reports keyPressed and
// keyReleased and the host plays them on the synth's track. A key lights
// while held, and while a voice of the sounding synth plays it.
Rectangle {
    id: strip
    required property var panel
    property int base: Contract.defaultKeyBase
    // The note the mouse holds, or -1.
    property int held: -1
    // The notes the synth's voices are playing (telemetry), as a set.
    readonly property var sounding: {
        const notes = ({})
        if (!panel.live)
            return notes
        for (let v = 0; v < Contract.telemetryNoteVoices; ++v) {
            const key = Math.round(panel.at(Contract.T.voiceKey + v))
            if (key >= 0)
                notes[key] = true
        }
        return notes
    }
    readonly property real padX: panel.theme.keyStripPadX
    readonly property real padY: panel.theme.keyStripPadY
    readonly property real gap: panel.theme.keyGap
    // A black key's share of a white key's width and of the keys' height.
    readonly property real blackWidthShare: 0.56
    readonly property real blackHeightShare: 0.58
    readonly property real whiteWidth: (width - 2 * padX - (Contract.whiteKeys.length - 1) * gap) / Contract.whiteKeys.length
    readonly property real blackWidth: whiteWidth * blackWidthShare
    readonly property real blackHeight: (height - 2 * padY) * blackHeightShare
    readonly property real labelSize: panel.theme.typeCaption
    signal keyPressed(int note, int velocity)
    signal keyReleased(int note)

    objectName: "novaKeyStrip"
    color: panel.theme.rail
    Accessible.role: Accessible.Grouping
    Accessible.name: "Keys: click to play"

    function lit(note) {
        return note === held || sounding[note] === true
    }
    function whiteX(index) {
        return padX + index * (whiteWidth + gap)
    }
    function blackX(key) {
        return whiteX(key.after) + whiteWidth + gap / 2 - blackWidth / 2
    }
    // The note under a point of the strip, black keys first, or -1.
    function noteAt(x, y) {
        if (y >= padY && y < padY + blackHeight) {
            for (let i = 0; i < Contract.blackKeys.length; ++i) {
                const k = Contract.blackKeys[i]
                const left = blackX(k)
                if (x >= left && x < left + blackWidth)
                    return base + k.offset
            }
        }
        if (y < padY || y >= height - padY)
            return -1
        const index = Math.floor((x - padX) / (whiteWidth + gap))
        return index >= 0 && index < Contract.whiteKeys.length ? base + Contract.whiteKeys[index].offset : -1
    }
    function velocityAt(y) {
        const depth = Math.max(0, Math.min(1, (y - padY) / Math.max(1, height - 2 * padY)))
        return Math.round(Contract.keyVelocityMinimum
                          + depth * (Contract.keyVelocityMaximum - Contract.keyVelocityMinimum))
    }
    function press(note, y) {
        if (note === held)
            return
        release()
        if (note < 0)
            return
        held = note
        keyPressed(note, velocityAt(y))
    }
    function release() {
        if (held < 0)
            return
        const note = held
        held = -1
        keyReleased(note)
    }

    Rectangle {
        width: parent.width
        height: strip.panel.theme.hairline
        color: strip.panel.theme.line
    }
    Repeater {
        model: Contract.whiteKeys
        delegate: Rectangle {
            id: white
            required property var modelData
            required property int index
            readonly property int note: strip.base + modelData.offset
            readonly property bool on: strip.lit(note)
            objectName: "novaKey:" + modelData.label
            x: strip.whiteX(index)
            y: strip.padY
            width: strip.whiteWidth
            height: strip.height - 2 * strip.padY
            radius: strip.panel.theme.radiusMd
            gradient: Gradient {
                GradientStop { position: 0; color: white.on ? strip.panel.theme.keyLitTop : strip.panel.theme.keyWhiteTop }
                GradientStop { position: 1; color: white.on ? strip.panel.theme.keyLitBottom : strip.panel.theme.keyWhiteBottom }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: strip.panel.theme.whiteKeyLabelLift
                text: white.modelData.label
                color: white.on ? strip.panel.theme.bg : strip.panel.theme.keyLabel
                font.family: strip.panel.theme.mono
                font.pixelSize: strip.labelSize
                font.weight: Font.DemiBold
            }
        }
    }
    Repeater {
        model: Contract.blackKeys
        delegate: Rectangle {
            id: black
            required property var modelData
            readonly property int note: strip.base + modelData.offset
            readonly property bool on: strip.lit(note)
            objectName: "novaKey:" + modelData.label
            x: strip.blackX(modelData)
            y: strip.padY
            width: strip.blackWidth
            height: strip.blackHeight
            radius: strip.panel.theme.radiusSm
            color: on ? strip.panel.theme.keyLitBottom : strip.panel.theme.keyBlack
            border.color: strip.panel.theme.line2
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: strip.panel.theme.blackKeyLabelLift
                text: black.modelData.label
                color: black.on ? strip.panel.theme.bg : strip.panel.theme.faint
                font.family: strip.panel.theme.mono
                font.pixelSize: strip.labelSize
                font.weight: Font.DemiBold
            }
        }
    }
    MouseArea {
        anchors.fill: parent
        preventStealing: true
        onPressed: mouse => strip.press(strip.noteAt(mouse.x, mouse.y), mouse.y)
        onPositionChanged: mouse => {
            if (pressed)
                strip.press(strip.noteAt(mouse.x, mouse.y), mouse.y)
        }
        onReleased: strip.release()
        onCanceled: strip.release()
    }
    Component.onDestruction: release()
}
