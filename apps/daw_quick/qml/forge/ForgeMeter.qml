import QtQuick

// A vertical level meter cut into the steel: RMS in dBFS from -48 to +6,
// the bar glowing from copper to white-hot near full scale.
Item {
    id: meter
    required property ForgeTheme theme
    required property string label
    // Linear RMS.
    property real level: 0
    property bool live: false
    readonly property real floorDb: -48
    readonly property real topDb: 6
    readonly property real db: level > 0.000001 ? 20 * Math.log(level) / Math.LN10 : -120
    readonly property real fraction: live ? Math.max(0, Math.min(1, (db - floorDb) / (topDb - floorDb))) : 0

    objectName: "forgeMeter:" + label.toLowerCase()
    implicitWidth: theme.meterWidth
    implicitHeight: theme.meterHeight
    Accessible.role: Accessible.Indicator
    Accessible.name: label + " level"
    Accessible.description: live ? db.toFixed(1) + " dBFS" : "Silent"

    Rectangle {
        id: track
        x: (parent.width - width) / 2
        y: 0
        width: meter.theme.meterTrackWidth
        height: parent.height - meter.theme.meterFootRoom
        radius: meter.theme.radiusXs
        color: meter.theme.well
        border.color: meter.theme.wellEdge
        clip: true
        Rectangle {
            x: meter.theme.meterBarInset
            width: parent.width - 2 * meter.theme.meterBarInset
            y: parent.height - meter.theme.meterBarInset - height
            height: (parent.height - 2 * meter.theme.meterBarInset) * meter.fraction
            radius: meter.theme.meterBarRadius
            gradient: Gradient {
                GradientStop { position: 0; color: meter.theme.whiteHot }
                GradientStop { position: meter.theme.meterAmberStop; color: meter.theme.amber }
                GradientStop { position: meter.theme.meterEmberStop; color: meter.theme.ember }
                GradientStop { position: 1; color: meter.theme.heatCold }
            }
            Behavior on height { NumberAnimation { duration: 60 } }
        }
        // 0 dBFS line.
        Rectangle {
            y: (parent.height - meter.theme.meterBarInset) * (1 - (0 - meter.floorDb) / (meter.topDb - meter.floorDb))
            width: parent.width
            height: meter.theme.hairline
            color: Qt.rgba(meter.theme.amber.r, meter.theme.amber.g, meter.theme.amber.b, meter.theme.meterZeroAlpha)
        }
    }
    // Scale ticks.
    Repeater {
        model: [0, -12, -24, -36]
        delegate: Text {
            required property int modelData
            x: track.x + track.width + meter.theme.meterScaleGap
            y: track.y + (track.height - meter.theme.meterBarInset)
                * (1 - (modelData - meter.floorDb) / (meter.topDb - meter.floorDb)) - meter.theme.meterScaleLift
            text: modelData === 0 ? "0" : modelData.toString()
            color: meter.theme.inkFaint
            font.family: meter.theme.mono
            font.pixelSize: meter.theme.scaleTextSize
        }
    }
    Text {
        y: track.height + meter.theme.spaceXs
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: meter.label.toUpperCase()
        color: meter.theme.inkDim
        font.pixelSize: meter.theme.typeMicro
        font.bold: true
        font.letterSpacing: meter.theme.labelSpacing
    }
    Text {
        objectName: "forgeMeterValue:" + meter.label.toLowerCase()
        y: track.height + meter.theme.meterValueY
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: meter.live && meter.db > -100 ? meter.db.toFixed(1) : "—"
        color: meter.live ? meter.theme.molten : meter.theme.inkFaint
        font.family: meter.theme.mono
        font.pixelSize: meter.theme.typeCaption
    }
}
