pragma ComponentBehavior: Bound
import QtQuick

// The bar meter: momentary and short-term loudness on the EBU scale, in LU
// relative to the target (EBU +9: -18 to +9 LU; EBU +18: -36 to +18 LU),
// each with its maximum as a fine tick; beside them left and right true
// peak in dBTP against the ceiling. Readings at or below -199 are silent.
Item {
    id: meter
    required property LoudnessTheme theme
    property real momentary: -200
    property real shortTerm: -200
    property real momentaryMax: -200
    property real shortTermMax: -200
    property real truePeakLeft: -200
    property real truePeakRight: -200
    property real target: -14
    property real ceiling: -1
    // The scale's top in LU: 9 or 18; the bottom is twice that below 0.
    property real scaleTop: 9

    readonly property real scaleBottom: -2 * scaleTop
    readonly property real peakTop: 3
    readonly property real peakBottom: -30
    readonly property real barTop: theme.meterBarTop
    readonly property real barHeight: height - barTop - theme.meterBarBottom
    // LU between scale ticks: 3 on EBU +9, 6 on EBU +18.
    readonly property real tickStep: scaleTop > 9 ? 6 : 3
    // Readings at or below this are silent.
    readonly property real silent: -199
    // On target within this many LU; close (amber) within nearLu.
    readonly property real onTargetLu: 1
    readonly property real nearLu: 2
    // The true-peak scale's marks in dBTP.
    readonly property var peakMarks: [3, 0, -3, -6, -12, -18, -24, -30]
    // How long a bar takes to follow a new reading (ms).
    readonly property int barEase: 60

    objectName: "loudnessMeter"

    // Fraction up the loudness scale for a reading in LUFS.
    function loudnessFraction(lufs) {
        if (lufs <= silent)
            return 0
        const lu = lufs - target
        return Math.max(0, Math.min(1, (lu - scaleBottom) / (scaleTop - scaleBottom)))
    }
    function peakFraction(dbtp) {
        if (dbtp <= silent)
            return 0
        return Math.max(0, Math.min(1, (dbtp - peakBottom) / (peakTop - peakBottom)))
    }
    function luY(lu) {
        return barTop + barHeight * (1 - (lu - scaleBottom) / (scaleTop - scaleBottom))
    }
    function peakY(db) {
        return barTop + barHeight * (1 - (db - peakBottom) / (peakTop - peakBottom))
    }
    // Colour for a loudness reading against the target.
    function tone(lufs) {
        const lu = lufs - target
        return Math.abs(lu) <= onTargetLu ? theme.onTarget : lu > onTargetLu ? (lu > nearLu ? theme.off : theme.near)
                                                         : theme.shortTerm
    }

    readonly property real momentaryFraction: loudnessFraction(momentary)
    readonly property real shortTermFraction: loudnessFraction(shortTerm)
    readonly property real leftPeakFraction: peakFraction(truePeakLeft)
    readonly property real rightPeakFraction: peakFraction(truePeakRight)

    readonly property real scaleX: theme.meterScaleWidth
    readonly property real barWidth: theme.meterBarWidth
    readonly property real momentaryX: scaleX + theme.spaceSm
    readonly property real shortTermX: momentaryX + barWidth + theme.spaceMd
    readonly property real peakX: shortTermX + barWidth + theme.meterPeakGap
    readonly property real peakWidth: theme.meterPeakWidth
    readonly property real peakSpacing: theme.spaceXs

    // The LU scale: a tick every 3 LU (every 6 on the +18 scale), labelled.
    Repeater {
        model: Math.round((meter.scaleTop - meter.scaleBottom) / meter.tickStep) + 1
        delegate: Item {
            id: tick
            required property int index
            readonly property real lu: meter.scaleTop - tick.index * meter.tickStep
            Rectangle {
                x: meter.scaleX - meter.theme.meterTickOverhang
                y: meter.luY(tick.lu)
                width: meter.shortTermX + meter.barWidth - meter.scaleX + 2 * meter.theme.meterTickOverhang
                height: meter.theme.hairline
                color: tick.lu === 0 ? meter.theme.zeroTick : meter.theme.grid
            }
            Text {
                x: 0
                width: meter.scaleX - meter.theme.spaceMd
                y: meter.luY(tick.lu) - meter.theme.scaleLabelLift
                horizontalAlignment: Text.AlignRight
                text: tick.lu > 0 ? "+" + tick.lu : tick.lu === 0 ? "0" : "−" + (-tick.lu)
                color: tick.lu === 0 ? meter.theme.accent : meter.theme.inkFaint
                font.family: meter.theme.mono
                font.pixelSize: meter.theme.typeCaption
            }
        }
    }

    component Bar: Rectangle {
        id: bar
        required property real fraction
        required property real maxFraction
        required property color fill
        required property string name
        property string objectId: ""
        objectName: bar.objectId
        y: meter.barTop
        width: meter.barWidth
        height: meter.barHeight
        radius: meter.theme.radiusSm
        color: meter.theme.well
        border.color: meter.theme.wellEdge
        Rectangle {
            x: meter.theme.meterFillInset
            width: parent.width - 2 * meter.theme.meterFillInset
            y: parent.height - height - meter.theme.hairline
            height: Math.max(0, (parent.height - 2 * meter.theme.hairline) * bar.fraction)
            radius: meter.theme.radiusXs
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.lighter(bar.fill, meter.theme.meterFillLift) }
                GradientStop { position: 1; color: Qt.rgba(bar.fill.r, bar.fill.g, bar.fill.b, meter.theme.meterFillBottomAlpha) }
            }
            Behavior on height { NumberAnimation { duration: meter.barEase } }
        }
        // The maximum, as a fine bright tick.
        Rectangle {
            visible: bar.maxFraction > 0
            x: meter.theme.hairline
            width: parent.width - 2 * meter.theme.hairline
            y: (parent.height - 2 * meter.theme.hairline) * (1 - bar.maxFraction)
            height: meter.theme.meterMaxTick
            color: meter.theme.glacier
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height + meter.theme.spaceXs
            text: bar.name
            color: meter.theme.inkDim
            font.pixelSize: meter.theme.typeCaption
            font.bold: true
            font.letterSpacing: meter.theme.meterNameSpacing
        }
    }

    Bar {
        x: meter.momentaryX
        fraction: meter.momentaryFraction
        maxFraction: meter.loudnessFraction(meter.momentaryMax)
        fill: meter.momentary > meter.silent ? meter.tone(meter.momentary) : meter.theme.momentary
        name: "M"
        objectId: "loudnessMeterMomentary"
    }
    Bar {
        x: meter.shortTermX
        fraction: meter.shortTermFraction
        maxFraction: meter.loudnessFraction(meter.shortTermMax)
        fill: meter.shortTerm > meter.silent ? meter.tone(meter.shortTerm) : meter.theme.shortTerm
        name: "S"
        objectId: "loudnessMeterShortTerm"
    }

    // True peak: its own dBTP scale, the ceiling as a red line.
    Repeater {
        model: meter.peakMarks
        delegate: Item {
            id: peakTick
            required property real modelData
            Rectangle {
                x: meter.peakX - meter.theme.meterPeakTickOverhang
                y: meter.peakY(peakTick.modelData)
                width: 2 * meter.peakWidth + meter.peakSpacing + 2 * meter.theme.meterPeakTickOverhang
                height: meter.theme.hairline
                color: meter.theme.grid
            }
            Text {
                x: meter.peakX + 2 * meter.peakWidth + meter.peakSpacing + meter.theme.spaceSm
                y: meter.peakY(peakTick.modelData) - meter.theme.scaleLabelLift
                text: peakTick.modelData > 0 ? "+" + peakTick.modelData
                    : peakTick.modelData === 0 ? "0" : "−" + (-peakTick.modelData)
                color: meter.theme.inkFaint
                font.family: meter.theme.mono
                font.pixelSize: meter.theme.typeCaption
            }
        }
    }
    component PeakBar: Rectangle {
        id: peakBar
        required property real fraction
        required property real dbtp
        required property string name
        property string objectId: ""
        objectName: peakBar.objectId
        y: meter.barTop
        width: meter.peakWidth
        height: meter.barHeight
        radius: meter.theme.radiusXs
        color: meter.theme.well
        border.color: meter.theme.wellEdge
        readonly property bool over: dbtp > meter.ceiling
        Rectangle {
            x: meter.theme.meterFillInset
            width: parent.width - 2 * meter.theme.meterFillInset
            y: parent.height - height - meter.theme.hairline
            height: Math.max(0, (parent.height - 2 * meter.theme.hairline) * peakBar.fraction)
            radius: meter.theme.meterPeakFillRadius
            color: peakBar.over ? meter.theme.off : meter.theme.peak
            opacity: meter.theme.meterPeakFillOpacity
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height + meter.theme.spaceXs
            text: peakBar.name
            color: peakBar.over ? meter.theme.off : meter.theme.inkDim
            font.pixelSize: meter.theme.typeCaption
            font.bold: true
        }
    }
    PeakBar {
        x: meter.peakX
        fraction: meter.leftPeakFraction
        dbtp: meter.truePeakLeft
        name: "L"
        objectId: "loudnessMeterPeakLeft"
    }
    PeakBar {
        x: meter.peakX + meter.peakWidth + meter.peakSpacing
        fraction: meter.rightPeakFraction
        dbtp: meter.truePeakRight
        name: "R"
        objectId: "loudnessMeterPeakRight"
    }
    Rectangle {
        objectName: "loudnessMeterCeiling"
        x: meter.peakX - meter.theme.meterCeilingOverhang
        y: meter.peakY(meter.ceiling) - meter.theme.meterCeilingWidth / 2
        width: 2 * meter.peakWidth + meter.peakSpacing + 2 * meter.theme.meterCeilingOverhang
        height: meter.theme.meterCeilingWidth
        color: meter.theme.off
    }
    Text {
        x: meter.peakX - meter.theme.spaceSm
        y: meter.barTop - meter.theme.meterUnitLift
        text: "dBTP"
        color: meter.theme.inkFaint
        font.pixelSize: meter.theme.typeMicro
        font.bold: true
        font.letterSpacing: meter.theme.meterUnitSpacing
    }
    Text {
        x: meter.momentaryX
        y: meter.barTop - meter.theme.meterUnitLift
        text: "LU"
        color: meter.theme.inkFaint
        font.pixelSize: meter.theme.typeMicro
        font.bold: true
        font.letterSpacing: meter.theme.meterUnitSpacing
    }
}
