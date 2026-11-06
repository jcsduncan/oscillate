import QtQuick

// One channel's meter: the RMS as a bar in console green, amber over
// -12 dBFS and red over -3, the held peak as a bright line, on a dB scale
// from `floorDb` to `ceilDb`. Levels at or under the floor show nothing.
Item {
    id: meter
    required property TrimTheme theme
    property real peakDb: -200
    property real rmsDb: -200
    property bool live: false
    property real floorDb: -60
    property real ceilDb: theme.meterCeilDb
    // Where the fill and the hold stand, 0 (floor) to 1 (ceiling).
    readonly property real rmsAt: live ? fraction(rmsDb) : 0
    readonly property real peakAt: live ? fraction(peakDb) : 0
    // The gap between one colour zone and the next.
    readonly property real zoneEdge: 0.001
    readonly property real inset: theme.meterInset

    implicitWidth: theme.meterDefaultWidth
    implicitHeight: theme.meterDefaultLength

    function fraction(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / (ceilDb - floorDb)))
    }

    Rectangle {
        id: well
        anchors.fill: parent
        radius: meter.theme.radiusSm
        color: meter.theme.meterOff
        border.color: meter.theme.wellEdge
        clip: true
        // The colour zones, revealed from the bottom up to the RMS.
        Item {
            objectName: "trimMeterFill"
            x: meter.inset
            width: parent.width - 2 * meter.inset
            anchors.bottom: parent.bottom
            anchors.bottomMargin: meter.inset
            height: (parent.height - 2 * meter.inset) * meter.rmsAt
            clip: true
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: well.height - 2 * meter.inset
                radius: meter.theme.radiusXs
                gradient: Gradient {
                    GradientStop { position: 0; color: meter.theme.meterHigh }
                    GradientStop { position: meter.fromTop(meter.theme.meterOverDb); color: meter.theme.meterHigh }
                    GradientStop { position: meter.fromTop(meter.theme.meterOverDb) + meter.zoneEdge; color: meter.theme.meterMid }
                    GradientStop { position: meter.fromTop(meter.theme.meterHotDb); color: meter.theme.meterMid }
                    GradientStop { position: meter.fromTop(meter.theme.meterHotDb) + meter.zoneEdge; color: meter.theme.meterLow }
                    GradientStop { position: 1; color: meter.theme.meterFoot }
                }
            }
        }
        // The held peak.
        Rectangle {
            objectName: "trimMeterPeak"
            visible: meter.peakAt > 0
            x: meter.theme.peakLineInset
            width: parent.width - 2 * meter.theme.peakLineInset
            height: meter.theme.peakLineHeight
            y: meter.inset + (parent.height - 2 * meter.inset) * (1 - meter.peakAt) - height / 2
            color: meter.peakDb > meter.theme.meterOverDb ? meter.theme.meterHigh : meter.theme.peakHold
        }
    }

    function fromTop(db) {
        return 1 - fraction(db)
    }
}
