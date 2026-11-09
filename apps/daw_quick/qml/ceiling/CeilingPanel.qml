pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Ceiling device panel: a mastering brickwall in charcoal and coral.
// The gain reduction leads, as a history over the last ten seconds hanging
// from 0 dB, with the reduction now and its largest beside it; then the
// input and output peak meters with the ceiling drawn across the output.
// Below: the ceiling as the hero dial, the lookahead (the limiter's
// latency) and the release. Every control reads and writes through
// `controller` (NativeSynthDeviceController for Ceiling); the readings
// come only from the telemetry (layout in ceiling.hpp).
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

    readonly property bool playing: live.live === true
    readonly property var inPeak: playing ? live.input_peak : [-200, -200]
    readonly property var outPeak: playing ? live.output_peak : [-200, -200]
    readonly property real reductionDb: playing ? live.reduction : 0
    readonly property real ceiling: revision >= 0 ? controller.value("ceiling.ceiling") : -1
    readonly property real gain: revision >= 0 ? controller.value("ceiling.gain") : 0
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

    // ---- Formatting ------------------------------------------------------------------

    // Level readings hold one decimal, so the figures do not jump in width.
    readonly property int levelDecimals: 1
    function signed(value) {
        return Format.signed_number(value, levelDecimals)
    }
    function dbText(db) {
        return db <= floorDb ? Format.number(-Infinity) : signed(db)
    }
    // The reduction is a cut: shown below zero.
    function reductionText(db) {
        return Format.number(-db, levelDecimals)
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
                    text: "LOOKAHEAD LIMITER · " + (panel.revision >= 0 ? panel.controller.display("ceiling.lookahead") : "")
                        + " LATENCY"
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

    // ---- Row A: the reduction and the meters -------------------------------------------

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
        width: panel.width - 2 * panel.margin - panel.gap - panel.theme.meterPlateWidth
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
                    : panel.reductionDb < 0.05 ? panel.theme.inkDim : panel.theme.reduction
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
            Rectangle {
                objectName: "ceilingClearHistory"
                anchors.horizontalCenter: parent.horizontalCenter
                width: panel.theme.clearWidth
                height: panel.theme.clearHeight
                radius: height / 2
                color: clearArea.pressed ? panel.theme.button : panel.theme.buttonHi
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                activeFocusOnTab: true
                opacity: panel.samples.length > 0 ? 1 : panel.theme.idleOpacity
                Accessible.role: Accessible.Button
                Kit.HoverTip { theme: panel.theme }
                Accessible.name: "Clear the reduction history"
                Keys.onReturnPressed: panel.clearHistory()
                Keys.onSpacePressed: panel.clearHistory()
                Text {
                    anchors.centerIn: parent
                    text: "CLEAR"
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                    font.letterSpacing: panel.theme.clearSpacing
                }
                MouseArea {
                    id: clearArea
                    anchors.fill: parent
                    onClicked: panel.clearHistory()
                }
            }
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
        width: panel.width - panel.margin - x
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
            text: "The line on OUT is the ceiling."
        }
    }

    // ---- Row B: gain, ceiling, lookahead, release ----------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: panel.height - rowBY - margin

    Kit.Card {
        id: gainPlate
        theme: panel.theme
        title: "GAIN"
        note: "DRIVE INTO THE LIMITER"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.gainPlateWidth
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
            x: panel.theme.gainValueX
            y: panel.theme.gainValueY
            text: panel.signed(panel.gain)
            color: panel.theme.accent
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.gainValueSize
            font.weight: Font.Light
            Text {
                anchors.left: parent.right
                anchors.leftMargin: panel.theme.gainUnitGap
                anchors.baseline: parent.baseline
                text: "dB"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeSubtitle
            }
        }
        Note {
            x: panel.theme.heroKnobX
            y: panel.theme.gainNoteY
            width: gainPlate.width - 2 * panel.theme.heroKnobX
            text: "Turn up to make the mix louder: the ceiling catches the peaks."
        }
    }

    Kit.Card {
        id: ceilingPlate
        theme: panel.theme
        title: "CEILING"
        note: "THE OUTPUT NEVER PASSES IT"
        x: gainPlate.x + gainPlate.width + panel.gap
        y: panel.rowBY
        width: panel.theme.ceilingPlateWidth
        height: panel.rowBHeight
        Kit.Knob {
            x: panel.theme.heroKnobX
            y: panel.theme.cardContentY
            controller: panel.controller
            theme: panel.theme
            parameterId: "ceiling.ceiling"
            diameter: panel.theme.heroKnobDiameter
            label: ""
            valueText: panel.signed(panel.ceiling) + " dB"
        }
        Text {
            objectName: "ceilingValue"
            x: panel.theme.ceilingValueX
            y: panel.theme.ceilingValueY
            text: panel.signed(panel.ceiling)
            color: panel.theme.accent
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.ceilingValueSize
            font.weight: Font.Light
            Text {
                anchors.left: parent.right
                anchors.leftMargin: panel.theme.spaceSm
                anchors.baseline: parent.baseline
                text: "dBFS"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeHeading
            }
        }
        Note {
            x: panel.theme.ceilingValueX
            y: panel.theme.ceilingNoteY
            width: ceilingPlate.width - panel.theme.ceilingValueX - panel.theme.cardInset
            text: "Peaks over the ceiling are turned down to it, ahead of time; everything under it passes untouched."
        }
    }

    Kit.Card {
        id: lookaheadPlate
        theme: panel.theme
        title: "LOOKAHEAD"
        note: "NOT AUTOMATABLE"
        x: ceilingPlate.x + ceilingPlate.width + panel.gap
        y: panel.rowBY
        width: panel.theme.lookaheadPlateWidth
        height: panel.rowBHeight
        Kit.Knob {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            controller: panel.controller
            theme: panel.theme
            parameterId: "ceiling.lookahead"
            diameter: panel.theme.smallKnobDiameter
            readout: true
        }
        Note {
            x: panel.theme.sideNoteX
            y: panel.theme.sideNoteY
            width: lookaheadPlate.width - panel.theme.sideNoteX - panel.theme.cardInset
            text: "How far ahead the limiter sees each peak, so it can turn down smoothly. The track is delayed by as much; changing it rebuilds the chain."
        }
    }

    Kit.Card {
        id: releasePlate
        theme: panel.theme
        title: "RELEASE"
        x: lookaheadPlate.x + lookaheadPlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        Kit.Knob {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            controller: panel.controller
            theme: panel.theme
            parameterId: "ceiling.release"
            diameter: panel.theme.smallKnobDiameter
            readout: true
        }
        Note {
            x: panel.theme.sideNoteX
            y: panel.theme.sideNoteY
            width: releasePlate.width - panel.theme.sideNoteX - panel.theme.cardInset
            text: "How fast the gain recovers after a peak. Short is loud and can pump; long is smooth."
        }
    }

    Timer {
        objectName: "ceilingTelemetryPoll"
        interval: panel.playing ? 33 : 250
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
