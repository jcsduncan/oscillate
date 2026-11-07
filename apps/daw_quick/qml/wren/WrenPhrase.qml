pragma ComponentBehavior: Bound
import QtQuick

// Wren's phrase lane: the first two bars of the track's first MIDI clip as
// Breath is about to play them (the controller's wren_phrase), drawn as a
// piano roll on beat lines. Each note is coloured by how it starts
// (tongued, or staccato), stands as tall and as opaque as it is loud, and
// carries the velocity Breath plays; slurred notes hang from an arc, grace
// notes are white ticks on the beat, and a wave starts where the vibrato
// blooms, growing to its depth. Without notes it says how to get some.
Item {
    id: lane
    required property WrenTheme theme
    // wren_phrase's map: {bars, beats_per_bar, max_velocity, low_key,
    // high_key, notes}.
    property var phrase: ({})
    // Bars of an unknown meter are drawn in 4/4.
    readonly property real defaultBeatsPerBar: 4
    readonly property int defaultBars: 2
    readonly property int bars: phrase.bars !== undefined ? phrase.bars : defaultBars
    readonly property real beatsPerBar: phrase.beats_per_bar !== undefined ? phrase.beats_per_bar
                                                                          : defaultBeatsPerBar
    readonly property int beatsShown: Math.round(bars * beatsPerBar)
    readonly property var notes: phrase.notes !== undefined ? phrase.notes : []
    readonly property real maxVelocity: phrase.max_velocity !== undefined ? phrase.max_velocity : 1

    // Geometry. The key range shows the notes with room above and below,
    // at least an octave.
    readonly property int keyPad: 2
    readonly property int minKeys: 12
    readonly property int lowKey: (phrase.low_key !== undefined ? phrase.low_key : 0) - keyPad
    readonly property int highKey: Math.max((phrase.high_key !== undefined ? phrase.high_key : 0) + keyPad,
                                            lowKey + minKeys - 1)
    readonly property int keys: highKey - lowKey + 1
    readonly property real legendHeight: 16
    readonly property real legendGap: 8
    readonly property real footHeight: 16
    readonly property real inset: 14
    // Room over the top note for its slur arc and wave, and under the
    // lowest for its velocity.
    readonly property real topRoom: 34
    readonly property real labelRoom: 18
    readonly property real minBar: 6
    readonly property real maxBar: 18
    readonly property real minOpacity: 0.45
    readonly property real noteGap: 3
    readonly property real minNoteWidth: 4
    readonly property real noteRadius: 4
    readonly property real labelGap: 2
    readonly property real graceWidth: 6
    readonly property real graceHeight: 12
    readonly property real graceRadius: 2
    readonly property real waveHeight: 10
    readonly property real waveGap: 4
    readonly property real wavePeriod: 9
    readonly property real waveStroke: 1.4
    readonly property real arcRise: 14
    // An arc clears the wave over the higher of its notes.
    readonly property real arcGap: waveGap + waveHeight + arcRise + labelGap
    readonly property real minArcWidth: 4
    readonly property real arcStroke: 1.5
    readonly property real legendSwatch: 10
    readonly property real legendWave: 18
    readonly property real legendSpacing: 16
    readonly property real legendInner: 6

    readonly property real usable: Math.max(1, ground.width - 2 * inset)
    readonly property real keyHeight: Math.max(1, (ground.height - topRoom - labelRoom) / Math.max(1, keys))
    function xOf(fraction) {
        return inset + fraction * usable
    }
    // The vertical centre of a key's row.
    function yOfKey(key) {
        return topRoom + (highKey - key + 0.5) * keyHeight
    }
    function barHeightOf(velocity) {
        return minBar + (maxBar - minBar) * velocity / maxVelocity
    }
    // Where note `index`'s body starts and ends, after its grace.
    function bodyX(note) {
        return xOf(note.position + note.grace_length)
    }
    function bodyEnd(note) {
        return Math.max(bodyX(note) + minNoteWidth, xOf(note.position + note.length) - noteGap)
    }
    function bodyTop(note) {
        return yOfKey(note.key) - barHeightOf(note.velocity) / 2
    }
    function bodyMiddle(note) {
        return (bodyX(note) + bodyEnd(note)) / 2
    }
    // The slur arcs: from the middle of the note before to the middle of a
    // slurred note, so a run of slurs chains end to end, and from a grace to
    // the middle of its note.
    readonly property var arcs: {
        const out = []
        for (let i = 0; i < notes.length; ++i) {
            const note = notes[i]
            if (note.grace) {
                const graceTop = yOfKey(note.grace_key) - graceHeight / 2
                out.push({ index: i, from: xOf(note.position), to: bodyMiddle(note),
                           top: Math.min(graceTop, bodyTop(note)) - arcGap })
            } else if (note.tongue === "slur" && i > 0) {
                const before = notes[i - 1]
                out.push({ index: i, from: bodyMiddle(before), to: bodyMiddle(note),
                           top: Math.min(bodyTop(before), bodyTop(note)) - arcGap })
            }
        }
        return out
    }

    objectName: "wrenPhrase"
    Accessible.role: Accessible.Graphic
    Accessible.name: "Phrase lane"
    Accessible.description: notes.length > 0 ? notes.length + " notes in two bars" : hint.text

    // An arc over two points: a slur mark.
    component Arc: Canvas {
        id: arc
        property color stroke: lane.theme.slur
        property real lineWidth: lane.arcStroke
        onStrokeChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const g = getContext("2d")
            g.reset()
            g.strokeStyle = stroke
            g.lineWidth = lineWidth
            g.beginPath()
            g.moveTo(lineWidth, height)
            g.bezierCurveTo(lineWidth, lineWidth, width - lineWidth, lineWidth, width - lineWidth, height)
            g.stroke()
        }
    }
    // A vibrato wave whose swing grows from 0 to `depth` over `rise` of its
    // width.
    component Wave: Canvas {
        id: wave
        property real depth: 1
        property real rise: 0
        onDepthChanged: requestPaint()
        onRiseChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const g = getContext("2d")
            g.reset()
            g.strokeStyle = lane.theme.wave
            g.lineWidth = lane.waveStroke
            g.beginPath()
            const middle = height / 2
            const swing = (height - lane.waveStroke) / 2
            for (let x = 0; x <= width; ++x) {
                const grown = rise > 0 ? Math.min(1, x / rise) : 1
                const y = middle - swing * depth * grown * Math.sin(2 * Math.PI * x / lane.wavePeriod)
                if (x === 0)
                    g.moveTo(x, y)
                else
                    g.lineTo(x, y)
            }
            g.stroke()
        }
    }

    // A legend entry: its swatch, then its name.
    component Key: Row {
        id: key
        property string label: ""
        default property alias swatch: holder.data
        spacing: lane.legendInner
        Item {
            id: holder
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: lane.legendSwatch
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: key.label
            color: lane.theme.inkDim
            font.family: lane.theme.sans
            font.pixelSize: lane.theme.captionSize
        }
    }

    // ---- Legend ---------------------------------------------------------------------

    Text {
        height: lane.legendHeight
        verticalAlignment: Text.AlignVCenter
        text: "TWO BARS · AS IT PLAYS"
        color: lane.theme.inkDim
        font.family: lane.theme.sans
        font.pixelSize: lane.theme.captionSize
        font.letterSpacing: lane.theme.labelSpacing
    }
    Row {
        id: legend
        anchors.right: parent.right
        height: lane.legendHeight
        spacing: lane.legendSpacing
        Key {
            label: "Tongued"
            Rectangle {
                width: lane.legendSwatch
                height: lane.legendSwatch
                radius: lane.graceRadius
                color: lane.theme.tongued
            }
        }
        Key {
            label: "Slur"
            Arc { width: lane.legendWave - lane.legendInner; height: lane.legendSwatch / 2 + lane.arcStroke }
        }
        Key {
            label: "Staccato"
            Rectangle {
                width: lane.legendSwatch
                height: lane.legendSwatch
                radius: lane.graceRadius
                color: lane.theme.staccato
            }
        }
        Key {
            label: "Grace"
            Rectangle {
                width: lane.graceWidth
                height: lane.legendSwatch
                radius: lane.graceRadius
                color: lane.theme.grace
            }
        }
        Key {
            label: "Vibrato"
            Wave { width: lane.legendWave; height: lane.legendSwatch }
        }
    }

    // ---- The lane -------------------------------------------------------------------

    Rectangle {
        id: ground
        objectName: "wrenPhraseGround"
        y: lane.legendHeight + lane.legendGap
        width: lane.width
        height: lane.height - y - lane.legendGap - lane.footHeight
        radius: lane.theme.tileRadius - lane.theme.idleBorder
        color: lane.theme.laneGround
        clip: true

        Repeater {
            model: lane.beatsShown + 1
            delegate: Rectangle {
                required property int index
                readonly property bool barLine: index % Math.max(1, Math.round(lane.beatsPerBar)) === 0
                x: lane.xOf(index / Math.max(1, lane.beatsShown))
                width: lane.theme.idleBorder
                height: ground.height
                color: barLine ? lane.theme.barLine : lane.theme.beatLine
            }
        }

        Repeater {
            model: lane.arcs
            delegate: Arc {
                required property var modelData
                objectName: "wrenPhraseSlur:" + modelData.index
                x: modelData.from
                y: modelData.top
                width: Math.max(lane.minArcWidth, modelData.to - modelData.from)
                height: lane.arcRise
            }
        }

        Repeater {
            model: lane.notes
            delegate: Item {
                id: note
                required property var modelData
                required property int index
                readonly property real barHeight: lane.barHeightOf(modelData.velocity)
                readonly property color noteColor: lane.theme.noteColor(modelData.staccato)
                readonly property int velocity: modelData.velocity
                readonly property string tongue: modelData.tongue
                readonly property bool staccato: modelData.staccato
                readonly property real start: lane.bodyX(modelData)
                readonly property real end: lane.bodyEnd(modelData)
                readonly property real ceiling: lane.bodyTop(modelData)
                readonly property real waveFrom: Math.max(start, lane.xOf(modelData.vibrato_from))
                objectName: "wrenPhraseNote:" + index
                anchors.fill: parent
                Accessible.role: Accessible.StaticText
                Accessible.name: modelData.name + ", velocity " + modelData.velocity
                    + (modelData.staccato ? ", staccato" : "")
                    + (modelData.tongue === "slur" ? ", slurred" : modelData.tongue === "k" ? ", k tongued" : "")
                    + (modelData.grace ? ", with a grace" : "") + (modelData.vibrato > 0 ? ", vibrato" : "")

                Rectangle {
                    objectName: "wrenPhraseBody:" + note.index
                    x: note.start
                    y: note.ceiling
                    width: note.end - note.start
                    height: note.barHeight
                    radius: Math.min(lane.noteRadius, height / 2)
                    color: note.noteColor
                    opacity: lane.minOpacity + (1 - lane.minOpacity) * note.velocity / lane.maxVelocity
                }
                Text {
                    x: note.start
                    y: note.ceiling + note.barHeight + lane.labelGap
                    text: note.velocity + (note.tongue === "k" ? " k" : "")
                    color: lane.theme.inkDim
                    font.family: lane.theme.mono
                    font.pixelSize: lane.theme.smallSize
                }
                Wave {
                    objectName: "wrenPhraseVibrato:" + note.index
                    visible: note.modelData.vibrato > 0 && note.end - note.waveFrom > lane.wavePeriod
                    x: note.waveFrom
                    y: note.ceiling - lane.waveGap - lane.waveHeight
                    width: Math.max(0, note.end - note.waveFrom)
                    height: lane.waveHeight
                    depth: Math.min(1, note.modelData.vibrato)
                    rise: Math.max(0, lane.xOf(note.modelData.vibrato_full) - note.waveFrom)
                }
                Rectangle {
                    objectName: "wrenPhraseGrace:" + note.index
                    visible: note.modelData.grace
                    x: lane.xOf(note.modelData.position)
                    y: lane.yOfKey(note.modelData.grace_key) - lane.graceHeight / 2
                    width: Math.max(lane.graceWidth, note.start - x - lane.theme.idleBorder)
                    height: lane.graceHeight
                    radius: lane.graceRadius
                    color: lane.theme.grace
                }
            }
        }

        Text {
            id: hint
            objectName: "wrenPhraseHint"
            anchors.centerIn: parent
            visible: lane.notes.length === 0
            text: "Draw notes in a clip to see Breath play them"
            color: lane.theme.inkDim
            font.family: lane.theme.sans
            font.pixelSize: lane.theme.bodySize
        }
    }

    Text {
        y: ground.y + ground.height + lane.legendGap
        width: lane.width
        height: lane.footHeight
        verticalAlignment: Text.AlignVCenter
        text: "Numbers are the velocities Breath plays. Height is loudness; the wave starts where the vibrato blooms."
        color: lane.theme.inkDim
        font.family: lane.theme.sans
        font.pixelSize: lane.theme.captionSize
        elide: Text.ElideRight
    }
}
