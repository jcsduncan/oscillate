pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Keel device panel: adaptive tonal balance. On the left the balance in
// third-octave bands, centred so only its shape shows: the tracked balance
// as a ghost, the target, the correction now and the balance Keel lets out;
// with the target on Curve the view edits the drawn curve. Under it the
// correction over the last 24 seconds, then the input and output peaks
// and Level Match's gain. On the right Amount, Speed and the limits; the
// target (Tilt, Curve or Reference), its tilt, Learn and Flatten; the
// regions' weights and splits; and the output: Level Match, Delta and the
// trim. Every control reads and writes through `controller`
// (NativeSynthDeviceController for Keel); the readings come only from the
// telemetry (layout in keel.hpp).
Rectangle {
    id: panel
    required property var controller
    // The device chain sets these for an effect that reads a sidechain.
    property bool keyed: false
    property string keyName: "the sidechain"
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property KeelTheme theme: KeelTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true
    readonly property bool tracked: playing && live.tracked === true
    readonly property real silentDb: -120
    // The meters' bars: -60 dBFS to 0 dBFS for levels, 6 dB either way for
    // Level Match.
    readonly property real barFloorDb: -60
    readonly property real barTopDb: 0
    readonly property real matchFullDb: 6
    // Keel's target grid and history (keel.hpp), from the controller.
    readonly property var layout: controller.keel_layout()
    readonly property var bandHz: layout.band_hz !== undefined ? layout.band_hz : []
    readonly property int bands: bandHz.length
    readonly property real emptyDb: -150
    // The history's span and its time ticks, seconds ago.
    readonly property real historySeconds: layout.history_slices !== undefined
        ? layout.history_slices * layout.history_seconds : 0
    readonly property var secondsTicks: [24, 18, 12, 6]
    readonly property var historyHzTicks: [50, 200, 1000, 5000, 20000]
    readonly property var balanceHzTicks: [20, 100, 1000, 10000]
    readonly property real kilo: 1000
    // Target choices (keel_table::targets).
    readonly property int targetTilt: 0
    readonly property int targetCurve: 1
    readonly property int targetReference: 2

    readonly property int targetSource: revision >= 0 ? controller.choice("keel.target") : targetTilt
    readonly property real tilt: revision >= 0 ? controller.value("keel.tilt") : 0
    readonly property bool matching: revision >= 0 && controller.choice("keel.match") === 1
    readonly property bool delta: revision >= 0 && controller.choice("keel.delta") === 1
    readonly property bool referenced: playing && live.referenced === true

    // The drawn curve's parameters, low band to high.
    readonly property var curveIds: controller.parameter_ids("Curve")
    // The target the values describe, before any reference (dB per band).
    readonly property var valueTarget: revision >= 0 ? controller.keel_target() : []
    // Where the values' target sits on the view: on the target Keel reports
    // while it reads, else centred on its own mean.
    readonly property real valueOffset: {
        let sum = 0
        let count = 0
        if (tracked && live.target !== undefined) {
            for (let b = 0; b < bands; ++b) {
                if (live.target[b] > emptyDb) {
                    sum += live.target[b] - valueTarget[b]
                    ++count
                }
            }
            if (count > 0)
                return sum / count
        }
        for (let b = 0; b < bands; ++b)
            sum -= valueTarget[b]
        return bands > 0 ? sum / bands : 0
    }
    readonly property var shownTarget: tracked && (targetSource !== targetReference || referenced)
        ? live.target : valueTarget.map(db => db + valueOffset)

    readonly property real inPeak: playing ? Math.max(live.input_peak[0], live.input_peak[1]) : silentDb
    readonly property real outPeak: playing ? Math.max(live.output_peak[0], live.output_peak[1]) : silentDb
    readonly property real matchDb: playing ? live.match_db : 0

    objectName: "keelPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    color: theme.caseColor
    border.color: theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Keel tonal balance"

    function poll() {
        live = controller.telemetry()
    }
    // Every curve value to 0 dB, as one edit.
    function flatten() {
        const edit = {}
        for (const id of curveIds)
            edit[id] = 0
        controller.set_values(edit)
    }

    // ---- Formatting ------------------------------------------------------------------

    readonly property int levelDecimals: 1
    function levelText(db) {
        return playing && db > silentDb ? Format.signed_number(db, levelDecimals) + " dBFS" : "—"
    }
    function fraction(value, low, high) {
        return Math.max(0, Math.min(1, (value - low) / (high - low)))
    }
    function hzText(hz) {
        return hz >= kilo ? Format.number(hz / kilo, 0) + " kHz" : Format.number(hz, 0) + " Hz"
    }
    // A tick's label: hertz, or kilohertz as "1k".
    function hzTick(hz) {
        return hz >= kilo ? Format.number(hz / kilo, 0) + "k" : Format.number(hz, 0)
    }
    readonly property string targetText: targetSource === targetCurve ? "CURVE"
        : targetSource === targetReference ? "REFERENCE"
        : "TILT " + Format.signed_number(tilt, levelDecimals) + " dB/OCT"

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
        // The mark (a keel under a waterline in a ring) beside the wordmark
        // over the target.
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
                        readonly property string plotExemptReason: "Keel's logo, not a plot"
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
                        objectName: "keelWordmark"
                        text: "KEEL"
                        color: panel.theme.wordmarkInk
                        font.family: panel.theme.wordmarkFont
                        font.pixelSize: panel.theme.wordmarkSize
                        font.weight: panel.theme.wordmarkWeight
                        font.letterSpacing: panel.theme.wordmarkSpacing
                    }
                    Text {
                        objectName: "keelSubtitle"
                        text: "TONAL BALANCE · " + panel.targetText
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

    // ---- Left: the balance, its history and the meters ----------------------------------

    readonly property real bodyY: theme.bodyY
    readonly property real bodyHeight: height - bodyY - margin

    Kit.Card {
        id: viewCard
        theme: panel.theme
        title: "BALANCE"
        note: !panel.playing ? "IDLE" : !panel.tracked ? "LISTENING"
            : panel.targetSource === panel.targetReference && !panel.referenced ? "NO REFERENCE YET"
            : "LIVE"
        noteColor: panel.tracked ? panel.theme.accent : panel.theme.inkFaint
        x: panel.margin
        y: panel.bodyY
        width: panel.theme.viewWidth
        height: panel.bodyHeight
        Kit.PlotFrame {
            id: balanceFrame
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: viewCard.width - 2 * panel.theme.cardInset
            height: panel.theme.balanceHeight
            theme: panel.theme
            live: panel.tracked
            xUnit: "Hz"
            yUnit: "dB"
            xTicks: panel.balanceHzTicks.map(hz => ({ at: view.hzAt(hz), label: panel.hzTick(hz) }))
            yTicks: view.dbTicks.slice().reverse().map(db => ({ at: view.dbAt(db),
                                                                label: Format.signed_number(db, 0) }))
            KeelView {
                id: view
                objectName: "keelView"
                anchors.fill: parent
                theme: panel.theme
                controller: panel.controller
                live: panel.tracked
                balance: panel.tracked ? panel.live.balance : []
                target: panel.shownTarget
                correction: panel.tracked ? panel.live.correction : []
                curveIds: panel.curveIds
                bandHz: panel.bandHz
                referenceBand: panel.layout.reference_band !== undefined ? panel.layout.reference_band : 0
                handles: panel.valueTarget.map(db => db + panel.valueOffset)
                editable: panel.targetSource === panel.targetCurve && panel.controller.editable
            }
        }
        Caption {
            id: historyCaption
            x: panel.theme.cardInset
            y: balanceFrame.y + balanceFrame.height + panel.theme.spaceLg
            text: "CORRECTION OVER TIME"
        }
        Kit.PlotFrame {
            id: historyFrame
            x: panel.theme.cardInset
            y: historyCaption.y + historyCaption.height + panel.theme.spaceXs
            width: viewCard.width - 2 * panel.theme.cardInset
            height: panel.theme.historyHeight
            theme: panel.theme
            live: panel.tracked
            xUnit: "s"
            yUnit: "Hz"
            xTicks: panel.secondsTicks.map(s => ({ at: 1 - s / panel.historySeconds,
                                                   label: Format.signed_number(-s, 0) }))
                .concat([{ at: 1, label: "now" }])
            yTicks: panel.historyHzTicks.map(hz => ({ at: view.hzAt(hz), label: panel.hzTick(hz) }))
            KeelHistory {
                objectName: "keelHistory"
                anchors.fill: parent
                theme: panel.theme
                slices: panel.tracked ? panel.live.history : []
            }
        }

        readonly property real metersY: historyFrame.y + historyFrame.height + panel.theme.metersGap
        readonly property real meterWidth: (viewCard.width - 2 * panel.theme.cardInset
                                            - 2 * panel.theme.metersGap) / 3

        Meter {
            x: panel.theme.cardInset
            title: "IN PEAK"
            valueName: "keelInPeak"
            value: panel.levelText(panel.inPeak)
            level: panel.playing ? panel.fraction(panel.inPeak, panel.barFloorDb, panel.barTopDb) : 0
        }
        Meter {
            x: panel.theme.cardInset + viewCard.meterWidth + panel.theme.metersGap
            title: "OUT PEAK"
            valueName: "keelOutPeak"
            value: panel.levelText(panel.outPeak)
            level: panel.playing ? panel.fraction(panel.outPeak, panel.barFloorDb, panel.barTopDb) : 0
            barColor: panel.theme.output
        }
        Meter {
            x: panel.theme.cardInset + 2 * (viewCard.meterWidth + panel.theme.metersGap)
            title: "LEVEL MATCH"
            valueName: "keelMatch"
            value: !panel.matching ? "OFF" : panel.playing
                ? Format.signed_number(panel.matchDb, panel.levelDecimals) + " dB" : "—"
            level: panel.matching ? panel.fraction(Math.abs(panel.matchDb), 0, panel.matchFullDb) : 0
            barColor: panel.theme.accent
            valueColor: panel.matching ? panel.theme.ink : panel.theme.inkFaint
        }
    }

    // ---- Right: balance, target, regions and output ---------------------------------------

    readonly property real rightX: viewCard.x + viewCard.width + gap
    readonly property real rightWidth: width - rightX - margin

    component KnobRow: Row {
        id: row
        required property var ids
        property int diameter: panel.theme.knobDiameterMedium
        x: panel.theme.cardInset
        y: panel.theme.cardContentY
        Repeater {
            model: row.ids
            delegate: Item {
                id: slot
                required property string modelData
                width: row.width / row.ids.length
                height: knob.height
                Kit.Knob {
                    id: knob
                    anchors.horizontalCenter: parent.horizontalCenter
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: slot.modelData
                    diameter: row.diameter
                    readout: true
                }
            }
        }
    }

    Kit.Card {
        id: balanceCard
        theme: panel.theme
        title: "STEER"
        note: "OVER " + Format.number(panel.revision >= 0 ? panel.controller.value("keel.speed") : 0, panel.levelDecimals) + " s"
        x: panel.rightX
        y: panel.bodyY
        width: panel.rightWidth
        height: panel.theme.balanceCardHeight
        KnobRow {
            width: balanceCard.width - 2 * panel.theme.cardInset
            ids: ["keel.amount", "keel.speed", "keel.max_boost", "keel.max_cut"]
            diameter: panel.theme.knobDiameterLarge
        }
    }

    Kit.Card {
        id: targetCard
        theme: panel.theme
        title: "TARGET"
        note: panel.targetSource === panel.targetCurve ? "DRAG THE CURVE · LEARN TAKES THE MIX"
            : panel.targetSource === panel.targetReference
              ? (panel.referenced ? "FROM " + panel.keyName.toUpperCase() : "ROUTE A REFERENCE TO THE SIDECHAIN")
            : "A TILT ABOUT 1 kHz"
        noteColor: panel.targetSource === panel.targetReference && !panel.referenced
            ? panel.theme.warning : panel.theme.inkFaint
        x: panel.rightX
        y: balanceCard.y + balanceCard.height + panel.gap
        width: panel.rightWidth
        height: panel.theme.targetCardHeight
        Kit.SegmentedControl {
            id: targetChoice
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: panel.theme.targetChoiceWidth
            theme: panel.theme
            controller: panel.controller
            parameterId: "keel.target"
            labels: ["Tilt", "Curve", "Reference"]
        }
        Row {
            x: panel.theme.cardInset
            y: targetChoice.y + targetChoice.height + panel.theme.spaceLg
            spacing: panel.theme.spaceLg
            Kit.Button {
                objectName: "keelLearn"
                theme: panel.theme
                label: "Learn"
                tip: "Take the balance Keel has tracked as the target curve"
                active: panel.tracked
                onClicked: panel.controller.keel_learn()
            }
            Kit.Button {
                objectName: "keelFlatten"
                theme: panel.theme
                label: "Flatten"
                tip: "Set the drawn curve to 0 dB in every band"
                onClicked: panel.flatten()
            }
        }
        Kit.Knob {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.cardInset
            y: panel.theme.cardContentY - panel.theme.spaceXs
            controller: panel.controller
            theme: panel.theme
            parameterId: "keel.tilt"
            diameter: panel.theme.knobDiameterLarge
            readout: true
        }
    }

    Kit.Card {
        id: regionsCard
        theme: panel.theme
        title: "REGIONS"
        note: "UNDER " + panel.hzText(panel.revision >= 0 ? panel.controller.value("keel.low_split") : 0)
              + " · OVER " + panel.hzText(panel.revision >= 0 ? panel.controller.value("keel.high_split") : 0)
        x: panel.rightX
        y: targetCard.y + targetCard.height + panel.gap
        width: panel.rightWidth
        height: panel.theme.regionsCardHeight
        KnobRow {
            width: regionsCard.width - 2 * panel.theme.cardInset
            ids: ["keel.low", "keel.mid", "keel.high", "keel.low_split", "keel.high_split"]
            diameter: panel.theme.knobDiameterSmall
        }
    }

    Kit.Card {
        id: outputCard
        theme: panel.theme
        title: "OUTPUT"
        note: panel.delta ? "DELTA · WHAT KEEL CHANGES" : ""
        noteColor: panel.theme.warning
        x: panel.rightX
        y: regionsCard.y + regionsCard.height + panel.gap
        width: panel.rightWidth
        height: panel.height - panel.margin - y
        Column {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY + panel.theme.spaceSm
            spacing: panel.theme.spaceLg
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "keel.match"
                label: "LEVEL MATCH"
            }
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "keel.delta"
                label: "DELTA"
                accent: panel.theme.warning
            }
        }
        Kit.Knob {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.cardInset
            y: panel.theme.cardContentY - panel.theme.spaceXs
            controller: panel.controller
            theme: panel.theme
            parameterId: "keel.output"
            diameter: panel.theme.knobDiameterLarge
            readout: true
        }
    }

    Timer {
        objectName: "keelTelemetryPoll"
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
