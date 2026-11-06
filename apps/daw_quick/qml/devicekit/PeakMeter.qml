import QtQuick

// One channel's peak meter: the held peak as a bar in the theme's safe
// colour, hot over -12 dBFS and over above -3, with a bright hold line at
// its top, on a dB scale from `floorDb` to `ceilDb`. Levels at or under the
// floor, and any level while not `live`, show nothing.
Item {
    id: meter
    required property KitTheme theme
    property real peakDb: theme.peakMeterSilentDb
    property bool live: false
    property real floorDb: theme.peakMeterFloorDb
    property real ceilDb: theme.peakMeterCeilDb
    // Where the bar stands, 0 (floor) to 1 (ceiling).
    readonly property real peakAt: live ? fraction(peakDb) : 0

    objectName: theme.prefix + "PeakMeter"
    implicitWidth: theme.peakMeterWidth
    implicitHeight: theme.peakMeterHeight

    function fraction(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / (ceilDb - floorDb)))
    }
    function fromTop(db) {
        return 1 - fraction(db)
    }

    Rectangle {
        id: well
        anchors.fill: parent
        radius: meter.theme.radiusSm
        color: meter.theme.meterWell
        border.color: meter.theme.wellEdge
        clip: true
        // The colour zones, revealed from the bottom up to the peak.
        Item {
            objectName: meter.theme.prefix + "PeakMeterFill"
            x: meter.theme.peakMeterInset
            width: parent.width - 2 * meter.theme.peakMeterInset
            anchors.bottom: parent.bottom
            anchors.bottomMargin: meter.theme.peakMeterInset
            height: (parent.height - 2 * meter.theme.peakMeterInset) * meter.peakAt
            clip: true
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: well.height - 2 * meter.theme.peakMeterInset
                radius: meter.theme.radiusXs
                gradient: Gradient {
                    GradientStop { position: 0; color: meter.theme.meterOver }
                    GradientStop { position: meter.fromTop(meter.theme.peakMeterOverDb); color: meter.theme.meterOver }
                    GradientStop {
                        position: meter.fromTop(meter.theme.peakMeterOverDb) + meter.theme.peakMeterZoneEdge
                        color: meter.theme.meterHot
                    }
                    GradientStop { position: meter.fromTop(meter.theme.peakMeterHotDb); color: meter.theme.meterHot }
                    GradientStop {
                        position: meter.fromTop(meter.theme.peakMeterHotDb) + meter.theme.peakMeterZoneEdge
                        color: meter.theme.meterSafe
                    }
                    GradientStop { position: 1; color: Qt.darker(meter.theme.meterSafe, meter.theme.peakMeterSafeFade) }
                }
            }
        }
        // The hold line.
        Rectangle {
            visible: meter.peakAt > 0
            x: meter.theme.hairline
            width: parent.width - 2 * meter.theme.hairline
            height: meter.theme.peakMeterHoldHeight
            y: meter.theme.peakMeterInset + (parent.height - 2 * meter.theme.peakMeterInset) * (1 - meter.peakAt)
               - meter.theme.peakMeterHoldHeight / 2
            color: meter.peakDb > meter.theme.peakMeterOverDb ? meter.theme.meterOver : meter.theme.meterHold
        }
    }
}
