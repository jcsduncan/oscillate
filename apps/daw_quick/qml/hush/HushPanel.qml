pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Hush device panel: a gate and expander. On the left the last three
// seconds scrolling, the input as a grey ghost and what the gate let
// through in lilac, with the open threshold solid and the close threshold
// dashed, and under it the input, the gain reduction and the output. On
// the right Threshold, Hysteresis, Range and Ratio; Attack, Hold and
// Release with the lookahead and its latency; the mode, flip and stereo
// link; and the key: input or sidechain, its high-pass and low-pass, and
// listen. Every control reads and writes through `controller`
// (NativeSynthDeviceController for Hush); the readings come only from the
// telemetry (layout in hush.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property HushTheme theme: HushTheme {}
    // Set by the host when a sidechain send keys this slot.
    property bool keyed: false
    property string keyName: "the sidechain"

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true
    readonly property real silentDb: -120
    // The meters' bars: -60 dBFS to 0 dBFS for levels, 0 to 40 dB of
    // reduction.
    readonly property real barFloorDb: -60
    readonly property real barTopDb: 0
    readonly property real reductionFullDb: 40
    // Range at its minimum mutes (hush_table::mute_range_db).
    readonly property real muteRangeDb: -80
    readonly property real reductionMuteDb: 80
    // The view's time ticks: seconds ago (hush::history_seconds).
    readonly property var secondsTicks: [3, 2, 1]

    readonly property bool expander: revision >= 0 && controller.choice("hush.mode") === 1
    readonly property bool flipped: revision >= 0 && controller.choice("hush.flip") === 1
    readonly property bool sidechainKey: revision >= 0 && controller.choice("hush.key") === 1
    readonly property bool listening: revision >= 0 && controller.choice("hush.listen") === 1
    readonly property real thresholdDb: revision >= 0 ? controller.value("hush.threshold") : 0
    readonly property real closeDb: revision >= 0
        ? thresholdDb - controller.value("hush.hysteresis") : 0
    readonly property bool muting: revision >= 0 && controller.value("hush.range") <= muteRangeDb
    readonly property string lookaheadText: revision >= 0 ? controller.display("hush.lookahead") : ""
    readonly property real latencyMs: playing ? live.latency_ms
        : (lookaheadText === "Off" ? 0 : parseFloat(lookaheadText))

    readonly property real inPeak: playing ? Math.max(live.input_peak[0], live.input_peak[1]) : silentDb
    readonly property real outPeak: playing ? Math.max(live.output_peak[0], live.output_peak[1]) : silentDb
    readonly property real reductionDb: playing ? live.reduction : 0
    readonly property bool open: playing && live.open > 0

    objectName: "hushPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    color: theme.caseColor
    border.color: theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Hush gate"

    function poll() {
        live = controller.telemetry()
    }

    // ---- Formatting ------------------------------------------------------------------

    readonly property int levelDecimals: 1
    function levelText(db) {
        return playing && db > silentDb ? Format.signed_number(db, levelDecimals) + " dBFS" : "—"
    }
    function reductionText(db) {
        if (!playing)
            return "—"
        return db >= reductionMuteDb ? "−∞ dB" : Format.number(-db, levelDecimals) + " dB"
    }
    function fraction(value, low, high) {
        return Math.max(0, Math.min(1, (value - low) / (high - low)))
    }

    component Caption: Text {
        color: panel.theme.inkDim
        font.pixelSize: panel.theme.typeSmall
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
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
        // The mark (a gate opening and closing in a ring) beside the
        // wordmark over the mode and the key.
        brand: Component {
            Item {
                implicitWidth: words.x + words.implicitWidth
                implicitHeight: Math.max(mark.height, words.implicitHeight)
                Rectangle {
                    id: mark
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.theme.markSize
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.color: panel.theme.accent
                    border.width: panel.theme.markStroke
                    Canvas {
                        readonly property string plotExemptReason: "Hush's logo, not a plot"
                        anchors.fill: parent
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const w = width
                            ctx.strokeStyle = panel.theme.accent
                            ctx.lineWidth = panel.theme.markStroke
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            const points = panel.theme.markPoints
                            for (let i = 0; i < points.length; ++i) {
                                if (i === 0)
                                    ctx.moveTo(w * points[i][0], w * points[i][1])
                                else
                                    ctx.lineTo(w * points[i][0], w * points[i][1])
                            }
                            ctx.stroke()
                        }
                    }
                }
                Column {
                    id: words
                    x: mark.width + panel.theme.markGap
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: panel.theme.subtitleGap
                    Text {
                        objectName: "hushWordmark"
                        text: "HUSH"
                        color: panel.theme.wordmarkInk
                        font.family: panel.theme.wordmarkFont
                        font.pixelSize: panel.theme.wordmarkSize
                        font.weight: panel.theme.wordmarkWeight
                        font.letterSpacing: panel.theme.wordmarkSpacing
                    }
                    Text {
                        objectName: "hushSubtitle"
                        text: (panel.flipped ? "DUCKER" : panel.expander ? "EXPANDER" : "GATE")
                            + " · " + (panel.sidechainKey && panel.keyed ? "KEYED" : "SELF-KEYED")
                        color: panel.theme.subtitleInk
                        font.pixelSize: panel.theme.subtitleSize
                        font.bold: true
                        font.letterSpacing: panel.theme.subtitleSpacing
                    }
                }
            }
        }
    }

    Rectangle {
        id: keyedBadge
        objectName: "hushKeyed"
        visible: panel.keyed && panel.sidechainKey
        x: header.x + header.status.x - width - panel.theme.keyedBadgeGap
        y: header.y + (header.height - height) / 2
        width: keyedText.implicitWidth + panel.theme.keyedBadgePad
        height: panel.theme.keyedBadgeHeight
        radius: height / 2
        color: Qt.rgba(panel.theme.key.r, panel.theme.key.g, panel.theme.key.b, panel.theme.keyedFillAlpha)
        border.color: panel.theme.key
        Accessible.role: Accessible.StaticText
        Accessible.name: "Keyed from " + panel.keyName
        Text {
            id: keyedText
            anchors.centerIn: parent
            text: "KEYED · " + panel.keyName.toUpperCase()
            color: panel.theme.key
            font.pixelSize: panel.theme.typeCaption
            font.bold: true
            font.letterSpacing: panel.theme.keyedSpacing
        }
    }

    component Meter: Item {
        id: meter
        required property string title
        required property string valueName
        required property string value
        required property real level
        property color barColor: panel.theme.inkDim
        property color valueColor: panel.theme.ink
        property int valueSize: panel.theme.meterValueSize
        y: viewCard.metersY
        width: viewCard.meterWidth
        height: viewCard.height - panel.theme.cardInset - y
        Caption {
            text: meter.title
        }
        Text {
            objectName: meter.valueName
            y: panel.theme.spaceXl
            text: meter.value
            color: meter.valueColor
            font.family: panel.theme.mono
            font.pixelSize: meter.valueSize
            font.bold: meter.valueSize > panel.theme.meterValueSize
        }
        Rectangle {
            y: meter.height - height
            width: meter.width
            height: panel.theme.meterBarHeight
            radius: panel.theme.meterBarRadius
            color: panel.theme.grid
            Rectangle {
                width: parent.width * meter.level
                height: parent.height
                radius: parent.radius
                color: meter.barColor
            }
        }
    }

    // ---- Left: the gate view and the meters ---------------------------------------------

    readonly property real bodyY: theme.bodyY
    readonly property real bodyHeight: height - bodyY - margin

    Kit.Card {
        id: viewCard
        theme: panel.theme
        title: "GATE"
        note: !panel.playing ? "IDLE" : panel.open ? "OPEN" : "CLOSED"
        noteColor: panel.open ? panel.theme.accent : panel.theme.inkFaint
        x: panel.margin
        y: panel.bodyY
        width: panel.theme.viewWidth
        height: panel.bodyHeight
        Kit.PlotFrame {
            id: viewFrame
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: viewCard.width - 2 * panel.theme.cardInset
            height: panel.theme.plotHeight
            theme: panel.theme
            live: panel.playing
            xUnit: "s"
            yUnit: "dB"
            xTicks: panel.secondsTicks.map((s, i) => ({ at: i / panel.secondsTicks.length,
                                                        label: Format.signed_number(-s, 0) }))
                .concat([{ at: 1, label: "now" }])
            yTicks: scope.dbTicks.map(db => ({ at: 0.5 + scope.levelAt(db) / 2,
                                               label: Format.signed_number(db, 0) }))
            HushScope {
                id: scope
                objectName: "hushScope"
                anchors.fill: parent
                theme: panel.theme
                live: panel.playing
                peaks: panel.playing ? panel.live.history_peak : []
                gains: panel.playing ? panel.live.history_gain : []
                openDb: panel.thresholdDb
                closeDb: panel.closeDb
            }
        }

        readonly property real metersY: viewFrame.y + viewFrame.height + panel.theme.metersGap
        readonly property real meterWidth: (viewCard.width - 2 * panel.theme.cardInset
                                            - 2 * panel.theme.metersGap) / 3

        Meter {
            x: panel.theme.cardInset
            title: "IN PEAK"
            valueName: "hushInPeak"
            value: panel.levelText(panel.inPeak)
            level: panel.playing ? panel.fraction(panel.inPeak, panel.barFloorDb, panel.barTopDb) : 0
        }
        Meter {
            x: panel.theme.cardInset + viewCard.meterWidth + panel.theme.metersGap
            title: "REDUCTION"
            valueName: "hushReduction"
            value: panel.reductionText(panel.reductionDb)
            level: panel.fraction(panel.reductionDb, 0, panel.reductionFullDb)
            barColor: panel.theme.accent
            valueColor: panel.playing ? panel.theme.accent : panel.theme.inkFaint
            valueSize: panel.theme.reductionValueSize
        }
        Meter {
            x: panel.theme.cardInset + 2 * (viewCard.meterWidth + panel.theme.metersGap)
            title: "OUT PEAK"
            valueName: "hushOutPeak"
            value: panel.levelText(panel.outPeak)
            level: panel.playing ? panel.fraction(panel.outPeak, panel.barFloorDb, panel.barTopDb) : 0
            barColor: panel.theme.live
        }
    }

    // ---- Right: the gate, timing, mode and key --------------------------------------------

    readonly property real rightX: viewCard.x + viewCard.width + gap
    readonly property real rightWidth: width - rightX - margin

    component Hinted: Column {
        id: hinted
        required property string parameterId
        required property string hint
        property int diameter: panel.theme.knobDiameterLarge
        property bool inactive: false
        property string inactiveReason: ""
        property string valueText: ""
        spacing: panel.theme.knobHintGap
        Kit.Knob {
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: hinted.parameterId
            diameter: hinted.diameter
            readout: true
            inactive: hinted.inactive
            inactiveReason: hinted.inactiveReason
            valueText: hinted.valueText !== "" ? hinted.valueText
                : (panel.controller.revision >= 0 ? panel.controller.display(hinted.parameterId) : "")
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: hinted.hint
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
        }
    }

    Kit.Card {
        id: gateCard
        theme: panel.theme
        title: "THRESHOLD"
        note: "OPENS AT " + Format.signed_number(panel.thresholdDb, panel.levelDecimals)
            + " · CLOSES AT " + Format.signed_number(panel.closeDb, panel.levelDecimals) + " dBFS"
        x: panel.rightX
        y: panel.bodyY
        width: panel.rightWidth
        height: panel.theme.knobsHeight
        readonly property real slot: (width - 2 * panel.theme.cardInset) / 4
        Row {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Hinted {
                width: gateCard.slot
                parameterId: "hush.threshold"
                hint: "where it opens"
            }
            Hinted {
                width: gateCard.slot
                parameterId: "hush.hysteresis"
                hint: "closes this much lower"
            }
            Hinted {
                width: gateCard.slot
                parameterId: "hush.range"
                hint: "how far it closes"
                valueText: panel.muting ? "−∞ dB" : ""
            }
            Hinted {
                width: gateCard.slot
                parameterId: "hush.ratio"
                hint: "expander slope"
                inactive: !panel.expander
                inactiveReason: "Ratio shapes the expander"
            }
        }
    }

    Kit.Card {
        id: timingCard
        theme: panel.theme
        title: "TIMING"
        x: panel.rightX
        y: gateCard.y + gateCard.height + panel.gap
        width: panel.rightWidth
        height: panel.theme.knobsHeight
        readonly property real knobsWidth: (width - 2 * panel.theme.cardInset) * panel.theme.timingKnobsShare
        Row {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Repeater {
                model: [
                    { id: "hush.attack", hint: "opening" },
                    { id: "hush.hold", hint: "stays open" },
                    { id: "hush.release", hint: "closing" }
                ]
                delegate: Hinted {
                    required property var modelData
                    width: timingCard.knobsWidth / 3
                    parameterId: modelData.id
                    hint: modelData.hint
                }
            }
        }
        Caption {
            x: panel.theme.cardInset + timingCard.knobsWidth
            y: panel.theme.cardContentY
            text: "LOOKAHEAD"
        }
        Kit.SegmentedControl {
            id: lookahead
            x: panel.theme.cardInset + timingCard.knobsWidth
            y: panel.theme.cardContentY + panel.theme.spaceXl
            width: timingCard.width - x - panel.theme.cardInset
            theme: panel.theme
            controller: panel.controller
            parameterId: "hush.lookahead"
            labels: ["Off", "0.5", "1", "2", "5", "10"]
        }
        Text {
            objectName: "hushLatency"
            x: lookahead.x
            y: lookahead.y + lookahead.height + panel.theme.latencyGap
            width: lookahead.width
            text: panel.latencyMs > 0
                ? "Opens " + Format.number(panel.latencyMs, 1) + " ms ahead · reported for delay compensation"
                : "No lookahead · no latency"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    readonly property real lowerY: timingCard.y + timingCard.height + gap
    readonly property real lowerHeight: height - margin - lowerY

    Kit.Card {
        id: modeCard
        theme: panel.theme
        title: "MODE"
        x: panel.rightX
        y: panel.lowerY
        width: panel.theme.modeWidth
        height: panel.lowerHeight
        Kit.SegmentedControl {
            id: mode
            x: panel.theme.cardInset
            y: panel.theme.segmentRowY
            width: modeCard.width - 2 * panel.theme.cardInset
            theme: panel.theme
            controller: panel.controller
            parameterId: "hush.mode"
            labels: ["Gate", "Expander"]
        }
        HushSwitch {
            x: panel.theme.cardInset
            y: mode.y + mode.height + panel.theme.spaceMd
            width: (modeCard.width - 2 * panel.theme.cardInset) / 2 - panel.theme.spaceMd
            controller: panel.controller
            theme: panel.theme
            parameterId: "hush.flip"
            label: "Flip"
        }
        Kit.Knob {
            x: modeCard.width - panel.theme.cardInset - width
            y: mode.y + mode.height
            controller: panel.controller
            theme: panel.theme
            parameterId: "hush.link"
            diameter: panel.theme.knobDiameterSmall
            readout: true
        }
    }

    Kit.Card {
        id: keyCard
        theme: panel.theme
        title: "KEY"
        note: panel.sidechainKey ? (panel.keyed ? "FOLLOWS " + panel.keyName.toUpperCase()
                                                 : "SIDECHAIN · NONE ROUTED, FOLLOWS THE INPUT")
                                 : "FOLLOWS THE INPUT"
        noteColor: panel.sidechainKey && panel.keyed ? panel.theme.key : panel.theme.inkFaint
        x: modeCard.x + modeCard.width + panel.gap
        y: panel.lowerY
        width: panel.rightX + panel.rightWidth - x
        height: panel.lowerHeight
        Kit.SegmentedControl {
            id: keySource
            x: panel.theme.cardInset
            y: panel.theme.segmentRowY
            width: panel.theme.keySegmentWidth
            theme: panel.theme
            controller: panel.controller
            parameterId: "hush.key"
            labels: ["Input", "Sidechain"]
        }
        HushSwitch {
            x: panel.theme.cardInset
            y: keySource.y + keySource.height + panel.theme.spaceMd
            width: panel.theme.keySegmentWidth
            controller: panel.controller
            theme: panel.theme
            parameterId: "hush.listen"
            label: "Listen to the key"
        }
        Row {
            x: keySource.x + keySource.width + panel.theme.spaceLg
            y: panel.theme.segmentRowY - panel.theme.spaceXs
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "hush.key_hp"
                diameter: panel.theme.knobDiameterSmall
                readout: true
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "hush.key_lp"
                diameter: panel.theme.knobDiameterSmall
                readout: true
            }
        }
    }

    Timer {
        objectName: "hushTelemetryPoll"
        interval: panel.playing ? panel.theme.pollPlayingMs : panel.theme.pollIdleMs
        repeat: true
        running: panel.visible
        onTriggered: panel.poll()
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
