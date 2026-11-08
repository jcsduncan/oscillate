pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Edge device panel: a clipper under a fixed 0 dBFS ceiling. On the
// left the transfer curve, input against output, with the ceiling dashed
// and the signal's dot on the curve, and under it how hard each of the last
// sixteen beats clipped. On the right the Input, Shape and Output knobs;
// oversampling with the latency it costs and the light filters; stereo
// link, delta and the level-matched compare; and the meters: the input
// peak, how far it is clipped and the output's true peak. Every control
// reads and writes through `controller` (NativeSynthDeviceController for
// Edge); the readings come only from the telemetry (layout in edge.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property EdgeTheme theme: EdgeTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true
    readonly property real silentDb: -120
    // The meters' bars: -24 dBFS to +6 dBFS for levels, 0 to 12 dB clipped.
    readonly property real barFloorDb: -24
    readonly property real barTopDb: 6
    readonly property real clippedFullDb: 12
    // The history's bars: full height at this much clipping.
    readonly property real historyFullDb: 6
    // The curve's resolution.
    readonly property int curvePoints: 121
    // When the panel cannot read the running latency, it shows it at 48 kHz.
    readonly property real nominalRate: 48000

    readonly property real inPeak: playing ? Math.max(live.input_peak[0], live.input_peak[1]) : silentDb
    readonly property real outPeak: playing ? Math.max(live.output_peak[0], live.output_peak[1]) : silentDb
    readonly property real clippedDb: playing ? live.clipped : 0
    readonly property var history: playing ? live.history : []
    readonly property var curve: revision >= 0
        ? controller.edge_transfer(transfer.inMin, transfer.inMax, curvePoints) : []
    readonly property real latencyMs: playing ? live.latency_ms
        : (revision >= 0 ? controller.edge_latency_ms(nominalRate) : 0)
    readonly property string factorText: revision >= 0
        ? controller.display("edge.oversampling").replace("x", "×") : ""
    readonly property bool matched: revision >= 0 && controller.choice("edge.match") === 1
    readonly property bool comparing: revision >= 0 && controller.choice("edge.compare") === 1

    objectName: "edgePanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    color: theme.caseColor
    border.color: theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Edge clipper"

    function poll() {
        live = controller.telemetry()
    }

    // ---- Formatting ------------------------------------------------------------------

    readonly property int levelDecimals: 1
    function levelText(db, unit) {
        return playing && db > silentDb ? Format.signed_number(db, levelDecimals) + " " + unit : "—"
    }
    function clippedText(db) {
        return playing ? Format.number(-db, levelDecimals) + " dB" : "—"
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
    component Note: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeCaption
        wrapMode: Text.WordWrap
    }

    // ---- Header ----------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Rectangle {
            id: mark
            anchors.verticalCenter: parent.verticalCenter
            width: panel.theme.markSize
            height: width
            radius: width / 2
            color: "transparent"
            border.color: panel.theme.accent
            border.width: panel.theme.markStroke
            // A flat top: the clipped wave.
            Canvas {
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
            anchors.left: mark.right
            anchors.leftMargin: panel.theme.markGap
            anchors.verticalCenter: parent.verticalCenter
            spacing: panel.theme.spaceXxs
            Text {
                text: "EDGE"
                color: panel.theme.glacier
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.wordmarkSize
                font.bold: true
                font.letterSpacing: panel.theme.wordmarkSpacing
            }
            Text {
                objectName: "edgeSubtitle"
                text: "CLIPPER · " + panel.factorText + " OVERSAMPLED"
                color: panel.theme.accent
                font.pixelSize: panel.theme.typeSmall
                font.bold: true
                font.letterSpacing: panel.theme.subtitleSpacing
            }
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: panel.theme.headerSpacing
            Kit.StatusBadge {
                objectName: "edgeStatus"
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
            }
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: panel.theme.presetArrowGap
                Kit.PresetArrow {
                    anchors.verticalCenter: parent.verticalCenter
                    controller: panel.controller
                    theme: panel.theme
                    objectName: "edgePresetPrevious"
                    direction: -1
                }
                Rectangle {
                    id: display
                    objectName: "edgePresetDisplay"
                    width: panel.theme.presetWidth
                    height: panel.theme.presetHeight
                    radius: panel.theme.radiusXl
                    color: panel.theme.button
                    border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                    activeFocusOnTab: true
                    Accessible.role: Accessible.ComboBox
                    Accessible.name: "Preset"
                    Accessible.description: panel.controller.preset_name
                        + (panel.controller.preset_modified ? ", edited" : "")
                    function openPresets() {
                        if (panel.controller.editable)
                            menu.openPresets(display, panel.controller, panel.theme.accent)
                    }
                    Keys.onReturnPressed: openPresets()
                    Keys.onSpacePressed: openPresets()
                    Text {
                        objectName: "edgePresetName"
                        x: panel.theme.presetTextX
                        anchors.verticalCenter: parent.verticalCenter
                        width: chevron.x - x - panel.theme.spaceSm
                        text: panel.controller.preset_name + (panel.controller.preset_modified ? " · edited" : "")
                        color: panel.theme.ink
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeLabel
                        elide: Text.ElideRight
                    }
                    Text {
                        id: chevron
                        anchors.right: parent.right
                        anchors.rightMargin: panel.theme.presetTextX
                        anchors.verticalCenter: parent.verticalCenter
                        text: "▾"
                        color: panel.theme.inkDim
                        font.pixelSize: panel.theme.typeBody
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: panel.controller.editable
                        onClicked: {
                            display.forceActiveFocus()
                            display.openPresets()
                        }
                    }
                }
                Kit.PresetArrow {
                    anchors.verticalCenter: parent.verticalCenter
                    controller: panel.controller
                    theme: panel.theme
                    objectName: "edgePresetNext"
                    direction: 1
                }
            }
            Kit.Button {
                objectName: "edgeCompare"
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.compareWidth
                theme: panel.theme
                label: panel.matched ? "A/B matched" : "A/B"
                tip: panel.matched ? "Hear the input, level-matched to the output"
                                   : "Hear the input as it comes in"
                on: panel.comparing
                active: panel.controller.editable
                onClicked: {
                    panel.theme.lastTouched = "edge.compare"
                    panel.controller.set_choice("edge.compare", panel.comparing ? 0 : 1)
                }
            }
        }
    }

    // ---- Left: the transfer and the clip history ---------------------------------------

    readonly property real bodyY: theme.bodyY
    readonly property real bodyHeight: height - bodyY - margin

    Kit.Card {
        id: transferCard
        theme: panel.theme
        title: "TRANSFER"
        note: "IN → OUT"
        x: panel.margin
        y: panel.bodyY
        width: panel.theme.transferWidth
        height: panel.bodyHeight
        EdgeTransfer {
            id: transfer
            objectName: "edgeTransfer"
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: transferCard.width - 2 * panel.theme.cardInset
            height: panel.theme.plotHeight
            theme: panel.theme
            curve: panel.curve
            live: panel.playing
            dotIn: panel.playing ? panel.live.dot_input : panel.silentDb
            dotOut: panel.playing ? panel.live.dot_output : panel.silentDb
        }
        Row {
            id: historyRow
            objectName: "edgeHistory"
            x: panel.theme.cardInset
            y: panel.theme.historyY
            width: transferCard.width - 2 * panel.theme.cardInset
            height: panel.theme.historyHeight
            spacing: panel.theme.historyGap
            readonly property int beats: panel.theme.historyBeats
            readonly property real barWidth: (width - spacing * (beats - 1)) / beats
            Repeater {
                model: historyRow.beats
                delegate: Item {
                    id: beat
                    required property int index
                    readonly property real db: index < panel.history.length ? panel.history[index] : 0
                    width: historyRow.barWidth
                    height: historyRow.height
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: Math.max(panel.theme.historyMinBar,
                                         panel.fraction(beat.db, 0, panel.historyFullDb) * parent.height)
                        radius: panel.theme.radiusXs
                        color: beat.db > panel.theme.historyLitDb ? panel.theme.accent : panel.theme.historyQuiet
                    }
                }
            }
        }
        Note {
            x: panel.theme.cardInset
            y: historyRow.y + historyRow.height + panel.theme.historyCaptionGap
            width: transferCard.width - 2 * panel.theme.cardInset
            text: "Clipping over the last 4 bars, one bar per beat"
        }
    }

    // ---- Right: knobs, quality and switches, meters --------------------------------------

    readonly property real rightX: transferCard.x + transferCard.width + gap
    readonly property real rightWidth: width - rightX - margin

    Kit.Card {
        id: knobCard
        theme: panel.theme
        title: "CLIP"
        note: "THE CEILING IS FIXED AT 0 dBFS"
        x: panel.rightX
        y: panel.bodyY
        width: panel.rightWidth
        height: panel.theme.knobsHeight
        Row {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            width: knobCard.width - 2 * panel.theme.cardInset
            Repeater {
                model: [
                    { id: "edge.input", hint: "drive into the ceiling" },
                    { id: "edge.shape", hint: "hard ← → soft" },
                    { id: "edge.output", hint: "after the clipper" }
                ]
                delegate: Column {
                    id: knobColumn
                    required property var modelData
                    width: (knobCard.width - 2 * panel.theme.cardInset) / 3
                    spacing: panel.theme.knobHintGap
                    Kit.Knob {
                        anchors.horizontalCenter: parent.horizontalCenter
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: knobColumn.modelData.id
                        diameter: panel.theme.knobDiameterLarge
                        readout: true
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: knobColumn.modelData.hint
                        color: panel.theme.inkFaint
                        font.pixelSize: panel.theme.typeCaption
                    }
                }
            }
        }
    }

    readonly property real controlsY: knobCard.y + knobCard.height + gap

    Kit.Card {
        id: qualityCard
        theme: panel.theme
        title: "OVERSAMPLING"
        x: panel.rightX
        y: panel.controlsY
        width: panel.rightWidth - panel.gap - panel.theme.togglesWidth
        height: panel.theme.controlsRowHeight
        Kit.SegmentedControl {
            x: panel.theme.cardInset
            y: panel.theme.segmentRowY
            width: qualityCard.width - 2 * panel.theme.cardInset
            theme: panel.theme
            controller: panel.controller
            parameterId: "edge.oversampling"
            labels: ["1×", "2×", "4×", "8×", "16×"]
        }
        Text {
            objectName: "edgeLatency"
            x: panel.theme.cardInset
            y: panel.theme.latencyY
            width: lightSwitch.x - x - panel.theme.spaceMd
            text: "Latency " + Format.number(panel.latencyMs, 1) + " ms"
                + (panel.playing ? "" : " at 48 kHz") + " · reported for delay compensation"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
        EdgeSwitch {
            id: lightSwitch
            x: qualityCard.width - panel.theme.cardInset - width
            y: panel.theme.latencyY - panel.theme.spaceXs
            width: panel.theme.compareWidth
            controller: panel.controller
            theme: panel.theme
            parameterId: "edge.filters"
            label: "Light filters"
        }
    }

    Kit.Card {
        id: togglesCard
        theme: panel.theme
        title: "LISTEN"
        x: qualityCard.x + qualityCard.width + panel.gap
        y: panel.controlsY
        width: panel.theme.togglesWidth
        height: panel.theme.controlsRowHeight
        Column {
            x: panel.theme.cardInset
            y: panel.theme.cardContentY - panel.theme.spaceXs
            width: togglesCard.width - 2 * panel.theme.cardInset
            Repeater {
                model: [
                    { id: "edge.link", label: "Stereo link" },
                    { id: "edge.delta", label: "Delta (only the clipped part)" },
                    { id: "edge.match", label: "Level-matched compare" }
                ]
                delegate: EdgeSwitch {
                    required property var modelData
                    width: togglesCard.width - 2 * panel.theme.cardInset
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData.id
                    label: modelData.label
                }
            }
        }
    }

    readonly property real metersY: controlsY + theme.controlsRowHeight + gap
    readonly property real meterWidth: (rightWidth - 2 * gap) / 3

    component Meter: Kit.Card {
        id: meter
        required property string valueName
        required property string value
        required property real level
        property color barColor: panel.theme.inkDim
        property color valueColor: panel.theme.ink
        property int valueSize: panel.theme.meterValueSize
        theme: panel.theme
        y: panel.metersY
        width: panel.meterWidth
        height: panel.height - panel.margin - panel.metersY
        Text {
            objectName: meter.valueName
            x: panel.theme.cardInset
            y: panel.theme.cardContentY
            text: meter.value
            color: meter.valueColor
            font.family: panel.theme.mono
            font.pixelSize: meter.valueSize
            font.bold: meter.valueSize > panel.theme.meterValueSize
        }
        Rectangle {
            x: panel.theme.cardInset
            y: meter.height - panel.theme.cardInset - height
            width: meter.width - 2 * panel.theme.cardInset
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

    Meter {
        title: "IN PEAK"
        x: panel.rightX
        valueName: "edgeInPeak"
        value: panel.levelText(panel.inPeak, "dBFS")
        level: panel.playing ? panel.fraction(panel.inPeak, panel.barFloorDb, panel.barTopDb) : 0
    }
    Meter {
        title: "CLIPPED"
        x: panel.rightX + panel.meterWidth + panel.gap
        valueName: "edgeClipped"
        value: panel.clippedText(panel.clippedDb)
        level: panel.fraction(panel.clippedDb, 0, panel.clippedFullDb)
        barColor: panel.theme.accent
        valueColor: panel.playing ? panel.theme.accent : panel.theme.inkFaint
        valueSize: panel.theme.clippedValueSize
    }
    Meter {
        title: "OUT TRUE PEAK"
        x: panel.rightX + 2 * (panel.meterWidth + panel.gap)
        valueName: "edgeOutPeak"
        value: panel.levelText(panel.outPeak, "dBTP")
        level: panel.playing ? panel.fraction(panel.outPeak, panel.barFloorDb, panel.barTopDb) : 0
        barColor: panel.theme.live
    }

    Timer {
        objectName: "edgeTelemetryPoll"
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
