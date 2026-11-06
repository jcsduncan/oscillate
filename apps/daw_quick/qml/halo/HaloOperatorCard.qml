pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// One operator: its role in the current algorithm (carrier, modulator,
// feedback), frequency (ratio x fine, or fixed Hz), waveform, levels and
// envelope. Carriers wear amber, modulators teal.
Rectangle {
    id: card
    required property var controller
    required property HaloTheme theme
    required property Kit.Menu menu
    required property int op // 1..6
    property bool carrier: false
    property bool feedback: false
    // A feedback loop across operators: the operator feeding this one
    // (0-based), or the one this operator feeds back into; -1 for none.
    property int loopFrom: -1
    property int loopInto: -1

    readonly property string prefix: "halo.op" + op + "."
    readonly property color accent: carrier ? theme.amber : theme.teal
    readonly property bool fixedMode: controller.revision >= 0 && controller.choice(prefix + "mode") === 1
    readonly property string frequencyText: {
        if (controller.revision < 0)
            return ""
        if (fixedMode)
            return controller.display(prefix + "fixed")
        const choice = controller.choice(prefix + "ratio")
        const ratio = (choice === 0 ? 0.5 : choice) * (1 + controller.value(prefix + "fine"))
        return "×" + ratio.toFixed(2)
    }

    objectName: "haloOperator:" + op
    radius: theme.cardRadius
    gradient: Gradient {
        GradientStop { position: 0; color: card.theme.plateHi }
        GradientStop { position: card.theme.operatorSheenStop; color: card.theme.plate }
        GradientStop { position: 1; color: card.theme.plate }
    }
    border.color: card.theme.plateEdge
    Accessible.role: Accessible.Grouping
    Accessible.name: "Operator " + op + (carrier ? ", carrier" : ", modulator")
        + (loopFrom >= 0 ? ", feedback from operator " + (loopFrom + 1)
           : loopInto >= 0 ? ", feeds back into operator " + (loopInto + 1)
           : feedback ? ", feedback" : "")

    // Role strip and title.
    Rectangle {
        x: card.theme.hairline
        y: card.theme.hairline
        width: parent.width - 2 * card.theme.hairline
        height: card.theme.roleStripHeight
        radius: card.theme.radiusXs
        color: card.accent
    }
    Text {
        x: card.theme.cardTitleX
        y: card.theme.operatorTitleY
        text: "OP " + card.op
        color: card.theme.ink
        font.pixelSize: card.theme.typeSubtitle
        font.weight: Font.Black
        font.letterSpacing: card.theme.operatorTitleSpacing
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: card.theme.spaceMd
        y: card.theme.badgeRowY
        spacing: card.theme.spaceXs
        Rectangle {
            objectName: "haloFeedbackBadge:" + card.op
            visible: card.feedback || card.loopInto >= 0
            width: fbText.implicitWidth + card.theme.badgePad
            height: card.theme.badgeHeight
            radius: card.theme.radiusXs
            color: "transparent"
            border.color: card.theme.red
            Text {
                id: fbText
                anchors.centerIn: parent
                text: card.loopFrom >= 0 ? "FB ← " + (card.loopFrom + 1)
                    : card.loopInto >= 0 ? "FB → " + (card.loopInto + 1) : "FB"
                color: card.theme.red
                font.pixelSize: card.theme.typeMicro
                font.bold: true
                font.letterSpacing: card.theme.badgeSpacing
            }
        }
        Rectangle {
            objectName: "haloOperatorRole:" + card.op
            readonly property string role: card.carrier ? "CARRIER" : "MOD"
            width: roleText.implicitWidth + card.theme.badgePad
            height: card.theme.badgeHeight
            radius: card.theme.radiusXs
            color: card.carrier ? card.theme.amber : "transparent"
            border.color: card.accent
            Text {
                id: roleText
                anchors.centerIn: parent
                text: parent.role
                color: card.carrier ? card.theme.lcd : card.theme.teal
                font.pixelSize: card.theme.typeMicro
                font.bold: true
                font.letterSpacing: card.theme.badgeSpacing
            }
        }
    }

    // Frequency readout.
    Rectangle {
        id: readout
        x: card.theme.spaceMd
        y: card.theme.readoutTop
        width: card.width - 2 * card.theme.spaceMd
        height: card.theme.readoutHeight
        radius: card.theme.radiusSm
        color: card.theme.lcd
        border.color: card.theme.lcdEdge
        Text {
            objectName: "haloFrequency:" + card.op
            x: card.theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            text: card.frequencyText
            color: card.carrier ? card.theme.lcdAmber : card.theme.lcdText
            font.family: card.theme.mono
            font.pixelSize: card.theme.readoutSize
            font.bold: true
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: card.theme.readoutDetuneRight
            anchors.verticalCenter: parent.verticalCenter
            text: card.controller.revision >= 0 ? card.controller.display(card.prefix + "detune") : ""
            color: card.theme.lcdDim
            font.family: card.theme.mono
            font.pixelSize: card.theme.typeCaption
        }
    }

    // Mode, ratio and waveform.
    Kit.Segment {
        id: modeKeys
        x: card.theme.spaceMd
        y: card.theme.operatorModeY
        width: card.theme.operatorModeWidth
        height: card.theme.compactControlHeight
        controller: card.controller
        theme: card.theme
        parameterId: card.prefix + "mode"
        accent: card.accent
        labels: ["RATIO", "FIXED"]
    }
    Kit.Stepper {
        x: modeKeys.x + modeKeys.width + card.theme.spaceSm
        y: card.theme.operatorModeY
        width: card.width - x - card.theme.spaceMd
        height: card.theme.compactControlHeight
        controller: card.controller
        theme: card.theme
        parameterId: card.prefix + "ratio"
        accent: card.accent
        prefix: "×"
        opacity: card.fixedMode ? card.theme.stepperInactiveOpacity : 1
    }
    Kit.Segment {
        x: card.theme.spaceMd
        y: card.theme.operatorWaveY
        width: card.width - 2 * card.theme.spaceMd
        height: card.theme.compactControlHeight
        controller: card.controller
        theme: card.theme
        parameterId: card.prefix + "wave"
        accent: card.accent
        glyph: card.theme.paintWaveGlyph
    }

    // Levels and tuning.
    Grid {
        x: (card.width - width) / 2
        y: card.theme.operatorKnobsY
        columns: 3
        columnSpacing: 0
        rowSpacing: card.theme.spaceXxs
        Repeater {
            model: ["level", "velocity", "key", "detune", "fine", "fixed"]
            delegate: Item {
                id: cell
                required property string modelData
                width: (card.width - 2 * card.theme.spaceSm) / 3
                height: card.theme.operatorKnobCellHeight
                Kit.Knob {
                    anchors.horizontalCenter: parent.horizontalCenter
                    controller: card.controller
                    theme: card.theme
                    parameterId: card.prefix + cell.modelData
                    diameter: cell.modelData === "level" ? card.theme.operatorLevelDiameter : card.theme.operatorKnobDiameter
                    accent: card.accent
                    inactive: (cell.modelData === "fine" && card.fixedMode)
                        || (cell.modelData === "fixed" && !card.fixedMode)
                }
            }
        }
    }

    // The operator's four-rate envelope as the kernel runs it, on an LCD
    // strip in the chip's log units: L1, L2 and L3, a short hold, then
    // release to L4 (the dashed line lets go). Decays are straight lines, as
    // in decibels.
    Kit.EnvelopeView {
        id: envelope
        readonly property var sketch: card.controller.revision >= 0
            ? card.controller.operator_envelope(card.op - 1, card.theme.envelopeSamples) : ({})
        // Lengths from here on read to one decimal.
        readonly property real longSeconds: 10
        objectName: "haloEnvelope:" + (card.op - 1)
        x: card.theme.spaceMd
        y: card.theme.operatorEnvelopeY
        width: card.width - 2 * card.theme.spaceMd
        height: card.theme.operatorEnvelopeHeight
        controller: card.controller
        theme: card.theme
        accent: card.accent
        levels: sketch.points !== undefined ? sketch.points : []
        markers: sketch.seconds !== undefined ? [{ x: sketch.release / sketch.seconds }] : []
        Accessible.name: "Operator " + card.op + " envelope"
        Text {
            anchors.right: parent.right
            anchors.rightMargin: card.theme.envelopeLabelInset
            y: card.theme.envelopeLabelTop
            text: envelope.sketch.seconds !== undefined
                ? envelope.sketch.seconds.toFixed(envelope.sketch.seconds < envelope.longSeconds ? 2 : 1) + " s" : ""
            color: card.theme.envelopeLabelInk
            font.family: card.theme.envelopeLabelFont
            font.pixelSize: card.theme.envelopeLabelSize
        }
    }
    // The four rates (and keyboard rate scaling), then the four levels, on
    // the chip's 0-99 scales.
    Repeater {
        model: [["r1", "r2", "r3", "r4", "rate_scale"], ["l1", "l2", "l3", "l4"]]
        delegate: Row {
            id: envelopeRow
            required property var modelData
            required property int index
            // Both rows start at the same edge, so each level sits under
            // its rate.
            x: card.theme.spaceSm
            y: card.theme.operatorRatesY + index * card.theme.operatorRateRowHeight
            Repeater {
                model: envelopeRow.modelData
                delegate: Kit.Knob {
                    required property string modelData
                    width: (card.width - 2 * card.theme.spaceSm) / 5
                    controller: card.controller
                    theme: card.theme
                    parameterId: card.prefix + modelData
                    diameter: card.theme.operatorRateDiameter
                    accent: card.accent
                    label: modelData === "rate_scale" ? "KS" : modelData.toUpperCase()
                    longName: "Op " + card.op + " " + (info.name !== undefined ? info.name : modelData)
                }
            }
        }
    }
}
