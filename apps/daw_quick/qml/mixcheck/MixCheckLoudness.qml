pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// Loudness and peaks against the chosen target (club master, streaming,
// into a DJ mix): integrated loudness, true peak, loudness range and the
// drums' crest, each on a rail with the target zone shaded and the song's
// value marked; green inside, amber within 1 of it, red further out.
Column {
    id: loudnessView
    required property MixCheckTheme style
    // [{ id, name, note, lufsLow, lufsHigh, truePeak, lraLow, lraHigh, crestLow, crestHigh }]
    property var targets: []
    property string target: ""
    // { integrated, truePeak, range, crest }
    property var loudness: ({})
    signal targetPicked(string id)

    // Each rail's drawn range, and how far outside the zone still reads amber.
    readonly property var integratedRail: [-20, -4]
    readonly property var truePeakRail: [-4, 2]
    readonly property var rangeRail: [0, 14]
    readonly property var crestRail: [4, 18]
    readonly property real nearMargin: 1
    // A true peak this far under the ceiling or lower counts as inside.
    readonly property real truePeakFloor: -12

    readonly property var chosen: {
        const all = style.list(targets)
        for (let i = 0; i < all.length; ++i) {
            if (all[i] && all[i].id === target)
                return all[i]
        }
        return all.length > 0 && all[0] ? all[0] : ({})
    }
    readonly property var song: style.map(loudness)
    readonly property real truePeakOver: {
        const peak = style.number(song.truePeak)
        const ceiling = style.number(chosen.truePeak)
        return isFinite(peak) && isFinite(ceiling) && peak > ceiling ? peak - ceiling : 0
    }

    spacing: style.cardSpacing

    Item {
        width: parent.width
        height: targetSegment.height
        MixCheckHead {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - targetSegment.width - targetHint.width - 2 * loudnessView.style.controlLabelGap
            style: loudnessView.style
            title: "Loudness & peaks"
        }
        Text {
            id: targetHint
            anchors.right: targetSegment.left
            anchors.rightMargin: loudnessView.style.controlLabelGap
            anchors.verticalCenter: parent.verticalCenter
            text: "Target"
            color: loudnessView.style.textFaint
            font.family: loudnessView.style.sansFamily
            font.pixelSize: loudnessView.style.typeBody
        }
        Kit.SegmentedControl {
            id: targetSegment
            objectName: "mixCheckTarget"
            anchors.right: parent.right
            theme: loudnessView.style
            namePrefix: "mixCheckTarget"
            accessibleName: "Loudness target"
            options: {
                const style = loudnessView.style
                const all = style.list(loudnessView.targets)
                const result = []
                for (let i = 0; i < all.length; ++i) {
                    const entry = style.map(all[i])
                    result.push({ id: style.text(entry.id), label: style.text(entry.name),
                                  tip: style.text(entry.note) })
                }
                return result
            }
            current: loudnessView.style.text(loudnessView.chosen.id)
            onPicked: function(id) { loudnessView.targetPicked(id) }
        }
    }

    Row {
        id: gaugeRow
        spacing: loudnessView.style.spaceMd
        MixCheckGauge {
            style: loudnessView.style
            width: (loudnessView.width - 3 * gaugeRow.spacing) / 4
            nearMargin: loudnessView.nearMargin
            gaugeId: "integrated"
            title: "Integrated"
            value: loudnessView.style.number(loudnessView.song.integrated)
            unit: "LUFS"
            low: loudnessView.style.number(loudnessView.chosen.lufsLow)
            high: loudnessView.style.number(loudnessView.chosen.lufsHigh)
            rail: loudnessView.integratedRail
            caption: isFinite(low) && isFinite(high)
                ? "target " + loudnessView.style.signedDb(low, 0) + " to " + loudnessView.style.signedDb(high, 0)
                : ""
        }
        MixCheckGauge {
            style: loudnessView.style
            width: (loudnessView.width - 3 * gaugeRow.spacing) / 4
            nearMargin: loudnessView.nearMargin
            gaugeId: "truePeak"
            title: "True peak"
            value: loudnessView.style.number(loudnessView.song.truePeak)
            unit: "dBTP"
            signedValue: true
            low: loudnessView.truePeakFloor
            high: loudnessView.style.number(loudnessView.chosen.truePeak)
            rail: loudnessView.truePeakRail
            caption: isFinite(high) ? "ceiling " + loudnessView.style.signedDb(high, 1) + " dBTP" : ""
        }
        MixCheckGauge {
            style: loudnessView.style
            width: (loudnessView.width - 3 * gaugeRow.spacing) / 4
            nearMargin: loudnessView.nearMargin
            gaugeId: "range"
            title: "Loudness range"
            value: loudnessView.style.number(loudnessView.song.range)
            unit: "LU"
            low: loudnessView.style.number(loudnessView.chosen.lraLow)
            high: loudnessView.style.number(loudnessView.chosen.lraHigh)
            rail: loudnessView.rangeRail
            caption: "between quiet and loud parts"
        }
        MixCheckGauge {
            style: loudnessView.style
            width: (loudnessView.width - 3 * gaugeRow.spacing) / 4
            nearMargin: loudnessView.nearMargin
            gaugeId: "crest"
            title: "Crest, drums"
            value: loudnessView.style.number(loudnessView.song.crest)
            unit: "dB"
            low: loudnessView.style.number(loudnessView.chosen.crestLow)
            high: loudnessView.style.number(loudnessView.chosen.crestHigh)
            rail: loudnessView.crestRail
            caption: "peak to RMS: punch left"
        }
    }

    Text {
        objectName: "mixCheckLoudnessNote"
        width: parent.width
        wrapMode: Text.WordWrap
        textFormat: Text.StyledText
        visible: text.length > 0
        text: {
            const style = loudnessView.style
            const name = style.text(loudnessView.chosen.name)
            const note = style.text(loudnessView.chosen.note)
            let result = name.length > 0 ? "<b><font color=\"" + style.textMain + "\">" + name + ":</font></b> " + note
                                         : note
            if (loudnessView.truePeakOver > 0)
                result += " <font color=\"" + style.red + "\">True peak is "
                          + loudnessView.truePeakOver.toFixed(1)
                          + " dB over: lower the master limiter's ceiling.</font>"
            return result
        }
        color: loudnessView.style.textFaint
        font.family: loudnessView.style.sansFamily
        font.pixelSize: loudnessView.style.typeLabel
        lineHeight: loudnessView.style.proseLineHeight
    }
}
