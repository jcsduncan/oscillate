pragma ComponentBehavior: Bound
import QtQuick

// The history strip: the last 200 readings (about four seconds) as amber
// dots on a staff of semitones centred on the notes played, each dot
// coloured by how far it was from its nearest note; the newest at the
// right. Lines are named in the panel's notation.
Item {
    id: strip
    required property TuneTheme theme
    // Fractional MIDI notes, oldest first; 0 is no reading.
    property var notes: []
    // The twelve note names in the panel's notation, from C.
    property var names: []
    function nameOf(k) {
        return names.length === 12 ? names[((k % 12) + 12) % 12] + (Math.floor(k / 12) - 1) : ""
    }
    readonly property int span: 13
    // The staff's middle with no readings: A4.
    readonly property int restingCentre: 69
    // The staff's middle note: the median of the readings, so a held note
    // sits at the centre and a run stays on the strip.
    readonly property int centre: {
        const sounding = []
        for (let i = 0; i < notes.length; ++i) {
            if (notes[i] > 0)
                sounding.push(notes[i])
        }
        if (sounding.length === 0)
            return strip.restingCentre
        sounding.sort((a, b) => a - b)
        return Math.round(sounding[Math.floor(sounding.length / 2)])
    }
    // The notes named on the staff: those played, newest first, and the
    // centre; a note a semitone from one already named goes unnamed so
    // labels never touch.
    readonly property var named: {
        const out = {}
        const name = n => {
            if (out[n - 1] !== true && out[n + 1] !== true)
                out[n] = true
        }
        for (let i = notes.length - 1; i >= 0; --i) {
            if (notes[i] > 0)
                name(Math.round(notes[i]))
        }
        name(centre)
        return out
    }
    readonly property real low: centre - (span - 1) / 2 - 0.5
    readonly property real labelWidth: 46
    readonly property real plotW: width - labelWidth - theme.spaceMd
    function yOf(note) {
        return height - (note - low) / span * height
    }

    objectName: "tuneHistory"

    Rectangle {
        anchors.fill: parent
        radius: strip.theme.viewWellRadius
        color: strip.theme.well
        border.color: strip.theme.wellEdge
    }
    // The staff: a line per semitone, naturals stronger, named at the left.
    Repeater {
        model: strip.span
        delegate: Item {
            id: staff
            required property int index
            readonly property int note: strip.centre - (strip.span - 1) / 2 + index
            readonly property bool natural: [0, 2, 4, 5, 7, 9, 11].indexOf(((note % 12) + 12) % 12) >= 0
            width: strip.width
            y: strip.yOf(note)
            Rectangle {
                x: strip.labelWidth
                width: strip.plotW
                height: strip.theme.hairline
                color: staff.note === strip.centre ? strip.theme.gridStrong
                     : staff.natural ? strip.theme.grid : Qt.rgba(strip.theme.grid.r, strip.theme.grid.g, strip.theme.grid.b, strip.theme.sharpLineAlpha)
            }
            Text {
                x: strip.theme.spaceMd
                anchors.verticalCenter: parent.top
                width: strip.labelWidth - strip.theme.spaceMd - strip.theme.spaceSm
                horizontalAlignment: Text.AlignRight
                visible: strip.named[staff.note] === true
                text: strip.nameOf(staff.note)
                color: staff.note === strip.centre ? strip.theme.inkDim : strip.theme.inkFaint
                font.family: strip.theme.mono
                font.pixelSize: strip.theme.typeCaption
            }
        }
    }
    Repeater {
        objectName: "tuneHistoryDots"
        model: strip.notes.length
        delegate: Rectangle {
            id: dot
            required property int index
            readonly property real note: strip.notes[index]
            readonly property real cents: 100 * (note - Math.round(note))
            readonly property real age: 1 - index / Math.max(1, strip.notes.length - 1)
            visible: note > 0 && note >= strip.low && note <= strip.low + strip.span
            x: strip.labelWidth + strip.plotW * index / Math.max(1, strip.notes.length - 1) - width / 2
            y: strip.yOf(note) - height / 2
            width: strip.theme.historyDotSize
            height: strip.theme.historyDotSize
            radius: strip.theme.historyDotSize / 2
            opacity: strip.theme.historyFloorOpacity + (1 - strip.theme.historyFloorOpacity) * (1 - age)
            color: Math.abs(cents) < strip.theme.inTuneCents ? strip.theme.inTune
                 : Math.abs(cents) < strip.theme.nearCents ? strip.theme.amber : strip.theme.off
        }
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: strip.theme.nowInset
        anchors.bottom: parent.bottom
        anchors.bottomMargin: strip.theme.spaceXs
        text: "NOW"
        color: strip.theme.inkFaint
        font.pixelSize: strip.theme.typeMicro
        font.bold: true
        font.letterSpacing: strip.theme.captionSpacing
    }
}
