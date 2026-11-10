pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Ceiling device panel: a true-peak mastering limiter in charcoal and
// coral. Row A: the gain reduction as a history over the last ten seconds
// hanging from 0 dB, with the reduction now and its largest; the input and
// output meters with the ceiling drawn across the output; the output's
// loudness against the target, with Learn to move Gain by the difference.
// Row B: Drive (gain and the ceiling, large), Limit (lookahead, release,
// character), Clip (transients, clip, shape, with what the clipper and the
// guard took) and Stereo (the two links). Every control reads and writes
// through `controller` (NativeSynthDeviceController for Ceiling); the
// readings come only from the telemetry (layout in ceiling.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property CeilingTheme theme: CeilingTheme {}
    // The reduction over the last `historySeconds`: [[seconds, dB], ...].
    property var samples: []
    readonly property real historySeconds: 10

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    // The meters' scale.
    readonly property real floorDb: -60
    readonly property real ceilDb: 6
    // A peak above this reads in the over colour.
    readonly property real overDb: -3
    // A loudness reading with nothing to read (ceiling::no_loudness).
    readonly property real noLoudness: -70
    // The gain's range, for Learn (ceiling.gain).
    readonly property real gainMin: 0
    readonly property real gainMax: 24
    // When the panel cannot read the running latency, it shows it at 48 kHz.
    readonly property real nominalRate: 48000

    readonly property bool playing: live.live === true
    readonly property var inPeak: playing ? live.input_peak : [-200, -200]
    readonly property var outPeak: playing ? live.output_peak : [-200, -200]
    readonly property real reductionDb: playing ? live.reduction : 0
    readonly property real clippedDb: playing ? live.clipped : 0
    readonly property real guardedDb: playing ? live.guarded : 0
    readonly property real integrated: playing ? live.integrated : noLoudness
    readonly property real shortTerm: playing ? live.short_term : noLoudness
    readonly property real momentary: playing ? live.momentary : noLoudness
    readonly property real latencyMs: playing ? live.latency_ms
        : (revision >= 0 ? controller.ceiling_latency_ms(nominalRate) : 0)
    readonly property real ceiling: revision >= 0 ? controller.value("ceiling.ceiling") : -1
    readonly property real gain: revision >= 0 ? controller.value("ceiling.gain") : 0
    readonly property real target: revision >= 0 ? controller.value("ceiling.target") : -14
    readonly property bool measured: integrated > noLoudness
    readonly property real largestDb: {
        let most = 0
        for (let i = 0; i < samples.length; ++i)
            most = Math.max(most, samples[i][1])
        return most
    }

    objectName: "ceilingPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.caseTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.caseColor }
        GradientStop { position: 1; color: panel.theme.caseBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Ceiling limiter"

    // Read the telemetry, and while playing add the reduction to the
    // history, keeping a little more than it shows.
    function poll() {
        live = controller.telemetry()
        if (!playing)
            return
        const now = Date.now() / 1000
        const next = samples.concat([[now, live.reduction]])
        let first = 0
        while (first < next.length && now - next[first][0] > historySeconds + 1)
            ++first
        samples = first > 0 ? next.slice(first) : next
    }
    function clearHistory() {
        samples = []
    }
    // Learn: move Gain by the distance from the integrated loudness to the
    // target. Limiting makes loudness grow more slowly than gain, so a
    // second Learn on the same passage closes what remains.
    function learn() {
        if (!measured)
            return
        const wanted = Math.max(gainMin, Math.min(gainMax, gain + target - integrated))
        theme.lastTouched = "ceiling.gain"
        controller.set_value("ceiling.gain", wanted)
    }

    // ---- Formatting ------------------------------------------------------------------

    // Level readings hold one decimal, so the figures do not jump in width.
    readonly property int levelDecimals: 1
    function signed(value) {
        return Format.signed_number(value, levelDecimals)
    }
    function dbText(db) {
        return db <= floorDb ? Format.number(-Infinity) : signed(db)
    }
    // A cut is shown below zero.
    function reductionText(db) {
        return Format.number(-db, levelDecimals)
    }
    function loudnessText(lufs) {
        return playing && lufs > noLoudness ? signed(lufs) : "—"
    }
    // Where a level sits on the meters, 0 (floor) to 1 (top).
    function meterAt(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / (ceilDb - floorDb)))
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
    // A knob with its value under it and a hint under that.
    component HintedKnob: Column {
        id: hinted
        required property string parameterId
        required property string hint
        property int diameter: panel.theme.knobDiameterLarge
        spacing: panel.theme.knobHintGap
        Kit.Knob {
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: hinted.parameterId
            diameter: hinted.diameter
            readout: true
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: hinted.hint
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
        }
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
        // The wordmark over its accent rule, and the latency it costs.
        brand: Component {
            Item {
                implicitWidth: Math.max(wordmark.implicitWidth, latency.x + latency.width)
                implicitHeight: latency.y + latency.height
                Text {
                    id: wordmark
                    objectName: "ceilingWordmark"
                    text: "CEILING"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Rectangle {
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.hairline
                    width: wordmark.implicitWidth - panel.theme.wordmarkRuleTrim
                    height: panel.theme.strokeBold
                    radius: panel.theme.strokeBold / 2
                    color: panel.theme.accent
                }
                Text {
                    id: latency
                    objectName: "ceilingLatency"
                    x: panel.theme.spaceXxs
                    y: wordmark.height + panel.theme.subtitleGap
                    text: "TRUE-PEAK LIMITER · " + Format.number(panel.latencyMs, 1) + " ms LATENCY"
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                    // Kept clear of the preset browser: a wider fallback font
                    // steps the size down rather than running under it.
                    width: panel.theme.headerBrowserX - x - panel.theme.headerGap
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: panel.theme.typeFitFloor
                }
            }
        }
    }

    // ---- Row A: the reduction, the meters and the loudness ------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Kit.Card {
        id: reductionPlate
        theme: panel.theme
        title: "GAIN REDUCTION"
        note: panel.playing ? "HELD 1 s · LAST 10 s" : "NO SIGNAL"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowAY
        width: panel.width - 2 * panel.margin - 2 * panel.gap - panel.theme.meterPlateWidth
            - panel.theme.loudnessPlateWidth
        height: panel.rowAHeight
        CeilingHistory {
            x: panel.theme.spaceMd
            y: panel.theme.historyY
            width: reductionPlate.width - 2 * panel.theme.spaceMd - panel.theme.reductionColumnWidth
            height: reductionPlate.height - panel.theme.historyBottomTrim
            theme: panel.theme
            samples: panel.samples
            seconds: panel.historySeconds
            live: panel.playing
        }
        Column {
            x: reductionPlate.width - panel.theme.reductionColumnWidth
            y: panel.theme.reductionColumnY
            width: panel.theme.reductionColumnInner
            spacing: panel.theme.spaceXs
            Caption {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "NOW"
            }
            Text {
                objectName: "ceilingReduction"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: panel.playing ? panel.reductionText(panel.reductionDb) : "—"
                color: !panel.playing ? panel.theme.inkFaint
                    : panel.reductionDb < panel.theme.reductionIdleDb ? panel.theme.inkDim : panel.theme.reduction
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.reductionNowSize
                font.weight: Font.Light
            }
            Caption {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "dB"
            }
            Item {
                width: 1
                height: panel.theme.reductionSpacer
            }
            Caption {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "LARGEST"
            }
            Text {
                objectName: "ceilingReductionLargest"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: panel.samples.length > 0 ? panel.reductionText(panel.largestDb) : "—"
                color: panel.samples.length > 0 ? panel.theme.ink : panel.theme.inkFaint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.reductionLargestSize
                font.weight: Font.Light
            }
            Item {
                width: 1
                height: panel.theme.clearSpacer
            }
            Kit.Button {
                objectName: "ceilingClearHistory"
                anchors.horizontalCenter: parent.horizontalCenter
                width: panel.theme.clearWidth
                theme: panel.theme
                label: "Clear"
                tip: "Clear the reduction history"
                active: panel.samples.length > 0
                onClicked: panel.clearHistory()
            }
        }
    }

    Kit.Card {
        id: meterPlate
        theme: panel.theme
        title: "LEVELS"
        note: panel.playing ? "PEAK · TRUE PEAK" : "NO SIGNAL"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: reductionPlate.x + reductionPlate.width + panel.gap
        y: panel.rowAY
        width: panel.theme.meterPlateWidth
        height: panel.rowAHeight

        readonly property real meterHeight: panel.theme.meterHeight

        component Pair: Item {
            id: pair
            required property string title
            required property var peak
            required property string prefix
            property bool showCeiling: false
            width: panel.theme.meterPairWidth
            height: meterPlate.meterHeight + panel.theme.meterPairChrome
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
                        color: panel.playing && pair.peak[index] > panel.overDb ? panel.theme.meterOver
                            : panel.playing ? panel.theme.ink : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                }
            }
            Row {
                id: bars
                anchors.horizontalCenter: parent.horizontalCenter
                y: panel.theme.meterBarsY
                spacing: panel.theme.spaceMd
                Repeater {
                    model: 2
                    delegate: Kit.PeakMeter {
                        required property int index
                        objectName: pair.prefix + "Meter" + (index === 0 ? "L" : "R")
                        theme: panel.theme
                        width: panel.theme.meterBarWidth
                        height: meterPlate.meterHeight
                        live: panel.playing
                        floorDb: panel.floorDb
                        ceilDb: panel.ceilDb
                        peakDb: pair.peak[index]
                    }
                }
            }
            // The ceiling, across the bars.
            Rectangle {
                objectName: pair.prefix + "CeilingLine"
                visible: pair.showCeiling
                x: bars.x - panel.theme.spaceXs
                y: bars.y + panel.theme.meterInset
                    + (meterPlate.meterHeight - 2 * panel.theme.meterInset) * (1 - panel.meterAt(panel.ceiling))
                    - panel.theme.strokeBold / 2
                width: bars.width + 2 * panel.theme.spaceXs
                height: panel.theme.strokeBold
                color: panel.theme.ceilingLine
            }
        }

        Pair {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            title: "IN"
            prefix: "ceilingIn"
            peak: panel.inPeak
        }
        Pair {
            x: meterPlate.width - panel.theme.cardInset - width
            y: panel.theme.cardContentY
            title: "OUT"
            prefix: "ceilingOut"
            peak: panel.outPeak
            showCeiling: true
        }
        Note {
            x: panel.theme.cardInset
            y: meterPlate.height - panel.theme.noteBottom
            width: meterPlate.width - 2 * panel.theme.cardInset
            horizontalAlignment: Text.AlignHCenter
            text: "OUT reads true peak; the line is the ceiling."
        }
    }

    Kit.Card {
        id: loudnessPlate
        theme: panel.theme
        title: "LOUDNESS"
        note: "OUTPUT · LUFS"
        x: meterPlate.x + meterPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        Caption {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            text: "INTEGRATED"
        }
        Text {
            id: integratedValue
            objectName: "ceilingIntegrated"
            x: panel.theme.cardInset
            y: panel.theme.integratedY
            text: panel.loudnessText(panel.integrated)
            color: panel.playing && panel.measured ? panel.theme.accent : panel.theme.inkFaint
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.integratedSize
            font.weight: Font.Light
        }
        Text {
            objectName: "ceilingToTarget"
            anchors.left: integratedValue.right
            anchors.leftMargin: panel.theme.spaceMd
            anchors.baseline: integratedValue.baseline
            text: panel.playing && panel.measured
                ? Format.signed_number(panel.integrated - panel.target, panel.levelDecimals) + " LU to target"
                : ""
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeLabel
        }
        Column {
            x: panel.theme.cardInset
            y: panel.theme.loudnessRowsY
            spacing: panel.theme.spaceSm
            Repeater {
                model: [
                    { name: "SHORT-TERM", key: "ceilingShortTerm" },
                    { name: "MOMENTARY", key: "ceilingMomentary" }
                ]
                delegate: Row {
                    id: reading
                    required property var modelData
                    spacing: panel.theme.spaceMd
                    Caption {
                        width: panel.theme.loudnessCaptionWidth
                        text: reading.modelData.name
                        anchors.baseline: value.baseline
                    }
                    Text {
                        id: value
                        objectName: reading.modelData.key
                        text: panel.loudnessText(reading.modelData.key === "ceilingShortTerm"
                                                 ? panel.shortTerm : panel.momentary)
                        color: panel.playing ? panel.theme.ink : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeHeading
                    }
                }
            }
        }
        HintedKnob {
            x: panel.theme.cardInset
            y: panel.theme.targetKnobY
            parameterId: "ceiling.target"
            hint: "what Learn aims at"
            diameter: panel.theme.knobDiameterSmall
        }
        Kit.Button {
            objectName: "ceilingLearn"
            x: loudnessPlate.width - panel.theme.cardInset - width
            y: panel.theme.learnY
            width: panel.theme.learnWidth
            theme: panel.theme
            label: "Learn gain"
            tip: "Move Gain by the distance from the integrated loudness to the target"
            prime: panel.playing && panel.measured
            active: panel.controller.editable && panel.playing && panel.measured
            onClicked: panel.learn()
        }
        Note {
            x: loudnessPlate.width - panel.theme.cardInset - panel.theme.learnWidth
            y: panel.theme.learnNoteY
            width: panel.theme.learnWidth
            text: "Play the loudest part, then Learn; once more to close the rest."
        }
    }

    // ---- Row B: drive, limit, clip, stereo ------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: panel.height - rowBY - margin

    Kit.Card {
        id: drivePlate
        theme: panel.theme
        title: "DRIVE"
        note: "INTO THE CEILING"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.drivePlateWidth
        height: panel.rowBHeight
        Kit.Knob {
            x: panel.theme.heroKnobX
            y: panel.theme.cardContentY
            controller: panel.controller
            theme: panel.theme
            parameterId: "ceiling.gain"
            diameter: panel.theme.heroKnobDiameter
            label: ""
            valueText: panel.signed(panel.gain) + " dB"
        }
        Text {
            objectName: "gainValue"
            x: panel.theme.heroKnobX
            y: panel.theme.heroValueY
            width: panel.theme.heroKnobDiameter
            horizontalAlignment: Text.AlignHCenter
            text: panel.signed(panel.gain)
            color: panel.theme.accent
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.heroValueSize
            font.weight: Font.Light
        }
        Caption {
            x: panel.theme.heroKnobX
            y: panel.theme.heroCaptionY
            width: panel.theme.heroKnobDiameter
            horizontalAlignment: Text.AlignHCenter
            text: "GAIN · dB"
        }
        Kit.Knob {
            x: drivePlate.width - panel.theme.heroKnobX - panel.theme.heroKnobDiameter
            y: panel.theme.cardContentY
            controller: panel.controller
            theme: panel.theme
            parameterId: "ceiling.ceiling"
            diameter: panel.theme.heroKnobDiameter
            label: ""
            valueText: panel.signed(panel.ceiling) + " dBTP"
        }
        Text {
            objectName: "ceilingValue"
            x: drivePlate.width - panel.theme.heroKnobX - panel.theme.heroKnobDiameter
            y: panel.theme.heroValueY
            width: panel.theme.heroKnobDiameter
            horizontalAlignment: Text.AlignHCenter
            text: panel.signed(panel.ceiling)
            color: panel.theme.accent
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.heroValueSize
            font.weight: Font.Light
        }
        Caption {
            x: drivePlate.width - panel.theme.heroKnobX - panel.theme.heroKnobDiameter
            y: panel.theme.heroCaptionY
            width: panel.theme.heroKnobDiameter
            horizontalAlignment: Text.AlignHCenter
            text: "CEILING · dBTP"
        }
    }

    // A card of hinted knobs in a row.
    component KnobCard: Kit.Card {
        id: knobCard
        required property var knobs
        theme: panel.theme
        y: panel.rowBY
        height: panel.rowBHeight
        Row {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            Repeater {
                model: knobCard.knobs
                delegate: HintedKnob {
                    required property var modelData
                    width: (knobCard.width - 2 * panel.theme.cardInset) / knobCard.knobs.length
                    parameterId: modelData.id
                    hint: modelData.hint
                }
            }
        }
    }

    KnobCard {
        id: limitPlate
        title: "LIMIT"
        note: "LOOKAHEAD IS THE LATENCY"
        x: drivePlate.x + drivePlate.width + panel.gap
        width: panel.theme.limitPlateWidth
        knobs: [
            { id: "ceiling.lookahead", hint: "smooth ← → fast" },
            { id: "ceiling.release", hint: "to the loudness stage" },
            { id: "ceiling.character", hint: "fast ← → steady" }
        ]
    }

    KnobCard {
        id: clipPlate
        title: "CLIP"
        note: "OVERSAMPLED 8×"
        x: limitPlate.x + limitPlate.width + panel.gap
        width: panel.theme.clipPlateWidth
        knobs: [
            { id: "ceiling.transients", hint: "onsets through" },
            { id: "ceiling.clip", hint: "every peak" },
            { id: "ceiling.shape", hint: "hard ← → soft" }
        ]
        Row {
            x: panel.theme.cardInset
            y: clipPlate.height - panel.theme.noteBottom
            spacing: panel.theme.spaceXl
            Repeater {
                model: [
                    { name: "CLIPPED", key: "ceilingClipped" },
                    { name: "GUARD", key: "ceilingGuarded" }
                ]
                delegate: Row {
                    id: taken
                    required property var modelData
                    spacing: panel.theme.spaceSm
                    Caption {
                        text: taken.modelData.name
                        anchors.baseline: amount.baseline
                    }
                    Text {
                        id: amount
                        objectName: taken.modelData.key
                        text: panel.playing ? panel.reductionText(taken.modelData.key === "ceilingClipped"
                                                                  ? panel.clippedDb : panel.guardedDb) + " dB" : "—"
                        color: panel.playing ? panel.theme.reduction : panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                }
            }
        }
    }

    KnobCard {
        id: stereoPlate
        title: "STEREO"
        note: "LINK"
        x: clipPlate.x + clipPlate.width + panel.gap
        width: panel.width - panel.margin - x
        knobs: [
            { id: "ceiling.link", hint: "sustain" },
            { id: "ceiling.transient_link", hint: "onsets" }
        ]
    }

    Timer {
        objectName: "ceilingTelemetryPoll"
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
