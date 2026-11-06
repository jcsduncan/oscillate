pragma ComponentBehavior: Bound
import QtQuick

// The glue compressor's gain-reduction meter. While the synth is running the
// segments light down from the top with the live reduction (ADR-0037), 2 dB
// each; otherwise the bar rests at zero. The marker shows where Glue puts
// the threshold (GlueCompressor: -24 dB x amount).
Item {
    id: meter
    required property PulseTheme theme
    required property real glue
    // Live gain reduction in dB (>= 0); 0 when nothing is reported.
    property real reduction: 0

    readonly property real range: 24
    // GlueCompressor's threshold at full Glue (dB).
    readonly property real fullGlueThreshold: -24
    readonly property real threshold: fullGlueThreshold * glue
    // Twelve 2 dB segments; a segment lights once the reduction passes its
    // top by this much (dB).
    readonly property int segments: 12
    readonly property real segmentLitMargin: 0.25
    readonly property var scaleMarks: [0, range / 2, range]

    objectName: "pulseGlueMeter"
    implicitWidth: theme.glueMeterWidth
    implicitHeight: theme.glueMeterHeight
    Accessible.role: Accessible.Indicator
    Accessible.name: "Glue gain reduction"
    Accessible.description: "Threshold " + threshold.toFixed(1) + " dB, reducing "
        + reduction.toFixed(1) + " dB"

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 0
        text: "GR"
        color: meter.theme.ink
        font.pixelSize: meter.theme.typeSmall
        font.bold: true
        font.letterSpacing: meter.theme.legendSpacing
    }
    Rectangle {
        id: well
        anchors.horizontalCenter: parent.horizontalCenter
        y: meter.theme.glueWellTop
        width: meter.theme.glueWellWidth
        height: parent.height - 2 * meter.theme.glueWellTop
        radius: meter.theme.radiusXs
        color: meter.theme.lcd
        border.color: meter.theme.seam
        Repeater {
            model: meter.segments
            delegate: Rectangle {
                required property int index
                // Segment n covers 2n..2n+2 dB of reduction, from the top.
                readonly property bool lit: meter.reduction > index * meter.range / meter.segments
                    + meter.segmentLitMargin
                readonly property real pitch: (well.height - 2 * meter.theme.glueSegmentPad) / meter.segments
                objectName: "pulseGlueSegment:" + index
                x: meter.theme.glueSegmentInset
                y: meter.theme.glueSegmentPad + index * (well.height - 2 * meter.theme.glueSegmentPad) / meter.segments
                width: well.width - 2 * meter.theme.glueSegmentInset
                height: pitch - meter.theme.glueSegmentGap
                radius: meter.theme.glueSegmentRadius
                color: lit ? (index < meter.theme.glueHotSegments ? meter.theme.led : meter.theme.red)
                           : meter.theme.ledOff
                opacity: lit ? 1 : meter.theme.glueSegmentOffOpacity
            }
        }
    }
    Rectangle {
        objectName: "pulseGlueThreshold"
        x: well.x - meter.theme.glueMarkerOverhang
        y: well.y + well.height * Math.min(1, -meter.threshold / meter.range) - height / 2
        width: well.width + 2 * meter.theme.glueMarkerOverhang
        height: meter.theme.strokeBold
        color: meter.theme.red
        visible: meter.glue > 0
    }
    Repeater {
        model: meter.scaleMarks
        delegate: Text {
            required property int modelData
            x: well.x + well.width + meter.theme.glueScaleGap
            y: well.y + well.height * modelData / meter.range - meter.theme.glueScaleRise
            text: modelData
            color: meter.theme.inkDim
            font.pixelSize: meter.theme.typeMicro
        }
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        text: "dB"
        color: meter.theme.inkDim
        font.pixelSize: meter.theme.typeCaption
    }
}
