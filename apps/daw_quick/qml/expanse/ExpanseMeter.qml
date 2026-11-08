pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// A horizontal level meter lit like a band of aurora, -60 to 0 dBFS.
Item {
    id: meter
    required property ExpanseTheme theme
    required property string label
    property real level: 0 // Linear RMS.
    property bool live: false
    property color tone: theme.aurora
    readonly property real db: level > 0 ? 20 * Math.log10(level) : -120
    readonly property real fill: live ? Math.max(0, Math.min(1, (db + 60) / 60)) : 0

    objectName: "expanseMeter:" + label.toLowerCase()
    implicitWidth: theme.meterWidth
    implicitHeight: theme.meterHeight
    Accessible.role: Accessible.ProgressBar
    Accessible.name: label + " level"
    Accessible.description: live ? Format.decibels(db) : "Not playing"

    Text {
        id: name
        width: meter.theme.meterLabelWidth
        anchors.verticalCenter: parent.verticalCenter
        text: meter.label.toUpperCase()
        color: meter.theme.inkFaint
        font.pixelSize: meter.theme.typeMicro
        font.bold: true
        font.letterSpacing: meter.theme.captionSpacing
    }
    Rectangle {
        id: track
        x: name.width
        anchors.verticalCenter: parent.verticalCenter
        width: meter.width - name.width - meter.theme.meterValueRoom
        height: meter.theme.meterBarHeight
        radius: meter.theme.radiusSm
        color: meter.theme.well
        border.color: meter.theme.seam
        Rectangle {
            width: Math.max(meter.fill > 0 ? meter.theme.meterMinFill : 0, parent.width * meter.fill)
            height: parent.height
            radius: meter.theme.radiusSm
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Qt.rgba(meter.tone.r, meter.tone.g, meter.tone.b, meter.theme.meterFadeAlpha) }
                GradientStop { position: 1; color: meter.tone }
            }
            Behavior on width { NumberAnimation { duration: 80 } }
        }
        Repeater {
            model: 5
            delegate: Rectangle {
                required property int index
                x: track.width * (index + 1) / 6
                y: -meter.theme.spaceXxs
                width: meter.theme.hairline
                height: track.height + 2 * meter.theme.spaceXxs
                color: meter.theme.meterTick
            }
        }
    }
    Text {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: meter.theme.meterValueWidth
        horizontalAlignment: Text.AlignRight
        text: meter.live && meter.db > -119 ? Format.decibels(meter.db) : "—"
        color: meter.live ? meter.theme.ink : meter.theme.inkFaint
        font.family: meter.theme.mono
        font.pixelSize: meter.theme.typeSmall
    }
}
