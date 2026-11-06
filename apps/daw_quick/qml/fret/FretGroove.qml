pragma ComponentBehavior: Bound
import QtQuick

// Fret's groove lane: the first two bars of the track's first MIDI clip as
// Feel is about to play them (the controller's fret_groove), on a sixteenth
// grid. Each note stands as tall as its velocity, coloured by articulation,
// ringed when pushed and shifted right by its Pocket delay. Without notes
// it says how to get some.
Item {
    id: lane
    required property FretTheme theme
    // fret_groove's map: {bars, beats_per_bar, max_velocity, notes}.
    property var groove: ({})
    // A sixteenth grid.
    readonly property int stepsPerBeat: 4
    // Bars of an unknown meter are drawn in 4/4.
    readonly property real defaultBeatsPerBar: 4
    readonly property int defaultBars: 2
    readonly property int bars: groove.bars !== undefined ? groove.bars : defaultBars
    readonly property real beatsPerBar: groove.beats_per_bar !== undefined ? groove.beats_per_bar
                                                                          : defaultBeatsPerBar
    readonly property int beatsShown: Math.round(bars * beatsPerBar)
    readonly property int steps: beatsShown * stepsPerBeat
    readonly property var notes: groove.notes !== undefined ? groove.notes : []
    readonly property real maxVelocity: groove.max_velocity !== undefined ? groove.max_velocity : 1
    readonly property real gap: 3
    readonly property real cell: (columns.width - gap * (steps - 1)) / Math.max(1, steps)
    // Pocket's shift on screen, per millisecond late.
    readonly property real pocketPixelsPerMs: 0.6
    readonly property real labelHeight: 16
    readonly property real velocityHeight: 18
    // The legend over the lane: its height, the room between entries and
    // between a swatch and its name, and the gap down to the lane.
    readonly property real legendHeight: 16
    readonly property real legendSpacing: 16
    readonly property real legendInner: 6
    readonly property real legendGap: 8
    // A note's velocity, this far down its column.
    readonly property real velocityTop: 4

    objectName: "fretGroove"
    Accessible.role: Accessible.Graphic
    Accessible.name: "Groove lane"
    Accessible.description: notes.length > 0 ? notes.length + " notes in two bars" : hint.text

    Row {
        id: legend
        anchors.right: parent.right
        height: lane.legendHeight
        spacing: lane.legendSpacing
        Repeater {
            model: [{ name: "Pluck", art: "pluck" }, { name: "Staccato", art: "staccato" },
                    { name: "Ghost", art: "ghost" }, { name: "Bridge", art: "bridge" }]
            delegate: Row {
                required property var modelData
                spacing: lane.legendInner
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: lane.theme.swatch
                    height: lane.theme.swatch
                    radius: lane.theme.radiusXs
                    color: lane.theme.articulationColor(parent.modelData.art)
                }
                Text {
                    text: parent.modelData.name
                    color: lane.theme.inkDim
                    font.family: lane.theme.sans
                    font.pixelSize: lane.theme.typeLabel
                }
            }
        }
        Row {
            spacing: lane.legendInner
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: lane.theme.swatch
                height: lane.theme.swatch
                radius: lane.theme.radiusXs
                color: "transparent"
                border.width: lane.theme.strokeBold
                border.color: lane.theme.pushedRing
            }
            Text {
                text: "Pushed"
                color: lane.theme.inkDim
                font.family: lane.theme.sans
                font.pixelSize: lane.theme.typeLabel
            }
        }
    }
    Text {
        height: lane.legendHeight
        text: "TWO BARS · AS IT PLAYS"
        color: lane.theme.inkDim
        font.family: lane.theme.sans
        font.pixelSize: lane.theme.typeLabel
        font.letterSpacing: lane.theme.labelSpacing
    }

    Item {
        id: columns
        y: legend.height + lane.legendGap
        width: lane.width
        height: lane.height - y - 2 * lane.labelHeight
        clip: true

        Repeater {
            model: lane.steps
            delegate: Rectangle {
                required property int index
                x: index * (lane.cell + lane.gap)
                width: lane.cell
                height: columns.height
                radius: lane.theme.radiusMd
                color: index % lane.stepsPerBeat === 0 ? lane.theme.groundBeat : lane.theme.ground
            }
        }
        Repeater {
            model: lane.notes
            delegate: Item {
                id: note
                required property var modelData
                required property int index
                readonly property real barHeight: (columns.height - lane.velocityHeight)
                    * modelData.velocity / lane.maxVelocity
                readonly property color noteColor: lane.theme.articulationColor(modelData.articulation)
                readonly property string articulation: modelData.articulation
                readonly property int velocity: modelData.velocity
                readonly property bool pushed: modelData.pushed
                objectName: "fretGrooveNote:" + index
                x: modelData.position * (columns.width + lane.gap) + modelData.delay_ms * lane.pocketPixelsPerMs
                width: lane.cell
                height: columns.height
                Accessible.role: Accessible.StaticText
                Accessible.name: modelData.name + ", velocity " + modelData.velocity + ", " + modelData.articulation
                    + (modelData.pushed ? ", pushed" : "")
                Text {
                    width: parent.width
                    y: lane.velocityTop
                    horizontalAlignment: Text.AlignHCenter
                    text: note.velocity
                    color: lane.theme.noteInk
                    font.family: lane.theme.mono
                    font.pixelSize: lane.theme.typeSmall
                }
                Rectangle {
                    objectName: "fretGrooveBar:" + note.index
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: note.barHeight
                    radius: lane.theme.radiusSm
                    color: note.noteColor
                    border.width: note.pushed ? lane.theme.strokeBold : 0
                    border.color: lane.theme.pushedRing
                }
            }
        }
        Text {
            id: hint
            objectName: "fretGrooveHint"
            anchors.centerIn: parent
            visible: lane.notes.length === 0
            text: "Draw notes in a clip to see Feel play them"
            color: lane.theme.inkDim
            font.family: lane.theme.sans
            font.pixelSize: lane.theme.typeEmphasis
        }
    }

    // Beat numbers under the beats, then each note's name.
    Repeater {
        model: lane.steps
        delegate: Text {
            required property int index
            readonly property bool onBeat: index % lane.stepsPerBeat === 0
            x: index * (lane.cell + lane.gap)
            y: columns.y + columns.height
            width: lane.cell
            height: lane.labelHeight
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            text: onBeat ? String((index / lane.stepsPerBeat) % Math.max(1, Math.round(lane.beatsPerBar)) + 1) : "·"
            color: index % Math.max(1, Math.round(lane.beatsPerBar * lane.stepsPerBeat)) === 0
                ? lane.theme.ink : lane.theme.inkFaint
            font.family: lane.theme.mono
            font.pixelSize: lane.theme.typeBody
        }
    }
    Repeater {
        model: lane.notes
        delegate: Text {
            required property var modelData
            x: modelData.position * (columns.width + lane.gap) - lane.gap
            y: columns.y + columns.height + lane.labelHeight
            width: lane.cell + 2 * lane.gap
            height: lane.labelHeight
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            text: modelData.pitch
            color: lane.theme.noteInk
            font.family: lane.theme.mono
            font.pixelSize: lane.theme.typeBody

        }
    }
}
