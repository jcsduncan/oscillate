pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Temper device panel (ADR-0064): a resonance suppressor with dynamic
// EQ bands, in a blue-black case with copper cuts and an ice-blue listening
// curve. The header carries the preset, Soft or Hard, the phase (and the
// latency it costs) and Delta. The display leads: spectra, the cut now, the
// bands and the sensitivity curve, with the reduction history under it.
// Below it the selected band's inspector, then the cards: Suppress, Time,
// Split, Stereo, Unmask and Output. Every control reads and writes through
// `controller` (NativeSynthDeviceController for Temper); live readings come
// only from the telemetry (layout in temper.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property TemperTheme theme: TemperTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    // The table's own counts: dynamic bands and sensitivity nodes.
    readonly property int bandCount: countOf("temper.band_on_")
    readonly property int nodeCount: countOf("temper.sens_freq_")
    // Under this a level reads silence.
    readonly property real floorDb: -60

    // The band in the inspector (from 0).
    property int selected: 0
    readonly property bool playing: live.live === true
    readonly property bool keyed: playing && live.keyed === true
    readonly property bool selectedOn: revision >= 0 && controller.choice(bandId(selected, "on")) === 1
    readonly property bool unmaskOn: revision >= 0 && controller.choice("temper.unmask") === 1
    readonly property bool deltaOn: revision >= 0 && controller.choice("temper.delta") === 1

    objectName: "temperPanel"
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
    Accessible.name: "Temper resonance suppressor"

    Component.onCompleted: selected = firstOn()

    function countOf(prefix) {
        let n = 0
        while (controller.parameter(prefix + (n + 1)).name !== undefined)
            ++n
        return n
    }
    function bandId(band, name) {
        return "temper.band_" + name + "_" + (band + 1)
    }
    function isOn(band) {
        return revision >= 0 && controller.choice(bandId(band, "on")) === 1
    }
    function firstOn() {
        for (let b = 0; b < bandCount; ++b) {
            if (isOn(b))
                return b
        }
        return 0
    }
    function select(band) {
        selected = Math.max(0, Math.min(bandCount - 1, band))
    }
    // Remove: the band goes off and back to its defaults, as one undo step.
    function removeBand(band) {
        if (!controller.editable)
            return
        const on = bandId(band, "on")
        controller.begin_gesture(on)
        controller.set_choice(on, 0)
        for (const name of ["kind", "freq", "q", "range", "threshold", "ratio"]) {
            const id = bandId(band, name)
            controller.set_value(id, controller.parameter(id)["default"])
        }
        controller.end_gesture(on)
        theme.lastTouched = on
    }

    // ---- Formatting ------------------------------------------------------------------

    // A level, −∞ at the floor.
    function levelText(db) {
        return Format.signed_quantity(db <= floorDb ? -Infinity : db, "dBFS")
    }
    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        if (info.name === undefined)
            return ""
        return (info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || revision < 0 ? "" : controller.display(id)
    }
    // The loudest the key is anywhere, 0..1 over floorDb..0 dBFS.
    function keyLevel() {
        if (!keyed || !live.sidechain_spectrum)
            return 0
        let top = floorDb
        for (const db of live.sidechain_spectrum)
            top = Math.max(top, Number(db))
        return (top - floorDb) / -floorDb
    }

    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.capsSpacing
    }
    component Note: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeCaption
        wrapMode: Text.WordWrap
    }
    component PanelKnob: Kit.Knob {
        controller: panel.controller
        theme: panel.theme
        readout: true
    }

    // ---- Header ----------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Rectangle {
            id: ember
            y: panel.theme.emberY
            width: panel.theme.emberSize
            height: panel.theme.emberSize
            radius: width / 2
            color: panel.theme.cut
            border.color: panel.theme.cutHot
        }
        Text {
            id: wordmark
            x: ember.width + panel.theme.spaceMd
            y: 0
            text: "TEMPER"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "RESONANCE · DYNAMIC EQ"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.subtitleSpacing
        }

        Row {
            id: browser
            x: panel.theme.browserX
            y: panel.theme.browserY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "temperPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "temperPresetDisplay"
                width: panel.theme.presetWidth
                height: panel.theme.presetHeight
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.focusStroke : panel.theme.hairline
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
                    x: panel.theme.spaceLg
                    y: panel.theme.presetCaptionY
                    text: panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.capsSpacing
                }
                Text {
                    objectName: "temperPresetName"
                    x: panel.theme.spaceLg
                    y: panel.theme.presetNameY
                    width: panel.theme.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeSubtitle
                    elide: Text.ElideRight
                }
                Rectangle {
                    x: panel.theme.presetDividerX
                    y: panel.theme.spaceMd
                    width: panel.theme.hairline
                    height: parent.height - 2 * panel.theme.spaceMd
                    color: panel.theme.wellEdge
                }
                Text {
                    objectName: "temperReadout"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.space2xl
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.capsTightSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "temperReadoutValue"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetValueY
                    width: display.width - x - panel.theme.space2xl
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeEmphasis
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.presetChevronMargin
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
                objectName: "temperPresetNext"
                direction: 1
            }
        }

        Kit.Segment {
            id: modeSegment
            x: browser.x + browser.width + panel.theme.spaceXl
            y: panel.theme.headerControlY
            width: panel.theme.modeWidth
            height: panel.theme.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "temper.mode"
            labels: ["SOFT", "HARD"]
        }
        Kit.Segment {
            id: phaseSegment
            x: modeSegment.x + modeSegment.width + panel.theme.spaceLg
            y: panel.theme.headerControlY
            width: panel.theme.phaseWidth
            height: panel.theme.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "temper.phase"
            labels: ["LOW LATENCY", "NATURAL", "LINEAR"]
        }
        Rectangle {
            id: latencyChip
            x: phaseSegment.x + phaseSegment.width + panel.theme.headerChipGap
            y: panel.theme.headerControlY
            width: panel.theme.chipWidth
            height: panel.theme.chipHeight
            radius: height / 2
            color: "transparent"
            border.color: panel.theme.wellEdge
            Text {
                objectName: "temperLatency"
                anchors.centerIn: parent
                text: panel.playing ? Format.milliseconds(Number(panel.live.latency_ms)) : "— ms"
                color: panel.theme.inkDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeSmall
            }
        }
        Rectangle {
            id: delta
            objectName: "temperDelta"
            x: latencyChip.x + latencyChip.width + panel.theme.headerChipGap
            y: panel.theme.headerControlY
            width: panel.theme.chipWidth
            height: panel.theme.chipHeight
            radius: panel.theme.buttonRadius
            color: panel.deltaOn ? panel.theme.cut : panel.theme.button
            border.color: activeFocus ? panel.theme.listen
                : panel.deltaOn ? panel.theme.cut : panel.theme.plateHi
            activeFocusOnTab: panel.controller.editable
            Accessible.role: Accessible.CheckBox
            Accessible.name: "Delta: hear only what Temper removes"
            Accessible.checked: panel.deltaOn
            function flip() {
                panel.theme.lastTouched = "temper.delta"
                panel.controller.set_choice("temper.delta", panel.deltaOn ? 0 : 1)
            }
            Keys.onSpacePressed: flip()
            Keys.onReturnPressed: flip()
            Text {
                anchors.centerIn: parent
                text: "DELTA"
                color: panel.deltaOn ? panel.theme.screen : panel.theme.inkDim
                font.pixelSize: panel.theme.typeSmall
                font.bold: true
                font.letterSpacing: panel.theme.capsSpacing
            }
            MouseArea {
                anchors.fill: parent
                enabled: panel.controller.editable
                onClicked: delta.flip()
            }
        }

        Kit.StatusBadge {
            objectName: "temperStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- The display -----------------------------------------------------------------

    readonly property real displayY: theme.displayY
    readonly property real displayHeight: theme.displayHeight

    Kit.Card {
        id: displayPlate
        theme: panel.theme
        title: "SPECTRUM"
        note: panel.playing ? "IN · OUT · CUT NOW" : "NO SIGNAL"
        noteColor: panel.playing ? panel.theme.live : panel.theme.inkFaint
        x: panel.margin
        y: panel.displayY
        width: panel.width - 2 * panel.margin
        height: panel.displayHeight
        TemperDisplay {
            x: panel.theme.cardInset
            y: panel.theme.cardBodyY
            width: displayPlate.width - 2 * panel.theme.cardInset
            height: displayPlate.height - panel.theme.cardBodyY - panel.theme.cardInset
            controller: panel.controller
            theme: panel.theme
            bandCount: panel.bandCount
            nodeCount: panel.nodeCount
            selected: panel.selected
            live: panel.live
            onPicked: band => panel.select(band)
        }
    }

    // ---- The band inspector ----------------------------------------------------------

    readonly property real inspectorY: displayY + displayHeight + gap
    readonly property real inspectorHeight: theme.inspectorHeight

    Kit.Card {
        id: inspector
        objectName: "temperInspector"
        theme: panel.theme
        title: "BAND " + (panel.selected + 1)
        note: panel.selectedOn ? "DYNAMIC · MOVES WITH ITS LEVEL" : "OFF · DOUBLE-CLICK THE SCREEN OR SWITCH IT ON"
        noteColor: panel.selectedOn ? panel.theme.bands[panel.selected] : panel.theme.inkFaint
        x: panel.margin
        y: panel.inspectorY
        width: panel.width - 2 * panel.margin
        height: panel.inspectorHeight

        Row {
            id: chips
            objectName: "temperBands"
            x: panel.theme.inspectorX
            y: panel.theme.inspectorRowY
            spacing: panel.theme.spaceXs
            activeFocusOnTab: true
            Accessible.role: Accessible.PageTabList
            Accessible.name: "Bands"
            Keys.onLeftPressed: panel.select(panel.selected - 1)
            Keys.onRightPressed: panel.select(panel.selected + 1)
            Repeater {
                model: panel.bandCount
                delegate: Rectangle {
                    id: chip
                    required property int index
                    readonly property color tone: panel.theme.bands[index]
                    readonly property bool on: panel.isOn(index)
                    readonly property bool chosen: index === panel.selected
                    objectName: "temperBand:" + (index + 1)
                    width: panel.theme.bandChipSize
                    height: panel.theme.bandChipSize
                    radius: width / 2
                    color: on ? tone : panel.theme.button
                    border.color: chosen ? panel.theme.glacier : on ? tone : panel.theme.seam
                    border.width: chosen ? panel.theme.strokeBold : panel.theme.hairline
                    Accessible.role: Accessible.PageTab
                    Accessible.name: "Band " + (index + 1) + (on ? "" : ", off")
                    Accessible.checked: chosen
                    Text {
                        anchors.centerIn: parent
                        text: chip.index + 1
                        color: chip.on ? panel.theme.screen : panel.theme.inkDim
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            chips.forceActiveFocus()
                            panel.select(chip.index)
                        }
                    }
                }
            }
        }
        Caption {
            x: chips.x + chips.width + panel.theme.inspectorGroupGap
            y: panel.theme.inspectorCaptionY
            text: "KIND"
        }
        Kit.Segment {
            id: kindSegment
            x: chips.x + chips.width + panel.theme.inspectorGroupGap
            y: panel.theme.inspectorRowY
            width: panel.theme.kindWidth
            height: panel.theme.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: panel.bandId(panel.selected, "kind")
            accent: panel.theme.bands[panel.selected]
            labels: ["BELL", "LOW SHELF", "HIGH SHELF"]
        }
        Row {
            id: bandKnobs
            x: kindSegment.x + kindSegment.width + panel.theme.spaceXl
            y: panel.theme.inspectorKnobY
            spacing: panel.theme.spaceXs
            Repeater {
                model: ["freq", "q", "range", "threshold", "ratio"]
                delegate: PanelKnob {
                    required property string modelData
                    parameterId: panel.bandId(panel.selected, modelData)
                    accent: panel.theme.bands[panel.selected]
                    diameter: panel.theme.bandKnobDiameter
                    inactive: !panel.selectedOn
                }
            }
        }
        Column {
            x: bandKnobs.x + bandKnobs.width + panel.theme.inspectorGroupGap
            y: panel.theme.inspectorNowY
            spacing: panel.theme.spaceXs
            Caption { text: "NOW" }
            Text {
                objectName: "temperBandNow"
                text: panel.playing && panel.selectedOn && panel.live.band_gain
                    ? Format.signed_decibels(Number(panel.live.band_gain[panel.selected])) : "—"
                color: panel.selectedOn ? panel.theme.bands[panel.selected] : panel.theme.inkFaint
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeHeading
            }
            Text {
                objectName: "temperBandLevel"
                text: panel.playing && panel.selectedOn && panel.live.band_level
                    ? "level " + panel.levelText(Number(panel.live.band_level[panel.selected])) : ""
                color: panel.theme.inkDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeCaption
            }
        }
        Kit.Switch {
            x: inspector.width - panel.theme.inspectorSwitchRight
            y: panel.theme.inspectorSwitchY
            controller: panel.controller
            theme: panel.theme
            parameterId: panel.bandId(panel.selected, "on")
            accent: panel.theme.bands[panel.selected]
            label: panel.selectedOn ? "ON" : "OFF"
        }
        Rectangle {
            id: remove
            objectName: "temperBandRemove"
            x: inspector.width - panel.theme.removeRight
            y: panel.theme.inspectorRowY
            width: panel.theme.removeWidth
            height: panel.theme.removeHeight
            radius: panel.theme.buttonRadius
            color: removeArea.containsMouse ? panel.theme.buttonHi : panel.theme.button
            border.color: activeFocus ? panel.theme.listen : panel.theme.plateHi
            activeFocusOnTab: panel.controller.editable
            Accessible.role: Accessible.Button
            Accessible.name: "Remove band " + (panel.selected + 1)
            Keys.onSpacePressed: panel.removeBand(panel.selected)
            Keys.onReturnPressed: panel.removeBand(panel.selected)
            Text {
                anchors.centerIn: parent
                text: "REMOVE"
                color: panel.theme.inkDim
                font.pixelSize: panel.theme.typeSmall
                font.bold: true
                font.letterSpacing: panel.theme.capsSpacing
            }
            MouseArea {
                id: removeArea
                anchors.fill: parent
                hoverEnabled: true
                enabled: panel.controller.editable
                onClicked: panel.removeBand(panel.selected)
            }
        }
    }

    // ---- The cards -------------------------------------------------------------------

    readonly property real cardsY: inspectorY + inspectorHeight + gap
    readonly property real cardsHeight: panel.height - cardsY - margin
    readonly property real knobsY: theme.cardKnobsY

    Kit.Card {
        id: suppressCard
        theme: panel.theme
        title: "SUPPRESS"
        note: panel.playing ? Format.decibels(-Number(panel.live.max_cut)) : ""
        noteColor: panel.theme.cut
        x: panel.margin
        y: panel.cardsY
        width: panel.theme.suppressWidth
        height: panel.cardsHeight
        Row {
            x: panel.theme.spaceMd
            y: panel.knobsY
            Repeater {
                model: ["temper.depth", "temper.detail", "temper.max_cut", "temper.tilt"]
                delegate: PanelKnob {
                    required property string modelData
                    parameterId: modelData
                    accent: panel.theme.cut
                }
            }
        }
    }
    Kit.Card {
        id: timeCard
        theme: panel.theme
        title: "TIME"
        x: suppressCard.x + suppressCard.width + panel.gap
        y: panel.cardsY
        width: panel.theme.timeWidth
        height: panel.cardsHeight
        Row {
            x: panel.theme.spaceMd
            y: panel.knobsY
            Repeater {
                model: ["temper.attack", "temper.release"]
                delegate: PanelKnob {
                    required property string modelData
                    parameterId: modelData
                    accent: panel.theme.cut
                }
            }
        }
        Note {
            x: panel.theme.spaceLg
            y: timeCard.height - panel.theme.cardNoteBottom
            width: timeCard.width - 2 * panel.theme.spaceLg
            text: "Tilt leans these by register too."
        }
    }
    Kit.Card {
        id: splitCard
        theme: panel.theme
        title: "SPLIT"
        note: panel.playing ? (panel.live.transient > 0.5 ? "ATTACK" : "SUSTAIN") : ""
        noteColor: panel.theme.listen
        x: timeCard.x + timeCard.width + panel.gap
        y: panel.cardsY
        width: panel.theme.splitWidth
        height: panel.cardsHeight
        Row {
            x: panel.theme.spaceMd
            y: panel.knobsY
            Repeater {
                model: ["temper.attacks", "temper.sustains", "temper.separate"]
                delegate: PanelKnob {
                    required property string modelData
                    parameterId: modelData
                    accent: panel.theme.listen
                }
            }
        }
        Note {
            x: panel.theme.spaceLg
            y: splitCard.height - panel.theme.cardNoteBottom
            width: splitCard.width - 2 * panel.theme.spaceLg
            text: "How deep it cuts on hits and on tails."
        }
    }
    Kit.Card {
        id: stereoCard
        theme: panel.theme
        title: "STEREO"
        x: splitCard.x + splitCard.width + panel.gap
        y: panel.cardsY
        width: panel.theme.stereoWidth
        height: panel.cardsHeight
        Kit.Segment {
            x: panel.theme.spaceLg
            y: panel.theme.stereoSegmentY
            width: stereoCard.width - 2 * panel.theme.spaceLg
            height: panel.theme.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "temper.stereo"
            labels: ["ST", "MID", "SIDE", "L", "R"]
        }
        PanelKnob {
            x: panel.theme.spaceLg
            y: panel.theme.cardLowerY
            parameterId: "temper.link"
            accent: panel.theme.cut
            // Linking only means something with both channels processed.
            inactive: panel.revision >= 0 && panel.controller.choice("temper.stereo") !== 0
        }
    }
    Kit.Card {
        id: unmaskCard
        theme: panel.theme
        title: "UNMASK"
        note: panel.keyed ? "KEY CONNECTED" : "NO KEY"
        noteColor: panel.keyed ? panel.theme.side : panel.theme.inkFaint
        x: stereoCard.x + stereoCard.width + panel.gap
        y: panel.cardsY
        width: panel.theme.unmaskWidth
        height: panel.cardsHeight
        Kit.Toggle {
            x: panel.theme.spaceLg
            y: panel.theme.toggleY
            width: panel.theme.toggleWidth
            height: panel.theme.toggleHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "temper.unmask"
            // The card's heading already says Unmask: the toggle says what it hears.
            label: "KEY"
            lit: panel.theme.side
            onCaption: "ON"
            offCaption: "OFF"
        }
        Rectangle {
            objectName: "temperKeyMeter"
            x: panel.theme.keyMeterX
            y: panel.theme.keyMeterY
            width: unmaskCard.width - panel.theme.keyMeterX - panel.theme.spaceLg
            height: panel.theme.keyMeterHeight
            radius: height / 2
            color: panel.theme.well
            border.color: panel.theme.wellEdge
            Rectangle {
                objectName: "temperKeyMeterFill"
                width: parent.width * panel.keyLevel()
                height: parent.height
                radius: height / 2
                color: panel.theme.side
            }
        }
        Row {
            x: panel.theme.spaceMd
            y: panel.theme.cardLowerY
            Repeater {
                model: ["temper.unmask_amount", "temper.unmask_focus"]
                delegate: PanelKnob {
                    required property string modelData
                    parameterId: modelData
                    accent: panel.theme.side
                    inactive: !panel.unmaskOn
                }
            }
        }
    }
    Kit.Card {
        id: outputCard
        theme: panel.theme
        title: "OUTPUT"
        x: unmaskCard.x + unmaskCard.width + panel.gap
        y: panel.cardsY
        width: panel.width - panel.margin - x
        height: panel.cardsHeight
        Row {
            x: panel.theme.spaceMd
            y: panel.knobsY
            Repeater {
                model: ["temper.mix", "temper.trim"]
                delegate: PanelKnob {
                    required property string modelData
                    parameterId: modelData
                    accent: panel.theme.glacier
                }
            }
        }
    }

    Timer {
        objectName: "temperTelemetryPoll"
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
