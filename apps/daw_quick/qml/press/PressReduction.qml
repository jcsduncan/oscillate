pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The gain-reduction meter: a bar hanging from 0 dB at the top, down to the
// reduction the compressor applies (held 1 s), on a 0 to `rangeDb` scale
// marked every 3 dB; amber, deepening to orange-red past 12 dB.
Item {
    id: meter
    required property PressTheme theme
    property real reductionDb: 0
    property bool live: false
    property real rangeDb: 24
    // How far the bar reaches, 0 (none) to 1 (the full range).
    readonly property real reductionAt: live ? Math.max(0, Math.min(1, reductionDb / rangeDb)) : 0

    objectName: "pressReductionMeter"
    implicitWidth: theme.reductionMeterWidth
    implicitHeight: theme.reductionMeterHeight

    Rectangle {
        id: well
        x: 0
        width: meter.theme.reductionWellWidth
        height: parent.height
        radius: meter.theme.radiusSm
        color: meter.theme.well
        border.color: meter.theme.wellEdge
        clip: true
        Rectangle {
            objectName: "pressReductionFill"
            x: meter.theme.reductionFillInset
            y: meter.theme.reductionFillInset
            width: parent.width - 2 * meter.theme.reductionFillInset
            height: (parent.height - 2 * meter.theme.reductionFillInset) * meter.reductionAt
            radius: meter.theme.radiusXs
            gradient: Gradient {
                GradientStop { position: 0; color: meter.theme.reduction }
                GradientStop { position: 1; color: meter.reductionDb > 12 ? meter.theme.reductionDeep : meter.theme.reduction }
            }
        }
    }
    // The scale beside the bar.
    Repeater {
        model: Math.floor(meter.rangeDb / 3) + 1
        delegate: Item {
            id: mark
            required property int index
            readonly property real db: index * 3
            x: well.width + meter.theme.reductionScaleGap
            y: meter.theme.reductionFillInset
                + (meter.height - 2 * meter.theme.reductionFillInset) * db / meter.rangeDb
            Rectangle {
                width: meter.theme.reductionTickLength
                height: meter.theme.hairline
                color: meter.theme.gridStrong
            }
            Text {
                x: meter.theme.reductionLabelX
                y: meter.theme.reductionLabelY
                text: Format.number(-mark.db, 0)
                color: mark.db === 0 ? meter.theme.inkDim : meter.theme.inkFaint
                font.family: meter.theme.mono
                font.pixelSize: meter.theme.typeCaption
            }
        }
    }
}
