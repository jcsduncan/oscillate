pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Press device panel: a studio bus compressor in warm graphite and
// amber. The transfer curve leads, with the threshold, knee and ratio drawn
// on it and the live operating point riding it; beside it the gain
// reduction as a bar hanging from 0 dB, the input and output peak meters,
// and the dynamics dials. Below: attack and release, the detector's
// high-pass (and whether a sidechain keys it), and the makeup. Every
// control reads and writes through `controller` (NativeSynthDeviceController
// for Press); the curve comes from press_transfer and the meters only from
// the telemetry (layout in press.hpp).
Rectangle {
    id: panel
    required property var controller
    // Whether a sidechain send keys the detector, and the track that does.
    property bool keyed: false
    property string keyName: "the sidechain"
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property PressTheme theme: PressTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int revision: controller.revision
    // Under this a meter reads silence.
    readonly property real floorDb: -60
    // Bare level readouts (no unit beside them) and the ratio: one decimal.
    readonly property int levelDecimals: 1

    readonly property bool playing: live.live === true
    readonly property var inPeak: playing ? live.input_peak : [-200, -200]
    readonly property var outPeak: playing ? live.output_peak : [-200, -200]
    readonly property real reductionDb: playing ? live.reduction : 0
    readonly property bool highPass: revision >= 0 && controller.choice("press.detector_hp") === 1
    readonly property real ratio: revision >= 0 ? controller.value("press.ratio") : 4
    readonly property real threshold: revision >= 0 ? controller.value("press.threshold") : -18
    readonly property real knee: revision >= 0 ? controller.value("press.knee") : 6

    objectName: "pressPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.caseTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.caseColor }
        GradientStop { position: 1; color: panel.theme.caseBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Press compressor"

    // ---- Formatting ------------------------------------------------------------------

    // A bare level, −∞ at the floor.
    function dbText(db) {
        return Format.signed_number(db <= floorDb ? -Infinity : db, levelDecimals)
    }
    function louder(pair) {
        return Math.max(pair[0], pair[1])
    }
    // What the curve does, in a line.
    function summary() {
        if (ratio <= 1.0001)
            return "1:1 · NO COMPRESSION"
        return Format.number(ratio, levelDecimals) + ":1 ABOVE " + Format.signed_decibels(threshold)
            + (knee > 0 ? " · " + Format.decibels(knee) + " KNEE" : " · HARD KNEE")
    }

    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }
    component Note: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeCaption
        wrapMode: Text.WordWrap
    }

    // ---- Header ----------------------------------------------------------------------

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        brand: Component {
            Item {
                implicitWidth: wordmark.implicitWidth
                implicitHeight: subtitle.y + subtitle.implicitHeight
                Text {
                    id: wordmark
                    objectName: "pressWordmark"
                    text: "PRESS"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Rectangle {
                    x: panel.theme.wordmarkInset
                    y: wordmark.height + panel.theme.underlineGap
                    width: wordmark.implicitWidth - panel.theme.underlineTrim
                    height: panel.theme.strokeBold
                    radius: height / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: panel.theme.accent }
                        GradientStop { position: 1; color: panel.theme.reduction }
                    }
                }
                Text {
                    id: subtitle
                    objectName: "pressSubtitle"
                    x: panel.theme.wordmarkInset
                    y: wordmark.height + panel.theme.subtitleGap
                    text: "COMPRESSOR · " + (panel.keyed ? "KEYED" : "SELF-KEYED")
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
        }
    }

    // The key: lit when another track's sidechain send drives the detector.
    // A tag, not a status: left of the header's status badge.
    Rectangle {
        id: keyedBadge
        objectName: "pressKeyed"
        visible: panel.keyed
        x: header.x + header.status.x - width - panel.theme.keyedBadgeGap
        y: header.y + (header.rowHeight - height) / 2
        width: keyedText.implicitWidth + panel.theme.keyedBadgePad
        height: panel.theme.statusHeight
        radius: height / 2
        color: Qt.rgba(panel.theme.key.r, panel.theme.key.g, panel.theme.key.b, panel.theme.keyedFillAlpha)
        border.color: Qt.rgba(panel.theme.key.r, panel.theme.key.g, panel.theme.key.b, panel.theme.statusEdgeAlpha)
        Accessible.role: Accessible.StaticText
        Accessible.name: "Keyed from " + panel.keyName
        Text {
            id: keyedText
            objectName: "pressKeyedText"
            anchors.centerIn: parent
            text: "KEYED · " + panel.keyName.toUpperCase()
            color: panel.theme.key
            font.pixelSize: panel.theme.typeSmall
            font.bold: true
            font.letterSpacing: panel.theme.keyedSpacing
        }
    }

    // ---- Row A: transfer, reduction, levels, dynamics ------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Kit.Card {
        id: transferPlate
        theme: panel.theme
        title: "TRANSFER"
        note: panel.summary()
        noteColor: panel.theme.accent
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.transferWidth
        height: panel.rowAHeight
        PressTransfer {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: transferPlate.width - 2 * panel.theme.cardInset
            height: transferPlate.height - panel.theme.cardContentY - panel.theme.cardInset
            controller: panel.controller
            theme: panel.theme
            playing: panel.playing
            inputDb: panel.louder(panel.inPeak)
            reductionDb: panel.reductionDb
        }
    }

    Kit.Card {
        id: reductionPlate
        theme: panel.theme
        title: "REDUCTION"
        note: panel.playing ? heldNote : ""
        longestNote: heldNote
        readonly property string heldNote: "HELD 1 s"
        noteColor: panel.theme.live
        x: transferPlate.x + transferPlate.width + panel.gap
        y: panel.rowAY
        width: Math.max(panel.theme.reductionWidth, Math.ceil(fitWidth))
        height: panel.rowAHeight
        PressReduction {
            x: panel.theme.reductionBarX
            y: panel.theme.cardBodyY
            height: panel.theme.reductionBarHeight
            theme: panel.theme
            live: panel.playing
            reductionDb: panel.reductionDb
        }
        Text {
            objectName: "pressReduction"
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.reductionValueY
            text: panel.playing ? Format.number(-panel.reductionDb, panel.levelDecimals) : "—"
            color: !panel.playing ? panel.theme.inkFaint
                : panel.reductionDb < 0.05 ? panel.theme.inkDim : panel.theme.reduction
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.reductionValueSize
            font.weight: Font.Light
        }
        Caption {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.reductionCaptionY
            text: "dB GAIN REDUCTION"
        }
    }

    Kit.Card {
        id: meterPlate
        theme: panel.theme
        title: "LEVELS"
        note: panel.playing ? "PEAK" : "NO SIGNAL"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: reductionPlate.x + reductionPlate.width + panel.gap
        y: panel.rowAY
        width: panel.theme.levelsWidth
        height: panel.rowAHeight

        readonly property real meterHeight: panel.theme.meterHeight

        component Pair: Item {
            id: pair
            required property string title
            required property var peak
            required property string prefix
            width: panel.theme.meterPairWidth
            height: meterPlate.meterHeight + panel.theme.meterPairHead
            Caption {
                anchors.horizontalCenter: parent.horizontalCenter
                text: pair.title
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                y: panel.theme.peakRowY
                spacing: panel.theme.spaceXs
                Repeater {
                    model: 2
                    delegate: Text {
                        required property int index
                        objectName: pair.prefix + "Peak" + (index === 0 ? "L" : "R")
                        width: panel.theme.peakTextWidth
                        horizontalAlignment: Text.AlignHCenter
                        text: panel.playing ? panel.dbText(pair.peak[index]) : "—"
                        color: panel.playing && pair.peak[index] > -3 ? panel.theme.meterOver
                            : panel.playing ? panel.theme.ink : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                y: panel.theme.meterRowY
                spacing: panel.theme.spaceMd
                Repeater {
                    model: 2
                    delegate: Kit.PeakMeter {
                        required property int index
                        objectName: pair.prefix + "Meter" + (index === 0 ? "L" : "R")
                        theme: panel.theme
                        width: panel.theme.meterWidth
                        height: meterPlate.meterHeight
                        live: panel.playing
                        floorDb: panel.floorDb
                        peakDb: pair.peak[index]
                    }
                }
            }
        }

        Pair {
            x: panel.theme.cardPad
            y: panel.theme.cardBodyY
            title: "IN"
            prefix: "pressIn"
            peak: panel.inPeak
        }
        Pair {
            x: meterPlate.width - panel.theme.cardPad - width
            y: panel.theme.cardBodyY
            title: "OUT"
            prefix: "pressOut"
            peak: panel.outPeak
        }
        Note {
            x: panel.theme.cardPad
            y: meterPlate.height - panel.theme.levelsNoteBottom
            width: meterPlate.width - 2 * panel.theme.cardPad
            horizontalAlignment: Text.AlignHCenter
            text: "−60 to +6 dBFS"
        }
    }

    Kit.Card {
        id: dynamicsPlate
        theme: panel.theme
        title: "DYNAMICS"
        x: meterPlate.x + meterPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        Kit.Knob {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.thresholdKnobY
            controller: panel.controller
            theme: panel.theme
            parameterId: "press.threshold"
            diameter: panel.theme.thresholdDiameter
            readout: true
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.dialRowY
            spacing: panel.theme.dialRowSpacing
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "press.ratio"
                diameter: panel.theme.dialDiameter
                readout: true
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "press.knee"
                diameter: panel.theme.dialDiameter
                readout: true
            }
        }
        Note {
            x: panel.theme.spaceXl
            y: dynamicsPlate.height - panel.theme.dynamicsNoteBottom
            width: dynamicsPlate.width - 2 * panel.theme.spaceXl
            horizontalAlignment: Text.AlignHCenter
            text: "Drag across the curve to set the threshold; scroll over it for the ratio."
        }
    }

    // ---- Row B: timing, detector, output ----------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: panel.height - rowBY - margin

    Kit.Card {
        id: timingPlate
        theme: panel.theme
        title: "TIMING"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.timingWidth
        height: panel.rowBHeight
        Row {
            x: panel.theme.cardPad
            y: panel.theme.cardBodyY
            spacing: panel.theme.timingSpacing
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "press.attack"
                diameter: panel.theme.dialDiameter
                readout: true
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "press.release"
                diameter: panel.theme.dialDiameter
                readout: true
            }
            Note {
                width: timingPlate.width
                    - 2 * (panel.theme.dialDiameter + panel.theme.knobWidthPad + panel.theme.timingSpacing)
                    - 2 * panel.theme.cardPad
                y: panel.theme.timingNoteY
                text: "Attack is how fast the detector rises to a peak; release, how fast it lets go."
            }
        }
    }

    Kit.Card {
        id: detectorPlate
        theme: panel.theme
        title: "DETECTOR"
        note: panel.keyed ? "FOLLOWS " + panel.keyName.toUpperCase() : "FOLLOWS THE INPUT"
        noteColor: panel.keyed ? panel.theme.key : panel.theme.inkFaint
        x: timingPlate.x + timingPlate.width + panel.gap
        y: panel.rowBY
        width: panel.theme.detectorWidth
        height: panel.rowBHeight
        Kit.Toggle {
            x: panel.theme.cardPad
            y: panel.theme.toggleY
            width: panel.theme.toggleWidth
            height: panel.theme.toggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "press.detector_hp"
            onCaption: "LOWS IGNORED BY THE DETECTOR"
            offCaption: "FULL RANGE"
        }
        Kit.Knob {
            x: panel.theme.detectorKnobX
            y: panel.theme.cardBodyY
            controller: panel.controller
            theme: panel.theme
            parameterId: "press.detector_freq"
            diameter: panel.theme.dialDiameter
            readout: true
            inactive: !panel.highPass
            inactiveReason: panel.highPass ? "" : "The detector's high-pass is off"
        }
        Note {
            x: panel.theme.cardPad
            y: panel.theme.detectorNoteY
            width: panel.theme.detectorNoteWidth
            text: panel.keyed
                ? "Another track keys the detector: the input is compressed as " + panel.keyName + " gets louder."
                : "The detector listens to the input. Route a sidechain send here to key it from another track."
        }
    }

    Kit.Card {
        id: outputPlate
        theme: panel.theme
        title: "OUTPUT"
        x: detectorPlate.x + detectorPlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        Kit.Knob {
            x: panel.theme.cardPad
            y: panel.theme.cardBodyY
            controller: panel.controller
            theme: panel.theme
            parameterId: "press.makeup"
            diameter: panel.theme.dialDiameter
            readout: true
        }
        Note {
            x: panel.theme.outputNoteX
            y: panel.theme.outputNoteY
            width: outputPlate.width - panel.theme.outputNoteX - panel.theme.cardPad
            text: "Makeup lifts the whole output after the gain reduction, and moves the curve with it."
        }
    }

    Timer {
        objectName: "pressTelemetryPoll"
        interval: panel.playing ? 33 : 250
        repeat: true
        running: panel.visible
        onTriggered: panel.live = panel.controller.telemetry()
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
