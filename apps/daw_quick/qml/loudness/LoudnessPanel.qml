pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Loudness device panel: a broadcast-grade EBU R128 / BS.1770-4 meter
// in graphite and teal. The integrated loudness leads, large enough to read
// across a room, with its distance to the target in traffic-light colour;
// beside it short-term and momentary (with their maxima), the loudness
// range, true peak against the ceiling, PSR and PLR; then the bar meter on
// the EBU scale; under them the loudness history with the target and its
// tolerance band; below, the controls. Every control reads and writes
// through `controller` (NativeSynthDeviceController for Loudness); readings
// come only from the meter's telemetry (layout in loudness.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: panel.idle()
    property LoudnessTheme theme: LoudnessTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.panelGap
    readonly property int revision: controller.revision

    // loudness.hpp: fourteen readings, then 300 momentary and 300 short-term
    // values at 10 Hz, oldest first. -200 is a reading not available yet.
    readonly property int readings: 14
    readonly property int historyPoints: 300
    readonly property int frameSize: readings + 2 * historyPoints
    readonly property real silent: -199

    // The history kept by the panel, beyond the 30 s the meter carries:
    // up to ten minutes at 10 Hz.
    readonly property int keptPoints: 6000
    property var shortTermHistory: []
    property var momentaryHistory: []
    property real lastDuration: -1
    property real carry: 0
    property real polledAt: 0
    readonly property int historyLength: shortTermHistory.length

    readonly property bool playing: live.playing === true
    readonly property real target: revision >= 0 ? controller.value("loudness.target") : -14
    readonly property real ceiling: revision >= 0 ? controller.value("loudness.ceiling") : -1
    readonly property real scaleTop: revision >= 0 && controller.choice("loudness.scale") === 1 ? 18 : 9
    readonly property bool integrating: revision >= 0 && controller.choice("loudness.integrate") === 1
    // On target within this many LU; close within nearLu.
    readonly property real onTargetLu: 1
    readonly property real nearLu: 2
    // Telemetry polls: the meter's 10 Hz while playing, slower while idle.
    readonly property int livePoll: 100
    readonly property int idlePoll: 250

    objectName: "loudnessPanel"
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
    Accessible.name: "Loudness meter"

    function idle() {
        return { playing: false, momentary: -200, shortTerm: -200, integrated: -200, range: 0,
                 momentaryMax: -200, shortTermMax: -200, truePeak: -200, truePeakLeft: -200,
                 truePeakRight: -200, truePeakRecent: -200, samplePeak: -200, psr: 0, plr: 0,
                 duration: 0, momentaryHistory: [], shortTermHistory: [] }
    }
    function decode(state) {
        const f = state.frame
        if (f === undefined || f.length < frameSize)
            return idle()
        return {
            playing: true,
            momentary: f[0], shortTerm: f[1], integrated: f[2], range: f[3],
            momentaryMax: f[4], shortTermMax: f[5],
            truePeak: f[6], truePeakLeft: f[7], truePeakRight: f[8], truePeakRecent: f[9],
            samplePeak: f[10], psr: f[11], plr: f[12], duration: f[13],
            momentaryHistory: f.slice(readings, readings + historyPoints),
            shortTermHistory: f.slice(readings + historyPoints, readings + 2 * historyPoints)
        }
    }
    // Append what is new in the meter's history. While integrating, the
    // meter's duration advances 0.1 s per value, so its change counts them
    // exactly; paused, the time since the last poll does.
    function ingest(state, seconds) {
        if (!state.playing)
            return
        let count = 0
        if (lastDuration < 0) {
            // First frame: take the meter's whole history, less the silence
            // before it had any reading.
            let first = 0
            while (first < historyPoints && state.shortTermHistory[first] <= silent
                   && state.momentaryHistory[first] <= silent)
                ++first
            count = historyPoints - first
            carry = 0
        } else if (state.duration > lastDuration + 0.05) {
            count = Math.round((state.duration - lastDuration) * 10)
            carry = 0
        } else if (state.duration < lastDuration - 0.05) {
            // Integrate was switched: start the panel's history afresh too.
            shortTermHistory = []
            momentaryHistory = []
            count = Math.round(state.duration * 10)
            carry = 0
        } else {
            carry += Math.max(0, seconds) * 10
            count = Math.floor(carry)
            carry -= count
        }
        lastDuration = state.duration
        count = Math.min(historyPoints, count)
        if (count <= 0)
            return
        const st = shortTermHistory.concat(state.shortTermHistory.slice(historyPoints - count))
        const m = momentaryHistory.concat(state.momentaryHistory.slice(historyPoints - count))
        shortTermHistory = st.length > keptPoints ? st.slice(st.length - keptPoints) : st
        momentaryHistory = m.length > keptPoints ? m.slice(m.length - keptPoints) : m
    }
    function clearHistory() {
        shortTermHistory = []
        momentaryHistory = []
        lastDuration = -1
        carry = 0
    }
    function poll() {
        const now = Date.now() / 1000
        const seconds = polledAt > 0 ? now - polledAt : 0
        polledAt = now
        live = decode(controller.telemetry())
        ingest(live, seconds)
    }

    // ---- Formatting ------------------------------------------------------------------

    // Readings hold one decimal, so the figures do not jump in width.
    readonly property int readingDecimals: 1
    function lufsText(value) {
        return value <= silent ? "—" : Format.number(value, readingDecimals)
    }
    // True peak can pass 0 dBTP, so it carries its sign either way.
    function peakText(value) {
        return value <= silent ? "—" : Format.signed_number(value, readingDecimals)
    }
    function luText(value, available) {
        return available ? Format.number(Math.abs(value), readingDecimals) : "—"
    }
    readonly property bool hasIntegrated: live.integrated > silent
    readonly property real delta: hasIntegrated ? live.integrated - target : 0
    // On target within 1 LU, close within 2, otherwise off.
    readonly property color deltaTone: !hasIntegrated ? theme.inkFaint
        : Math.abs(delta) <= onTargetLu ? theme.onTarget : Math.abs(delta) <= nearLu ? theme.near : theme.off
    readonly property bool peakOver: live.truePeak > silent && live.truePeak > ceiling
    function durationText(seconds) {
        const s = Math.floor(seconds)
        const h = Math.floor(s / 3600)
        const m = Math.floor((s % 3600) / 60)
        const r = s % 60
        const two = v => (v < 10 ? "0" : "") + v
        return h > 0 ? h + ":" + two(m) + ":" + two(r) : m + ":" + two(r)
    }

    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        if (info.name === undefined)
            return ""
        return (info.group === info.name ? info.name : info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || revision < 0 ? "" : controller.display(id)
    }

    // ---- Header ----------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerTop
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: panel.theme.spaceXxs
            text: "LOUDNESS"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Rectangle {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.underlineGap
            width: wordmark.implicitWidth - panel.theme.underlineTrim
            height: panel.theme.underlineWidth
            radius: panel.theme.underlineWidth / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.accentSoft }
                GradientStop { position: 1; color: panel.theme.accent }
            }
        }
        Text {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.taglineGap
            text: "EBU R128 · ITU-R BS.1770-4"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }

        Row {
            id: browser
            x: panel.theme.browserX
            y: panel.theme.browserY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "loudnessPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "loudnessPresetDisplay"
                width: panel.theme.presetWidth
                height: panel.theme.presetHeight
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.focusStroke : panel.theme.hairline
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Delivery target"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", edited" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(display, panel.controller, panel.theme.accent)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                Text {
                    x: panel.theme.presetInset
                    y: panel.theme.presetCaptionY
                    text: "DELIVERY · " + panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.presetCaptionSpacing
                }
                Text {
                    objectName: "loudnessPresetName"
                    x: panel.theme.presetInset
                    y: panel.theme.presetNameY
                    width: panel.theme.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeHeading
                    font.letterSpacing: panel.theme.presetNameSpacing
                    elide: Text.ElideRight
                }
                Rectangle {
                    x: panel.theme.presetDividerX
                    y: panel.theme.presetDividerInset
                    width: panel.theme.hairline
                    height: parent.height - 2 * panel.theme.presetDividerInset
                    color: panel.theme.wellEdge
                }
                Text {
                    objectName: "loudnessReadout"
                    x: panel.theme.readoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "loudnessReadoutValue"
                    x: panel.theme.readoutX
                    y: panel.theme.readoutValueY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSubtitle
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.chevronMargin
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
                controller: panel.controller
                theme: panel.theme
                objectName: "loudnessPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "loudnessStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusTop
        }
    }

    // ---- Readings --------------------------------------------------------------------

    readonly property real rowAY: theme.rowATop
    readonly property real rowAHeight: theme.rowAHeight

    Kit.Card {
        theme: panel.theme
        id: integratedPlate
        title: "INTEGRATED"
        note: panel.playing ? (panel.integrating ? "MEASURING" : "PAUSED") : "NO SIGNAL"
        noteColor: panel.playing && panel.integrating ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.integratedWidth
        height: panel.rowAHeight
        // The status lamp, top right.
        Rectangle {
            objectName: "loudnessLamp"
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.spaceXl
            y: panel.theme.lampTop
            width: panel.theme.lampSize
            height: panel.theme.lampSize
            radius: panel.theme.lampSize / 2
            color: panel.deltaTone
            Rectangle {
                anchors.centerIn: parent
                width: panel.theme.lampHaloSize
                height: panel.theme.lampHaloSize
                radius: panel.theme.lampHaloSize / 2
                color: Qt.rgba(panel.deltaTone.r, panel.deltaTone.g, panel.deltaTone.b, panel.theme.lampHaloAlpha)
                visible: panel.hasIntegrated
            }
        }
        Text {
            objectName: "loudnessIntegrated"
            x: panel.theme.integratedX
            y: panel.theme.integratedY
            text: panel.lufsText(panel.live.integrated)
            color: panel.hasIntegrated ? panel.theme.glacier : panel.theme.inkFaint
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.integratedSize
            font.weight: Font.Light
        }
        Text {
            x: panel.theme.integratedLabelX
            y: panel.theme.integratedLabelY
            text: "LUFS INTEGRATED"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeSmall
            font.bold: true
            font.letterSpacing: panel.theme.integratedLabelSpacing
        }
        Text {
            objectName: "loudnessTargetDelta"
            x: panel.theme.deltaX
            y: panel.theme.deltaY
            text: panel.hasIntegrated ? Format.signed_number(panel.delta, panel.readingDecimals) + " LU TO TARGET" : "—"
            color: panel.deltaTone
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.deltaSize
        }
        Rectangle {
            x: panel.theme.integratedX
            y: panel.theme.integratedRuleY
            width: integratedPlate.width - 2 * panel.theme.integratedX
            height: panel.theme.hairline
            color: panel.theme.seam
        }
        Row {
            x: panel.theme.factsX
            y: panel.theme.factsY
            spacing: panel.theme.factsSpacing
            component Fact: Column {
                id: fact
                required property string name
                required property string value
                property string objectId: ""
                property color tone: panel.theme.ink
                spacing: panel.theme.factGap
                Text {
                    text: fact.name
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.factCaptionSpacing
                }
                Text {
                    objectName: fact.objectId
                    text: fact.value
                    color: fact.tone
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.factSize
                }
            }
            Fact {
                name: "TARGET"
                value: panel.lufsText(panel.target) + " LUFS"
                tone: panel.theme.accent
            }
            Fact {
                name: "CEILING"
                value: panel.lufsText(panel.ceiling) + " dBTP"
            }
            Fact {
                name: "MEASURED"
                value: panel.durationText(panel.live.duration)
                objectId: "loudnessDuration"
            }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: nowPlate
        title: "NOW"
        x: integratedPlate.x + integratedPlate.width + panel.gap
        y: panel.rowAY
        width: panel.theme.nowWidth
        height: panel.rowAHeight
        Grid {
            x: panel.theme.spaceLg
            y: panel.theme.cardBodyY
            columns: 2
            columnSpacing: panel.theme.spaceMd
            rowSpacing: panel.theme.spaceMd
            component Tile: Rectangle {
                id: tile
                required property string name
                required property string value
                required property string unit
                property string sub: ""
                property string objectId: ""
                property color tone: panel.theme.ink
                property bool alarm: false
                // Three rows of two tiles fill the plate under its title.
                width: (nowPlate.width - 2 * panel.theme.spaceLg - panel.theme.spaceMd) / 2
                height: (nowPlate.height - panel.theme.cardBodyY - panel.theme.spaceLg - 2 * panel.theme.spaceMd) / 3
                radius: panel.theme.tileRadius
                color: panel.theme.well
                border.color: tile.alarm ? panel.theme.off : panel.theme.wellEdge
                border.width: tile.alarm ? panel.theme.focusStroke : panel.theme.hairline
                Text {
                    x: panel.theme.spaceLg
                    y: panel.theme.tileCaptionY
                    text: tile.name
                    color: tile.alarm ? panel.theme.off : panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.tileCaptionSpacing
                }
                Text {
                    objectName: tile.objectId
                    x: panel.theme.spaceLg
                    y: panel.theme.tileValueY
                    text: tile.value
                    color: tile.tone
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.tileValueSize
                    font.weight: Font.Light
                }
                Text {
                    anchors.baseline: parent.top
                    anchors.baselineOffset: panel.theme.tileUnitBaseline
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.spaceLg
                    text: tile.unit
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.typeSmall
                    font.bold: true
                    font.letterSpacing: panel.theme.tileUnitSpacing
                }
                Text {
                    objectName: tile.objectId === "" ? "" : tile.objectId + "Sub"
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.spaceLg
                    y: panel.theme.tileCaptionY
                    text: tile.sub
                    color: panel.theme.inkFaint
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeCaption
                }
            }
            Tile {
                name: "SHORT-TERM · 3 s"
                value: panel.lufsText(panel.live.shortTerm)
                unit: "LUFS"
                sub: "MAX " + panel.lufsText(panel.live.shortTermMax)
                objectId: "loudnessShortTerm"
                tone: panel.live.shortTerm > panel.silent ? panel.theme.shortTerm : panel.theme.inkFaint
            }
            Tile {
                name: "MOMENTARY · 400 ms"
                value: panel.lufsText(panel.live.momentary)
                unit: "LUFS"
                sub: "MAX " + panel.lufsText(panel.live.momentaryMax)
                objectId: "loudnessMomentary"
                tone: panel.live.momentary > panel.silent ? panel.theme.momentary : panel.theme.inkFaint
            }
            Tile {
                name: "LOUDNESS RANGE"
                value: panel.luText(panel.live.range, panel.live.shortTerm > panel.silent && panel.integrating)
                unit: "LU"
                objectId: "loudnessRange"
            }
            Tile {
                name: panel.peakOver ? "TRUE PEAK · OVER CEILING" : "TRUE PEAK"
                value: panel.peakText(panel.live.truePeak)
                unit: "dBTP"
                sub: "3 s " + panel.peakText(panel.live.truePeakRecent)
                objectId: "loudnessTruePeak"
                tone: panel.peakOver ? panel.theme.off : panel.theme.peak
                alarm: panel.peakOver
            }
            Tile {
                name: "PSR · PEAK TO SHORT-TERM"
                value: panel.luText(panel.live.psr, panel.live.shortTerm > panel.silent)
                unit: "LU"
                objectId: "loudnessPsr"
            }
            Tile {
                name: "PLR · PEAK TO LOUDNESS"
                value: panel.luText(panel.live.plr, panel.hasIntegrated)
                unit: "LU"
                objectId: "loudnessPlr"
            }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: meterPlate
        title: "METER"
        note: panel.scaleTop > 9 ? "EBU +18" : "EBU +9"
        x: nowPlate.x + nowPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        LoudnessMeter {
            x: panel.theme.spaceXl
            y: panel.theme.meterPlateTop
            width: meterPlate.width - panel.theme.spaceXl - panel.theme.spaceMd
            height: meterPlate.height - panel.theme.meterPlateTop - panel.theme.spaceMd
            theme: panel.theme
            momentary: panel.live.momentary
            shortTerm: panel.live.shortTerm
            momentaryMax: panel.live.momentaryMax
            shortTermMax: panel.live.shortTermMax
            truePeakLeft: panel.live.truePeakLeft
            truePeakRight: panel.live.truePeakRight
            target: panel.target
            ceiling: panel.ceiling
            scaleTop: panel.scaleTop
        }
    }

    // ---- History ---------------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: theme.rowBHeight

    Kit.Card {
        theme: panel.theme
        id: historyPlate
        title: "LOUDNESS HISTORY"
        note: "LAST 60 s · SHORT-TERM FILLED, MOMENTARY LINE · TARGET ±1 LU"
        x: panel.margin
        y: panel.rowBY
        width: panel.width - 2 * panel.margin
        height: panel.rowBHeight
        LoudnessHistory {
            x: panel.theme.spaceSm
            y: panel.theme.historyPlateTop
            width: historyPlate.width - 2 * panel.theme.spaceSm
            height: historyPlate.height - panel.theme.historyPlateTop - panel.theme.spaceXs
            theme: panel.theme
            shortTerm: panel.shortTermHistory
            momentary: panel.momentaryHistory
            target: panel.target
            scaleTop: panel.scaleTop
            seconds: 60
        }
    }

    // ---- Controls --------------------------------------------------------------------

    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: panel.height - rowCY - margin

    Kit.Card {
        theme: panel.theme
        id: targetPlate
        title: "TARGET"
        x: panel.margin
        y: panel.rowCY
        width: panel.theme.targetWidth
        height: panel.rowCHeight
        Row {
            x: panel.theme.spaceLg
            y: panel.theme.cardBodyY
            spacing: panel.theme.knobSpacing
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "loudness.target"
                diameter: panel.theme.knobLargeDiameter
                readout: true
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "loudness.ceiling"
                diameter: panel.theme.knobLargeDiameter
                readout: true
            }
        }
    }

    Kit.Card {
        theme: panel.theme
        id: measurePlate
        title: "MEASURE"
        x: targetPlate.x + targetPlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.measureWidth
        height: panel.rowCHeight
        Kit.Toggle {
            x: panel.theme.plateInset
            y: panel.theme.plateControlY
            width: measurePlate.width - 2 * panel.theme.plateInset
            height: panel.theme.toggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "loudness.integrate"
            label: "Integrate"
            lit: panel.theme.onTarget
            onCaption: "MEASURING · OFF RESETS"
            offCaption: "PAUSED AND CLEARED · ON STARTS AFRESH"
        }
        Text {
            x: panel.theme.plateInset
            y: panel.theme.measureNoteY
            width: measurePlate.width - 2 * panel.theme.plateInset
            text: "Integrated loudness, range and maxima build from the moment Integrate is on. A seek keeps them."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Kit.Card {
        theme: panel.theme
        id: scalePlate
        title: "SCALE"
        x: measurePlate.x + measurePlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.scaleWidth
        height: panel.rowCHeight
        Kit.Segment {
            x: panel.theme.plateInset
            y: panel.theme.plateControlY
            width: scalePlate.width - 2 * panel.theme.plateInset
            height: panel.theme.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "loudness.scale"
            labels: ["EBU +9", "EBU +18"]
        }
        Text {
            objectName: "loudnessScaleNote"
            x: panel.theme.plateInset
            y: panel.theme.scaleNoteY
            width: scalePlate.width - 2 * panel.theme.plateInset
            text: panel.scaleTop > 9 ? "−36 to +18 LU around the target: wide, for dynamic material."
                                     : "−18 to +9 LU around the target: the R128 meter view."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    Kit.Card {
        theme: panel.theme
        id: notePlate
        title: "READING THE METER"
        x: scalePlate.x + scalePlate.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        Column {
            x: panel.theme.plateInset
            y: panel.theme.cardBodyY
            width: notePlate.width - 2 * panel.theme.plateInset
            spacing: panel.theme.spaceSm
            component Key: Row {
                id: key
                required property color tone
                required property string text
                spacing: panel.theme.spaceMd
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.theme.keyDotSize
                    height: panel.theme.keyDotSize
                    radius: panel.theme.keyDotSize / 2
                    color: key.tone
                }
                Text {
                    text: key.text
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.typeSmall
                }
            }
            Key {
                tone: panel.theme.onTarget
                text: "Within 1 LU of the target"
            }
            Key {
                tone: panel.theme.near
                text: "Within 2 LU"
            }
            Key {
                tone: panel.theme.off
                text: "More than 2 LU away, or true peak over the ceiling"
            }
        }
    }

    Timer {
        objectName: "loudnessTelemetryPoll"
        interval: panel.playing ? panel.livePoll : panel.idlePoll
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
