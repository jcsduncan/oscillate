pragma ComponentBehavior: Bound
import QtQuick
import "../mixer"
import "../shell"

// One role's card: its tracks, a gain fader beside the role's meter (the
// tracks its stage acts on), the EQ's response and the three bands' knobs.
// The fader and knobs drive the role object directly (the device kit's
// controller interface), so a drag is one undo step.
Rectangle {
    id: card
    required property RebalanceTheme style
    // A RebalanceRole.
    required property var role
    // The MeterModel the role's meter reads from.
    required property var meters
    required property real lowHz
    required property real highHz
    required property real rangeDb
    property bool active: true

    readonly property color roleColor: style.roleColor(role.id)
    readonly property var meter: meters ? meters.channel(role.meterId) : null
    readonly property bool meterValid: !!meter && meter.valid
    readonly property var gainInfo: role.parameter("gain")
    // The fader's rule: no change.
    readonly property real unityDb: 0
    // The bands: a label and its knobs' parameters.
    readonly property var bands: [
        { label: "LOW", parameters: ["low", "lowHz"] },
        { label: "MID", parameters: ["mid", "midHz", "midQ"] },
        { label: "HIGH", parameters: ["high", "highHz"] }
    ]
    readonly property int knobsPerBand: bands.reduce((most, band) => Math.max(most, band.parameters.length), 0)
    readonly property var bandCentres: role.revision >= 0
        ? [role.value("lowHz"), role.value("midHz"), role.value("highHz")] : []

    objectName: "rebalanceRole:" + role.id
    implicitWidth: style.cardWidth
    radius: style.cardRadius
    color: style.panel
    border.color: style.line
    Accessible.role: Accessible.Grouping
    Accessible.name: role.name

    Column {
        id: content
        x: card.style.cardPadding
        y: card.style.cardPadding
        width: card.width - 2 * card.style.cardPadding
        spacing: card.style.spaceMd

        Item {
            id: cardHeader
            width: parent.width
            height: card.style.cardHeaderHeight
            Rectangle {
                id: dot
                anchors.verticalCenter: parent.verticalCenter
                width: card.style.roleDotSize
                height: width
                radius: width / 2
                color: card.roleColor
            }
            Text {
                anchors.left: dot.right
                anchors.leftMargin: card.style.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                text: card.role.name
                color: card.style.textMain
                font.family: card.style.sansFamily
                font.pixelSize: card.style.roleNameSize
                font.bold: true
            }
            Text {
                id: reset
                objectName: "rebalanceReset:" + card.role.id
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "RESET"
                color: card.role.neutral ? card.style.textFaint
                     : resetArea.containsMouse ? card.style.textMain : card.style.textDim
                font.pixelSize: card.style.typeCaption
                font.bold: true
                font.letterSpacing: card.style.trackingSm
                Accessible.role: Accessible.Button
                Accessible.name: "Reset " + card.role.name
                MouseArea {
                    id: resetArea
                    anchors.fill: parent
                    anchors.margins: -card.style.spaceSm
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.role.reset()
                    Tip { text: "Gain and EQ back to flat" }
                }
            }
        }

        Text {
            id: trackLine
            objectName: "rebalanceTracks:" + card.role.id
            readonly property bool tipShowsWhole: true
            width: parent.width
            elide: Text.ElideRight
            text: card.role.trackNames.length > 0 ? card.role.trackNames.join(" · ") : "No tracks"
            color: card.role.trackNames.length > 0 ? card.style.textDim : card.style.textFaint
            font.pixelSize: card.style.typeSmall
            Tip { text: parent.text }
        }

        // The gain fader with its scale, and the role's meter: as tall as the
        // card leaves room for, never shorter than roleFaderHeight.
        Item {
            width: parent.width
            height: Math.max(card.style.roleFaderHeight,
                             card.height - 2 * card.style.cardPadding - cardHeader.height - trackLine.height
                             - readout.height - curvePlot.height - bandRows.height
                             - (content.children.length - 1) * content.spacing)
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                height: parent.height
                spacing: card.style.faderGap
                Item {
                    width: card.style.scaleWidth
                    height: parent.height
                    Repeater {
                        model: card.style.scaleMarks
                        delegate: Text {
                            required property var modelData
                            readonly property real position: (Number(modelData) + card.rangeDb) / (2 * card.rangeDb)
                            anchors.right: parent.right
                            y: fader.travelInset + (1 - position) * (parent.height - 2 * fader.travelInset) - height / 2
                            text: (Number(modelData) > card.unityDb ? "+" : "") + String(modelData).replace("-", "−")
                            color: Number(modelData) === card.unityDb ? card.style.textMain : card.style.textFaint
                            font.pixelSize: card.style.typeMicro
                            font.family: card.style.monoFamily
                        }
                    }
                }
                MixerFader {
                    id: fader
                    objectName: "rebalanceFader:" + card.role.id
                    height: parent.height
                    controller: card.role
                    parameterId: "gain"
                    accessibleName: card.role.name + " gain"
                    accent: card.roleColor
                    detent: (card.unityDb - card.gainInfo.minimum) / (card.gainInfo.maximum - card.gainInfo.minimum)
                    tipText: accessibleName + ": drag, or wheel; Shift for fine, double-click resets"
                    Accessible.description: card.role.display("gain")
                }
                Row {
                    height: parent.height
                    spacing: card.style.meterGap
                    MixerMeter {
                        objectName: "rebalanceMeterL:" + card.role.id
                        height: parent.height
                        inset: fader.travelInset
                        peak: card.meterValid ? card.meter.leftPeak : 0
                        hold: card.meterValid ? card.meter.leftHold : 0
                        valid: card.meterValid
                        clipped: card.meterValid && card.meter.clipped
                    }
                    MixerMeter {
                        objectName: "rebalanceMeterR:" + card.role.id
                        height: parent.height
                        inset: fader.travelInset
                        peak: card.meterValid ? card.meter.rightPeak : 0
                        hold: card.meterValid ? card.meter.rightHold : 0
                        valid: card.meterValid
                        clipped: card.meterValid && card.meter.clipped
                    }
                }
            }
        }

        Text {
            id: readout
            objectName: "rebalanceGain:" + card.role.id
            anchors.horizontalCenter: parent.horizontalCenter
            // A fixed line, so a minus sign's fallback font never shifts the card.
            height: card.style.gainReadoutHeight
            verticalAlignment: Text.AlignVCenter
            text: card.role.revision >= 0 ? card.role.display("gain") : ""
            color: card.active ? card.style.textMain : card.style.textFaint
            font.family: card.style.monoFamily
            font.pixelSize: card.style.gainReadoutSize
        }

        RebalanceCurve {
            id: curvePlot
            objectName: "rebalanceCurve:" + card.role.id
            width: parent.width
            height: card.style.curveHeight
            style: card.style
            curve: card.role.curve
            lowHz: card.lowHz
            highHz: card.highHz
            rangeDb: card.rangeDb
            color: card.roleColor
            bands: card.bandCentres
            active: card.active && !card.role.neutral
        }

        Column {
            id: bandRows
            width: parent.width
            spacing: card.style.bandRowSpacing
            Repeater {
                model: card.bands
                delegate: Item {
                    id: band
                    required property var modelData
                    width: parent.width
                    height: knobs.height
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: band.modelData.label
                        color: card.style.textDim
                        font.pixelSize: card.style.typeCaption
                        font.bold: true
                        font.letterSpacing: card.style.trackingSm
                    }
                    Row {
                        id: knobs
                        x: card.style.bandLabelWidth
                        Repeater {
                            model: band.modelData.parameters
                            delegate: Item {
                                id: slot
                                required property string modelData
                                // The widest band's knobs share the room beside the label.
                                width: Math.max(card.style.knobSlotWidth,
                                                (band.width - card.style.bandLabelWidth) / card.knobsPerBand)
                                height: knob.implicitHeight
                                MixerKnob {
                                    id: knob
                                    objectName: "rebalanceKnob:" + card.role.id + ":" + slot.modelData
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    controller: card.role
                                    parameterId: slot.modelData
                                    accessibleName: card.role.name + " " + band.modelData.label.toLowerCase()
                                                    + " " + label.toLowerCase()
                                    accent: card.roleColor
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
