pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Gild device panel: a multiband exciter. On the left the spectrum,
// the programme as a grey ghost and what Gild adds in gold over the bands'
// shading, and under it the input and output peaks and the level of all
// that is added. On the right the band count, the phase and the
// crossovers; per band its curve, Drive, Even, Mix and solo, with what it
// adds against its own level and the drive reaching its curve; the
// oversampling with the latency; and the output: Dynamic, level, Level
// Match and Delta. Every control reads and writes through `controller`
// (NativeSynthDeviceController for Gild); the readings come only from the
// telemetry (layout in gild.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property GildTheme theme: GildTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true
    readonly property real silentDb: -120
    // The meters' bars: -60 dBFS to 0 dBFS.
    readonly property real barFloorDb: -60
    readonly property real barTopDb: 0
    // The latency shown before the first block: at this rate.
    readonly property real nominalRate: 48000
    readonly property int maxBands: 4
    // gild_table::oversampling's factors, in order.
    readonly property var factors: [1, 2, 4, 8, 16]

    readonly property int bands: revision >= 0 ? controller.choice("gild.bands") + 1 : 1
    readonly property bool linear: revision >= 0 && controller.choice("gild.phase") === 1
    readonly property int factor: revision >= 0 ? factors[controller.choice("gild.oversampling")] : 1
    readonly property bool delta: revision >= 0 && controller.choice("gild.delta") === 1
    readonly property var crossovers: revision >= 0
        ? [controller.value("gild.xover_1"), controller.value("gild.xover_2"),
           controller.value("gild.xover_3")] : []
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
    // All the bands add together (their shares are summed as powers).
    readonly property real addedDb: {
        if (!playing)
            return silentDb
        let power = 0
        for (let b = 0; b < bands; ++b)
            power += Math.pow(10, live.band_added[b] / 10)
        return power > 0 ? 10 * Math.log(power) / Math.LN10 : silentDb
    }

    objectName: "gildPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    color: theme.caseColor
    border.color: theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Gild multiband exciter"

    function poll() {
        live = controller.telemetry()
    }

    // ---- Formatting ------------------------------------------------------------------

    readonly property int levelDecimals: 1
    function bandId(band, part) {
        return "gild.band_" + (band + 1) + "." + part
    }
    function levelText(db) {
        return playing && db > silentDb ? Format.signed_number(db, levelDecimals) + " dBFS" : "—"
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
    // What a band adds against its own level.
    function addedText(band) {
        if (!playing || live.band_added[band] <= silentDb || live.band_level[band] <= silentDb)
            return "—"
        return Format.signed_number(live.band_added[band] - live.band_level[band], levelDecimals) + " dB"
    }
    function driveText(band) {
        if (!playing || live.band_drive[band] <= silentDb)
            return "—"
        return Format.signed_number(live.band_drive[band], levelDecimals) + " dB"
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
        // The mark (a sine carrying its third harmonic in a ring) beside the
        // wordmark over the bands, the phase and the oversampling.
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
                        readonly property string plotExemptReason: "Gild's logo, not a plot"
                        anchors.fill: parent
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const t = panel.theme
                            const w = width
                            const left = w * t.markWaveInset
                            const span = w - 2 * left
                            ctx.strokeStyle = t.accent
                            ctx.lineWidth = t.markStroke
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            for (let i = 0; i <= t.markPoints; ++i) {
                                const at = i / t.markPoints
                                const phase = 2 * Math.PI * at
                                const y = w / 2 - w * (t.markWaveHeight * Math.sin(phase)
                                                       + t.markHarmonicHeight * Math.sin(t.markHarmonic * phase))
                                if (i === 0)
                                    ctx.moveTo(left + span * at, y)
                                else
                                    ctx.lineTo(left + span * at, y)
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
                        objectName: "gildWordmark"
                        text: "GILD"
                        color: panel.theme.wordmarkInk
                        font.family: panel.theme.wordmarkFont
                        font.pixelSize: panel.theme.wordmarkSize
                        font.weight: panel.theme.wordmarkWeight
                        font.letterSpacing: panel.theme.wordmarkSpacing
                    }
                    Text {
                        objectName: "gildSubtitle"
                        text: (panel.bands === 1 ? "EXCITER · SINGLE BAND"
                               : "EXCITER · " + panel.bands + " BANDS · "
                                 + (panel.linear ? "LINEAR" : "MINIMUM") + " PHASE")
                              + " · " + panel.factor + "×"
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
            color: panel.theme.ink
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

    // ---- Left: the spectrum and the meters ------------------------------------------------

    readonly property real bodyY: theme.bodyY
    readonly property real bodyHeight: height - bodyY - margin

    Kit.Card {
        id: viewCard
        theme: panel.theme
        title: "HARMONICS ADDED"
        note: !panel.playing ? "IDLE" : panel.delta ? "DELTA" : panel.anySolo ? "SOLO" : "LIVE"
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
            xUnit: "Hz"
            yUnit: "dBFS"
            xTicks: view.hzTicks.map(hz => ({ at: view.hzAt(hz),
                                              label: hz >= 1000 ? Format.number(hz / 1000, 0) + "k"
                                                                : Format.number(hz, 0) }))
            yTicks: view.dbTicks.slice().reverse().map(db => ({ at: view.dbAt(db),
                                                                label: Format.signed_number(db, 0) }))
            GildView {
                id: view
                objectName: "gildView"
                anchors.fill: parent
                theme: panel.theme
                live: panel.playing
                input: panel.playing ? panel.live.input_spectrum : []
                added: panel.playing ? panel.live.added_spectrum : []
                bands: panel.bands
                crossovers: panel.crossovers
            }
        }

        readonly property real metersY: viewFrame.y + viewFrame.height + panel.theme.metersGap
        readonly property real meterWidth: (viewCard.width - 2 * panel.theme.cardInset
                                            - 2 * panel.theme.metersGap) / 3

        Meter {
            x: panel.theme.cardInset
            title: "IN PEAK"
            valueName: "gildInPeak"
            value: panel.levelText(panel.inPeak)
            level: panel.playing ? panel.fraction(panel.inPeak, panel.barFloorDb, panel.barTopDb) : 0
        }
        Meter {
            x: panel.theme.cardInset + viewCard.meterWidth + panel.theme.metersGap
            title: "OUT PEAK"
            valueName: "gildOutPeak"
            value: panel.levelText(panel.outPeak)
            level: panel.playing ? panel.fraction(panel.outPeak, panel.barFloorDb, panel.barTopDb) : 0
            barColor: panel.theme.output
        }
        Meter {
            x: panel.theme.cardInset + 2 * (viewCard.meterWidth + panel.theme.metersGap)
            title: "ADDED"
            valueName: "gildAdded"
            value: panel.levelText(panel.addedDb)
            level: panel.playing ? panel.fraction(panel.addedDb, panel.barFloorDb, panel.barTopDb) : 0
            barColor: panel.theme.accent
        }
    }

    // ---- Right: bands, harmonics, quality and output ------------------------------------

    readonly property real rightX: viewCard.x + viewCard.width + gap
    readonly property real rightWidth: width - rightX - margin

    Kit.Card {
        id: bandsCard
        theme: panel.theme
        title: "BANDS"
        note: panel.bands === 1 ? "ONE BAND · NO SPLIT"
            : panel.linear ? "LINEAR PHASE · BANDS SUM TO THE INPUT, DELAYED"
                           : "LINKWITZ-RILEY · BANDS SUM FLAT"
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
            parameterId: "gild.bands"
            labels: ["1", "2", "3", "4"]
        }
        Kit.SegmentedControl {
            x: panel.theme.cardInset
            y: bandCount.y + bandCount.height + panel.theme.spaceMd
            width: panel.theme.bandCountWidth
            theme: panel.theme
            controller: panel.controller
            parameterId: "gild.phase"
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
                    parameterId: "gild.xover_" + (index + 1)
                    diameter: panel.theme.knobDiameterSmall
                    readout: true
                    inactive: index + 1 >= panel.bands
                    inactiveReason: "Crossover " + (index + 1) + " splits " + (index + 2) + " bands or more"
                }
            }
        }
    }

    Kit.Card {
        id: harmonicsCard
        theme: panel.theme
        title: "HARMONICS"
        note: "PER BAND · LEVEL-MATCHED"
        x: panel.rightX
        y: bandsCard.y + bandsCard.height + panel.gap
        width: panel.rightWidth
        height: panel.theme.harmonicsHeight
        readonly property real slot: (width - 2 * panel.theme.cardInset) / panel.bands
        Repeater {
            model: panel.maxBands
            delegate: Item {
                id: column
                required property int index
                readonly property color colour: panel.theme.bandColours[index]
                visible: index < panel.bands
                x: panel.theme.cardInset + index * harmonicsCard.slot
                y: panel.theme.cardContentY
                width: harmonicsCard.slot
                height: harmonicsCard.height - y - panel.theme.cardInset
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
                    id: addedReading
                    objectName: "gildBandAdded:" + column.index
                    y: bandName.height + panel.theme.spaceXxs
                    x: swatch.width + panel.theme.spaceSm
                    text: "ADDS " + panel.addedText(column.index)
                    color: panel.playing ? column.colour : panel.theme.inkFaint
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeLabel
                }
                Text {
                    id: driveReading
                    objectName: "gildBandDrive:" + column.index
                    y: addedReading.y + addedReading.height
                    x: addedReading.x
                    text: "DRIVE " + panel.driveText(column.index)
                    color: panel.theme.inkFaint
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeCaption
                }
                Kit.Dropdown {
                    id: curve
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: driveReading.y + driveReading.height + panel.theme.curveGap
                    controller: panel.controller
                    theme: panel.theme
                    menu: menu
                    parameterId: panel.bandId(column.index, "curve")
                    accent: column.colour
                }
                Kit.Knob {
                    id: driveKnob
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: curve.y + curve.height + panel.theme.curveGap
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: panel.bandId(column.index, "drive")
                    diameter: panel.theme.knobDiameterMedium
                    readout: true
                    accent: column.colour
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: driveKnob.y + driveKnob.height + panel.theme.knobRowGap
                    Kit.Knob {
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: panel.bandId(column.index, "even")
                        diameter: panel.theme.knobDiameterSmall
                        readout: true
                        accent: column.colour
                    }
                    Kit.Knob {
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: panel.bandId(column.index, "mix")
                        diameter: panel.theme.knobDiameterSmall
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

    readonly property real lowerY: harmonicsCard.y + harmonicsCard.height + gap
    readonly property real lowerHeight: height - margin - lowerY

    Kit.Card {
        id: qualityCard
        theme: panel.theme
        title: "OVERSAMPLING"
        note: panel.factor >= 8 ? "ANTIALIASED" : ""
        x: panel.rightX
        y: panel.lowerY
        width: panel.theme.qualityWidth
        height: panel.lowerHeight
        Kit.SegmentedControl {
            id: oversampling
            x: panel.theme.cardInset
            y: panel.theme.segmentRowY
            width: qualityCard.width - 2 * panel.theme.cardInset
            theme: panel.theme
            controller: panel.controller
            parameterId: "gild.oversampling"
            labels: ["1×", "2×", "4×", "8×", "16×"]
        }
        Text {
            objectName: "gildLatency"
            x: oversampling.x
            y: oversampling.y + oversampling.height + panel.theme.latencyGap
            width: oversampling.width
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
        note: panel.delta ? "DELTA · ONLY WHAT IS ADDED" : ""
        x: qualityCard.x + qualityCard.width + panel.gap
        y: panel.lowerY
        width: panel.rightX + panel.rightWidth - x
        height: panel.lowerHeight
        Row {
            id: outputKnobs
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Repeater {
                model: ["gild.dynamic", "gild.output"]
                delegate: Item {
                    id: outputSlot
                    required property string modelData
                    width: outputKnob.width + panel.theme.spaceLg
                    height: outputKnob.height
                    Kit.Knob {
                        id: outputKnob
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: outputSlot.modelData
                        diameter: panel.theme.knobDiameterMedium
                        readout: true
                    }
                }
            }
        }
        Column {
            anchors.left: outputKnobs.right
            anchors.leftMargin: panel.theme.spaceLg
            y: panel.theme.cardContentY
            spacing: panel.theme.spaceMd
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "gild.match"
                label: "LEVEL MATCH"
            }
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "gild.delta"
                label: "DELTA"
                accent: panel.theme.warning
            }
        }
    }

    Timer {
        objectName: "gildTelemetryPoll"
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
