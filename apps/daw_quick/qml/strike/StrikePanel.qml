pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Strike device panel: a transient shaper. On the left the last two
// seconds scrolling, the input as a grey ghost under what Strike let out,
// each band's gain over them as a line in its colour (a lift rises, a cut
// falls), and under it the input and output peaks and how far Clip took
// the peaks down. On the right the band count, the phase and the
// crossovers; each band's Attack, Sustain and solo with its gain now; the
// timing with the lookahead and the latency; and the output: mix, level,
// stereo link, and clip under a ceiling. Every control reads and writes
// through `controller` (NativeSynthDeviceController for Strike); the
// readings come only from the telemetry (layout in strike.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property StrikeTheme theme: StrikeTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true
    readonly property real silentDb: -120
    // The meters' bars: -60 dBFS to 0 dBFS for levels, 0 to 12 dB of
    // clipping.
    readonly property real barFloorDb: -60
    readonly property real barTopDb: 0
    readonly property real clipFullDb: 12
    // The latency shown before the first block: at this rate.
    readonly property real nominalRate: 48000
    readonly property int maxBands: 4
    // The view's time ticks: seconds ago (strike::history_seconds).
    readonly property var secondsTicks: [2, 1.5, 1, 0.5]

    readonly property int bands: revision >= 0 ? controller.choice("strike.bands") + 1 : 1
    readonly property bool linear: revision >= 0 && controller.choice("strike.phase") === 1
    readonly property bool clipping: revision >= 0 && controller.choice("strike.clip") === 1
    readonly property real ceilingDb: revision >= 0 ? controller.value("strike.ceiling") : 0
    readonly property var crossovers: revision >= 0
        ? [controller.value("strike.xover_1"), controller.value("strike.xover_2"),
           controller.value("strike.xover_3")] : []
    readonly property bool anySolo: {
        if (revision < 0)
            return false
        for (let b = 0; b < bands; ++b) {
            if (controller.choice(bandId(b, "solo")) === 1)
                return true
        }
        return false
    }
    readonly property real latencyMs: playing ? live.latency_ms
        : (revision >= 0 ? controller.effect_latency_ms(nominalRate) : 0)

    readonly property real inPeak: playing ? Math.max(live.input_peak[0], live.input_peak[1]) : silentDb
    readonly property real outPeak: playing ? Math.max(live.output_peak[0], live.output_peak[1]) : silentDb
    readonly property real clippedDb: playing ? live.clipped : 0

    objectName: "strikePanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    color: theme.caseColor
    border.color: theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Strike transient shaper"

    function poll() {
        live = controller.telemetry()
    }

    // ---- Formatting ------------------------------------------------------------------

    readonly property int levelDecimals: 1
    function bandId(band, part) {
        return "strike.band_" + (band + 1) + "." + part
    }
    function levelText(db) {
        return playing && db > silentDb ? Format.signed_number(db, levelDecimals) + " dBFS" : "—"
    }
    function gainText(db) {
        return playing ? Format.signed_number(db, levelDecimals) + " dB" : "—"
    }
    function hzText(hz) {
        return hz >= 1000 ? Format.number(hz / 1000, levelDecimals) + " kHz" : Format.number(hz, 0) + " Hz"
    }
    // A band's range: under the first crossover, between two, or over the
    // last.
    function bandRange(band) {
        if (bands === 1)
            return "FULL RANGE"
        if (band === 0)
            return "UNDER " + hzText(crossovers[0])
        if (band === bands - 1)
            return "OVER " + hzText(crossovers[bands - 2])
        return hzText(crossovers[band - 1]) + " – " + hzText(crossovers[band])
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
        // The mark (a hit's envelope in a ring) beside the wordmark over the
        // band count and the phase.
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
                        readonly property string plotExemptReason: "Strike's logo, not a plot"
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
                        objectName: "strikeWordmark"
                        text: "STRIKE"
                        color: panel.theme.wordmarkInk
                        font.family: panel.theme.wordmarkFont
                        font.pixelSize: panel.theme.wordmarkSize
                        font.weight: panel.theme.wordmarkWeight
                        font.letterSpacing: panel.theme.wordmarkSpacing
                    }
                    Text {
                        objectName: "strikeSubtitle"
                        text: panel.bands === 1 ? "TRANSIENTS · SINGLE BAND"
                            : "TRANSIENTS · " + panel.bands + " BANDS · "
                              + (panel.linear ? "LINEAR" : "MINIMUM") + " PHASE"
                        color: panel.theme.subtitleInk
                        font.pixelSize: panel.theme.subtitleSize
                        font.bold: true
                        font.letterSpacing: panel.theme.subtitleSpacing
                    }
                }
            }
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
            font.pixelSize: panel.theme.meterValueSize
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

    // ---- Left: the view and the meters --------------------------------------------------

    readonly property real bodyY: theme.bodyY
    readonly property real bodyHeight: height - bodyY - margin

    Kit.Card {
        id: viewCard
        theme: panel.theme
        title: "SHAPE"
        note: !panel.playing ? "IDLE" : panel.anySolo ? "SOLO" : "LIVE"
        noteColor: panel.playing ? panel.theme.accent : panel.theme.inkFaint
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
                                                        label: Format.signed_number(-s, panel.levelDecimals) }))
                .concat([{ at: 1, label: "now" }])
            yTicks: view.gainTicks.slice().reverse().map(db => ({ at: view.gainAt(db),
                                                                  label: Format.signed_number(db, 0) }))
            StrikeView {
                id: view
                objectName: "strikeView"
                anchors.fill: parent
                theme: panel.theme
                live: panel.playing
                inputs: panel.playing ? panel.live.history_input : []
                outputs: panel.playing ? panel.live.history_output : []
                gains: panel.playing ? panel.live.history_gain : []
                bands: panel.bands
                clipping: panel.clipping
                ceilingDb: panel.ceilingDb
            }
        }

        readonly property real metersY: viewFrame.y + viewFrame.height + panel.theme.metersGap
        readonly property real meterWidth: (viewCard.width - 2 * panel.theme.cardInset
                                            - 2 * panel.theme.metersGap) / 3

        Meter {
            x: panel.theme.cardInset
            title: "IN PEAK"
            valueName: "strikeInPeak"
            value: panel.levelText(panel.inPeak)
            level: panel.playing ? panel.fraction(panel.inPeak, panel.barFloorDb, panel.barTopDb) : 0
        }
        Meter {
            x: panel.theme.cardInset + viewCard.meterWidth + panel.theme.metersGap
            title: "OUT PEAK"
            valueName: "strikeOutPeak"
            value: panel.levelText(panel.outPeak)
            level: panel.playing ? panel.fraction(panel.outPeak, panel.barFloorDb, panel.barTopDb) : 0
            barColor: panel.theme.output
        }
        Meter {
            x: panel.theme.cardInset + 2 * (viewCard.meterWidth + panel.theme.metersGap)
            title: "CLIP"
            valueName: "strikeClipped"
            value: !panel.clipping ? "OFF" : panel.playing
                ? Format.signed_number(-panel.clippedDb, panel.levelDecimals) + " dB" : "—"
            level: panel.clipping ? panel.fraction(panel.clippedDb, 0, panel.clipFullDb) : 0
            barColor: panel.theme.warning
            valueColor: panel.clipping ? panel.theme.ink : panel.theme.inkFaint
        }
    }

    // ---- Right: bands, shaping, timing and output ------------------------------------------

    readonly property real rightX: viewCard.x + viewCard.width + gap
    readonly property real rightWidth: width - rightX - margin

    Kit.Card {
        id: bandsCard
        theme: panel.theme
        title: "BANDS"
        note: panel.bands === 1 ? "ONE BAND · NO SPLIT"
            : panel.linear ? "LINEAR PHASE · BANDS SUM TO THE INPUT, DELAYED"
                           : "LINKWITZ-RILEY · NO LATENCY"
        x: panel.rightX
        y: panel.bodyY
        width: panel.rightWidth
        height: panel.theme.bandsHeight
        Kit.SegmentedControl {
            id: bandCount
            x: panel.theme.cardInset
            y: panel.theme.segmentRowY
            width: panel.theme.bandCountWidth
            theme: panel.theme
            controller: panel.controller
            parameterId: "strike.bands"
            labels: ["1", "2", "3", "4"]
        }
        Kit.SegmentedControl {
            x: panel.theme.cardInset
            y: bandCount.y + bandCount.height + panel.theme.spaceMd
            width: panel.theme.bandCountWidth
            theme: panel.theme
            controller: panel.controller
            parameterId: "strike.phase"
            labels: ["Minimum", "Linear"]
        }
        Row {
            x: bandCount.x + bandCount.width + panel.theme.spaceLg
            y: panel.theme.segmentRowY - panel.theme.spaceXs
            spacing: panel.theme.crossoverGap
            Repeater {
                model: panel.maxBands - 1
                delegate: Kit.Knob {
                    required property int index
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "strike.xover_" + (index + 1)
                    diameter: panel.theme.knobDiameterSmall
                    readout: true
                    inactive: index + 1 >= panel.bands
                    inactiveReason: "Crossover " + (index + 1) + " splits " + (index + 2) + " bands or more"
                }
            }
        }
    }

    Kit.Card {
        id: shapeCard
        theme: panel.theme
        title: "ATTACK · SUSTAIN"
        note: "PER BAND · AT ANY LEVEL"
        x: panel.rightX
        y: bandsCard.y + bandsCard.height + panel.gap
        width: panel.rightWidth
        height: panel.theme.shapeHeight
        readonly property real slot: (width - 2 * panel.theme.cardInset) / panel.bands
        readonly property int knobDiameter: panel.bands <= 2 ? panel.theme.knobDiameterLarge
                                                             : panel.theme.knobDiameterMedium
        Repeater {
            model: panel.maxBands
            delegate: Item {
                id: column
                required property int index
                readonly property color colour: panel.theme.bandColours[index]
                visible: index < panel.bands
                x: panel.theme.cardInset + index * shapeCard.slot
                y: panel.theme.cardContentY
                width: shapeCard.slot
                height: shapeCard.height - y - panel.theme.cardInset
                Rectangle {
                    id: swatch
                    y: (bandName.height - height) / 2
                    width: panel.theme.bandSwatch
                    height: width
                    radius: width / 2
                    color: column.colour
                }
                Text {
                    id: bandName
                    x: swatch.width + panel.theme.spaceSm
                    width: column.width - x - panel.theme.spaceSm
                    text: panel.bandRange(column.index)
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "strikeBandGain:" + column.index
                    y: bandName.height + panel.theme.spaceXxs
                    x: swatch.width + panel.theme.spaceSm
                    text: panel.gainText(panel.playing ? panel.live.band_gain[column.index] : 0)
                    color: panel.playing ? column.colour : panel.theme.inkFaint
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeLabel
                }
                Row {
                    id: knobs
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: bandName.height + panel.theme.spaceXl + panel.theme.spaceMd
                    Kit.Knob {
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: panel.bandId(column.index, "attack")
                        diameter: shapeCard.knobDiameter
                        readout: true
                        accent: column.colour
                    }
                    Kit.Knob {
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: panel.bandId(column.index, "sustain")
                        diameter: shapeCard.knobDiameter
                        readout: true
                        accent: column.colour
                    }
                }
                Kit.Switch {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: column.height - height
                    visible: panel.bands > 1
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: panel.bandId(column.index, "solo")
                    label: "SOLO"
                    accent: column.colour
                }
            }
        }
    }

    readonly property real lowerY: shapeCard.y + shapeCard.height + gap
    readonly property real lowerHeight: height - margin - lowerY

    Kit.Card {
        id: timingCard
        theme: panel.theme
        title: "TIMING"
        x: panel.rightX
        y: panel.lowerY
        width: panel.theme.timingWidth
        height: panel.lowerHeight
        Row {
            id: timingKnobs
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: timingCard.width - 2 * panel.theme.cardInset
            Repeater {
                model: ["strike.attack_time", "strike.sustain_time"]
                delegate: Item {
                    id: timing
                    required property string modelData
                    width: timingKnobs.width / 2
                    height: knob.height
                    Kit.Knob {
                        id: knob
                        anchors.horizontalCenter: parent.horizontalCenter
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: timing.modelData
                        diameter: panel.theme.knobDiameterMedium
                        readout: true
                    }
                }
            }
        }
        Caption {
            id: lookaheadCaption
            x: panel.theme.cardInset
            y: timingKnobs.y + timingKnobs.height + panel.theme.spaceMd
            text: "LOOKAHEAD"
        }
        Kit.SegmentedControl {
            id: lookahead
            x: panel.theme.cardInset
            y: lookaheadCaption.y + lookaheadCaption.height + panel.theme.spaceXs
            width: timingCard.width - 2 * panel.theme.cardInset
            theme: panel.theme
            controller: panel.controller
            parameterId: "strike.lookahead"
            labels: ["Off", "0.5", "1", "2", "5"]
        }
        Text {
            objectName: "strikeLatency"
            x: lookahead.x
            y: lookahead.y + lookahead.height + panel.theme.latencyGap
            width: lookahead.width
            text: panel.latencyMs > 0
                ? "Latency " + Format.number(panel.latencyMs, panel.levelDecimals) + " ms · reported for delay compensation"
                : "No latency"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Kit.Card {
        id: outputCard
        theme: panel.theme
        title: "OUTPUT"
        note: panel.clipping ? "CLIPS UNDER " + Format.signed_number(panel.ceilingDb, panel.levelDecimals) + " dBFS" : ""
        x: timingCard.x + timingCard.width + panel.gap
        y: panel.lowerY
        width: panel.rightX + panel.rightWidth - x
        height: panel.lowerHeight
        Row {
            id: outputKnobs
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: outputCard.width - 2 * panel.theme.cardInset
            Repeater {
                model: ["strike.mix", "strike.output", "strike.link"]
                delegate: Item {
                    id: outputSlot
                    required property string modelData
                    width: outputKnobs.width / 3
                    height: outputKnob.height
                    Kit.Knob {
                        id: outputKnob
                        anchors.horizontalCenter: parent.horizontalCenter
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: outputSlot.modelData
                        diameter: panel.theme.knobDiameterMedium
                        readout: true
                    }
                }
            }
        }
        Kit.Switch {
            id: clipSwitch
            x: panel.theme.cardInset
            y: outputKnobs.y + outputKnobs.height + panel.theme.spaceLg
            controller: panel.controller
            theme: panel.theme
            parameterId: "strike.clip"
            label: "CLIP"
            accent: panel.theme.warning
        }
        Kit.Knob {
            x: clipSwitch.x + clipSwitch.width + panel.theme.spaceLg
            y: outputKnobs.y + outputKnobs.height + panel.theme.spaceXs
            controller: panel.controller
            theme: panel.theme
            parameterId: "strike.ceiling"
            diameter: panel.theme.knobDiameterSmall
            readout: true
            inactive: !panel.clipping
            inactiveReason: "Ceiling sets where Clip holds the peaks"
        }
    }

    Timer {
        objectName: "strikeTelemetryPoll"
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
