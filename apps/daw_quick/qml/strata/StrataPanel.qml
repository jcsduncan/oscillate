pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Strata device panel (ADR-0082): multiband dynamics. The spectrum leads, with a tab per band
// (solo, mute, bypass, remove), draggable crossovers and thresholds, and
// each band's gain change drawn in its shape. Below: the selected band's
// transfer curve and controls, the OTT card and the global controls. In OTT
// mode the bands play what Depth, Time and Up / Down make of them; touching
// a band writes that out as bands first (strata_ott_values), so the sound
// does not change. Every control reads and writes through `controller`;
// meters and spectra come only from the telemetry (layout in strata.hpp).
Rectangle {
    id: panel
    required property var controller
    // Whether a sidechain send keys the effect, and the track that does.
    property bool keyed: false
    property string keyName: "the sidechain"
    property var live: ({ live: false })
    property StrataTheme theme: StrataTheme {}
    property int selected: 0

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int maxBands: 6
    readonly property int revision: controller.revision
    readonly property bool ott: revision >= 0 && controller.choice("strata.mode") === 1
    readonly property int bandCount: ott ? 3 : (revision >= 0 ? controller.choice("strata.bands") + 1 : 4)
    readonly property var ottValues: revision >= 0 && ott ? controller.strata_ott_values() : ({})
    readonly property bool playing: live.live === true
    readonly property var fields: ["threshold", "above", "below", "knee", "range", "attack",
        "release", "makeup", "process", "key", "solo", "mute", "bypass"]
    readonly property var bandNames: ["LOW", "LOW MID", "MID", "HIGH MID", "HIGH", "AIR"]

    objectName: "strataPanel"
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
    Accessible.name: "Strata multiband dynamics"

    onBandCountChanged: selected = Math.min(selected, bandCount - 1)

    // ---- Band values and edits ---------------------------------------------------------

    function bandId(b, field) {
        return "strata.band_" + (b + 1) + "." + field
    }
    // A band's value as it plays: in OTT mode, as OTT writes it out.
    function bandValue(b, field) {
        const id = bandId(b, field)
        return ottValues[id] !== undefined ? ottValues[id] : controller.value(id)
    }
    function read(base, id) {
        return base[id] !== undefined ? base[id] : controller.value(id)
    }
    // OTT written out as bands, as one edit: the first touch of a band.
    function takeOver() {
        if (ott)
            controller.set_values(controller.strata_ott_values())
    }
    function engageOtt() {
        if (!ott)
            controller.set_choice("strata.mode", 1)
    }
    function bandsOf(base, count) {
        const out = []
        for (let b = 0; b < count; ++b) {
            const band = {}
            for (const field of fields)
                band[field] = read(base, bandId(b, field))
            out.push(band)
        }
        return out
    }
    function crossoversOf(base, count) {
        const out = []
        for (let k = 0; k + 1 < count; ++k)
            out.push(read(base, "strata.xover_" + (k + 1)))
        return out
    }
    function write(base, bands, crossovers) {
        const values = base
        values["strata.mode"] = 0
        values["strata.bands"] = bands.length - 1
        for (let k = 0; k < crossovers.length; ++k)
            values["strata.xover_" + (k + 1)] = crossovers[k]
        for (let b = 0; b < bands.length; ++b)
            for (const field of fields)
                values[bandId(b, field)] = bands[b][field]
        controller.set_values(values)
    }
    // Splits the band under `hz` in two at `hz`; both halves start as it was.
    function addBandAt(hz) {
        const base = ott ? controller.strata_ott_values() : ({})
        const count = bandCount
        if (count >= maxBands)
            return
        const crossovers = crossoversOf(base, count)
        let at = 0
        while (at < crossovers.length && crossovers[at] < hz)
            ++at
        const bands = bandsOf(base, count)
        crossovers.splice(at, 0, hz)
        bands.splice(at + 1, 0, Object.assign({}, bands[at]))
        write(base, bands, crossovers)
        selected = at + 1
    }
    // Removes band `b`, its region joining the band below (the first band's
    // joins the one above), so removing a band just added restores the split.
    function removeBand(b) {
        const base = ott ? controller.strata_ott_values() : ({})
        const count = bandCount
        if (count <= 1)
            return
        const crossovers = crossoversOf(base, count)
        const bands = bandsOf(base, count)
        crossovers.splice(b > 0 ? b - 1 : 0, 1)
        bands.splice(b, 1)
        write(base, bands, crossovers)
        selected = Math.min(selected, bands.length - 1)
    }
    function resetRatios(b) {
        takeOver()
        const values = {}
        values[bandId(b, "above")] = controller.parameter(bandId(b, "above")).default
        values[bandId(b, "below")] = controller.parameter(bandId(b, "below")).default
        controller.set_values(values)
    }
    // The low band keyed from the sidechain (route the kick's send here).
    function kickAware() {
        takeOver()
        controller.set_choice(bandId(0, "key"), 2)
    }
    // Learn has listened: its suggestions become the thresholds, as one edit.
    function applyLearned() {
        const values = ott ? controller.strata_ott_values() : ({})
        for (let b = 0; b < bandCount; ++b) {
            const suggestion = live.learned[b]
            if (suggestion > -199)
                values[bandId(b, "threshold")] = suggestion
        }
        values["strata.learn"] = 0
        controller.set_values(values)
    }

    function signed(value, digits) {
        const text = Math.abs(value).toFixed(digits)
        const tiny = 0.5 * Math.pow(10, -digits)
        return value < -tiny ? "−" + text : value > tiny ? "+" + text : text
    }
    function hzText(hz) {
        return hz >= 1000 ? (hz / 1000).toFixed(hz >= 10000 ? 1 : 2) + " kHz" : Math.round(hz) + " Hz"
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
    component Button: Rectangle {
        id: button
        required property string label
        property bool lit: false
        property color tone: panel.theme.accent
        signal clicked()
        width: buttonText.implicitWidth + panel.theme.buttonPad
        height: panel.theme.buttonHeight
        radius: panel.theme.buttonRadius
        color: lit ? Qt.rgba(tone.r, tone.g, tone.b, panel.theme.buttonLitAlpha) : panel.theme.well
        border.color: lit ? tone : panel.theme.plateHi
        opacity: panel.controller.editable ? 1 : panel.theme.buttonDisabledOpacity
        Text {
            id: buttonText
            anchors.centerIn: parent
            text: button.label
            color: button.lit ? button.tone : panel.theme.inkDim
            font.pixelSize: panel.theme.typeCaption
            font.bold: true
            font.letterSpacing: panel.theme.tagSpacing
        }
        MouseArea {
            anchors.fill: parent
            enabled: panel.controller.editable
            onClicked: button.clicked()
        }
    }

    // ---- Header ----------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: panel.theme.wordmarkY
            text: "STRATA"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Row {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.underlineGap
            spacing: panel.theme.spaceXxs
            Repeater {
                model: panel.maxBands
                delegate: Rectangle {
                    required property int index
                    width: (wordmark.implicitWidth - panel.theme.underlineTrim) / panel.maxBands
                    height: panel.theme.strokeBold
                    radius: height / 2
                    color: panel.theme.band(index)
                }
            }
        }
        Text {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "MULTIBAND DYNAMICS · " + (panel.ott ? "OTT" : panel.bandCount + " BANDS")
                + (panel.keyed ? " · KEYED" : "")
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
                objectName: "strataPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "strataPresetDisplay"
                width: panel.theme.presetDisplayWidth
                height: panel.theme.presetDisplayHeight
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(display, panel.controller, panel.theme.accent)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                Text {
                    x: panel.theme.presetTextX
                    y: panel.theme.presetCategoryY
                    text: panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                }
                Text {
                    objectName: "strataPresetName"
                    x: panel.theme.presetTextX
                    y: panel.theme.presetNameY
                    width: display.width - panel.theme.presetNameRight
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeHeading
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
                objectName: "strataPresetNext"
                direction: 1
            }
        }

        Row {
            x: browser.x + browser.width + panel.theme.modeRowGap
            y: panel.theme.modeRowY
            spacing: panel.theme.modeRowSpacing
            Kit.Segment {
                controller: panel.controller
                theme: panel.theme
                parameterId: "strata.mode"
                labels: ["BANDS", "OTT"]
                width: panel.theme.modeSegmentWidth
            }
            Kit.Segment {
                controller: panel.controller
                theme: panel.theme
                parameterId: "strata.phase"
                labels: ["MIN PHASE", "LINEAR"]
                width: panel.theme.phaseSegmentWidth
            }
            Text {
                objectName: "strataLatency"
                anchors.verticalCenter: parent.verticalCenter
                readonly property real ms: panel.playing ? panel.live.latency_ms : -1
                text: ms >= 0 ? (ms < 0.05 ? "0 MS LATENCY" : ms.toFixed(1) + " MS LATENCY")
                              : "LATENCY WHEN PLAYING"
                color: panel.theme.inkFaint
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeCaption
            }
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "strata.delta"
                label: "Δ LISTEN"
                accent: panel.theme.key
            }
        }

        Rectangle {
            id: keyedBadge
            objectName: "strataKeyed"
            visible: panel.keyed
            anchors.right: status.left
            anchors.rightMargin: panel.theme.keyedBadgeGap
            y: panel.theme.statusY
            width: keyedText.implicitWidth + panel.theme.keyedBadgePad
            height: panel.theme.statusHeight
            radius: height / 2
            color: Qt.rgba(panel.theme.key.r, panel.theme.key.g, panel.theme.key.b, panel.theme.keyedFillAlpha)
            border.color: panel.theme.key
            Text {
                id: keyedText
                objectName: "strataKeyedText"
                anchors.centerIn: parent
                text: "KEYED · " + panel.keyName.toUpperCase()
                color: panel.theme.key
                font.pixelSize: panel.theme.typeCaption
                font.bold: true
            }
        }
        Kit.StatusBadge {
            id: status
            objectName: "strataStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- Band strip ------------------------------------------------------------------

    Item {
        id: strip
        x: panel.margin + panel.theme.stripInset
        y: panel.theme.stripY
        width: spectrum.width
        height: panel.theme.stripHeight

        Repeater {
            model: panel.bandCount
            delegate: Rectangle {
                id: tab
                required property int index
                readonly property color tone: panel.theme.band(index)
                readonly property bool chosen: index === panel.selected
                objectName: "strataBandTab:" + index
                x: Math.max(0, Math.min(strip.width - width,
                    spectrum.xOf(spectrum.centreOf(index)) - width / 2))
                y: panel.theme.tabY
                width: tabRow.implicitWidth + panel.theme.tabPad
                height: panel.theme.tabHeight
                radius: panel.theme.radiusLg
                color: chosen ? Qt.rgba(tone.r, tone.g, tone.b, panel.theme.tabChosenAlpha) : panel.theme.plate
                border.color: chosen ? tone : panel.theme.plateHi
                MouseArea {
                    anchors.fill: parent
                    onClicked: panel.selected = tab.index
                }
                Row {
                    id: tabRow
                    x: panel.theme.tabPad / 2
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: panel.theme.spaceXs
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.tabDotSize
                        height: panel.theme.tabDotSize
                        radius: panel.theme.tabDotRadius
                        color: tab.tone
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: (panel.bandCount === 1 ? "FULL" : panel.bandNames[tab.index])
                        color: tab.chosen ? panel.theme.ink : panel.theme.inkDim
                        font.pixelSize: panel.theme.typeCaption
                        font.bold: true
                        font.letterSpacing: panel.theme.tabLabelSpacing
                    }
                    Repeater {
                        model: [["solo", "S"], ["mute", "M"], ["bypass", "B"]]
                        delegate: Rectangle {
                            id: flag
                            required property var modelData
                            readonly property string parameterId: panel.bandId(tab.index, modelData[0])
                            readonly property bool on: panel.revision >= 0 && panel.controller.choice(parameterId) === 1
                            objectName: "strataToggle:" + parameterId
                            width: panel.theme.flagSize
                            height: panel.theme.flagSize
                            radius: panel.theme.radiusMd
                            color: on ? tab.tone : panel.theme.well
                            Text {
                                anchors.centerIn: parent
                                text: flag.modelData[1]
                                color: flag.on ? panel.theme.screen : panel.theme.inkFaint
                                font.pixelSize: panel.theme.typeCaption
                                font.bold: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: panel.controller.editable
                                onClicked: {
                                    panel.theme.lastTouched = flag.parameterId
                                    panel.controller.set_choice(flag.parameterId, flag.on ? 0 : 1)
                                }
                            }
                        }
                    }
                    Rectangle {
                        objectName: "strataRemoveBand:" + tab.index
                        visible: panel.bandCount > 1
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.flagSize
                        height: panel.theme.flagSize
                        radius: panel.theme.radiusMd
                        color: panel.theme.well
                        Text {
                            anchors.centerIn: parent
                            text: "×"
                            color: panel.theme.inkFaint
                            font.pixelSize: panel.theme.typeBody
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: panel.controller.editable
                            onClicked: panel.removeBand(tab.index)
                        }
                    }
                }
            }
        }
    }

    // ---- Spectrum --------------------------------------------------------------------

    Kit.Card {
        id: spectrumCard
        theme: panel.theme
        title: ""
        x: panel.margin
        y: panel.theme.spectrumY
        width: panel.width - 2 * panel.margin
        height: panel.theme.spectrumHeight
        StrataSpectrum {
            id: spectrum
            x: panel.theme.spectrumInset
            y: panel.theme.spectrumInset
            width: spectrumCard.width - 2 * panel.theme.spectrumInset
            height: spectrumCard.height - panel.theme.spectrumFoot
            panel: panel
            controller: panel.controller
            theme: panel.theme
        }
        Note {
            x: panel.theme.spectrumNoteX
            y: spectrumCard.height - panel.theme.spectrumNoteBottom
            width: spectrumCard.width - 2 * panel.theme.spectrumNoteX
            text: "Drag a crossover or a band's dashed threshold · double-click to add a band (up to six) · the coloured fills are each band's gain change now"
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.spectrumNoteX
            y: spectrumCard.height - panel.theme.bandsRowBottom
            spacing: panel.theme.spaceSm
            Caption { text: "BANDS"; anchors.verticalCenter: parent.verticalCenter }
            Kit.Segment {
                controller: panel.controller
                theme: panel.theme
                parameterId: "strata.bands"
                width: panel.theme.bandsSegmentWidth
                height: panel.theme.bandsSegmentHeight
            }
        }
    }

    // ---- Bottom row ------------------------------------------------------------------

    readonly property real rowY: spectrumCard.y + spectrumCard.height + gap
    readonly property real rowHeight: height - rowY - margin
    readonly property color bandTone: theme.band(selected)

    Kit.Card {
        id: transferCard
        theme: panel.theme
        title: "TRANSFER · " + (panel.bandCount === 1 ? "FULL" : panel.bandNames[panel.selected])
        note: panel.hzText(spectrum.edgeLow(panel.selected)) + " – " + panel.hzText(spectrum.edgeHigh(panel.selected))
        noteColor: panel.bandTone
        x: panel.margin
        y: panel.rowY
        width: panel.theme.transferCardWidth
        height: panel.rowHeight
        StrataTransfer {
            x: panel.theme.cardPad
            y: panel.theme.cardContentY
            width: transferCard.width - 2 * panel.theme.cardPad
            height: transferCard.height - panel.theme.cardContentY - panel.theme.cardPad
            panel: panel
            controller: panel.controller
            theme: panel.theme
            band: panel.selected
        }
    }

    Kit.Card {
        id: bandCard
        theme: panel.theme
        title: "BAND " + (panel.selected + 1)
        note: panel.ott ? "DRIVEN BY OTT · TOUCH TO TAKE OVER" : ""
        noteColor: panel.theme.accent
        x: transferCard.x + transferCard.width + panel.gap
        y: panel.rowY
        width: panel.theme.bandCardWidth
        height: panel.rowHeight

        Grid {
            id: knobs
            x: panel.theme.cardPad
            y: panel.theme.bandKnobsY
            // In OTT mode these are the band's own values, not what plays.
            opacity: panel.ott ? panel.theme.ottOverrideOpacity : 1
            columns: 4
            columnSpacing: panel.theme.knobColumnSpacing
            rowSpacing: panel.theme.spaceSm
            Repeater {
                model: ["threshold", "above", "below", "makeup", "attack", "release", "knee", "range"]
                delegate: Kit.Knob {
                    required property string modelData
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: panel.bandId(panel.selected, modelData)
                    diameter: panel.theme.bandKnobDiameter
                    readout: true
                    accent: panel.bandTone
                }
            }
        }
        Column {
            x: panel.theme.cardPad
            y: knobs.y + knobs.height + panel.theme.bandRowsGap
            spacing: panel.theme.spaceMd
            opacity: knobs.opacity
            Row {
                spacing: panel.theme.spaceMd
                Caption { text: "PROCESS"; width: panel.theme.bandRowCaptionWidth; anchors.verticalCenter: parent.verticalCenter }
                Kit.Segment {
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: panel.bandId(panel.selected, "process")
                    labels: ["STEREO", "MID", "SIDE", "L", "R"]
                    width: panel.theme.processSegmentWidth
                    accent: panel.bandTone
                }
            }
            Row {
                spacing: panel.theme.spaceMd
                Caption { text: "KEY"; width: panel.theme.bandRowCaptionWidth; anchors.verticalCenter: parent.verticalCenter }
                Kit.Segment {
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: panel.bandId(panel.selected, "key")
                    labels: ["BAND", "INPUT", "SIDECHAIN"]
                    width: panel.theme.keySegmentWidth
                    accent: panel.theme.key
                }
                Button {
                    objectName: "strataKickAware"
                    label: "KICK-AWARE LOW"
                    tone: panel.theme.key
                    lit: !panel.ott && panel.revision >= 0
                         && panel.controller.choice(panel.bandId(0, "key")) === 2
                    onClicked: panel.kickAware()
                }
            }
        }
        // In OTT mode the first touch writes OTT out as bands.
        MouseArea {
            objectName: "strataTakeOver"
            anchors.fill: parent
            visible: panel.ott
            enabled: panel.controller.editable
            onPressed: (mouse) => {
                panel.takeOver()
                mouse.accepted = true
            }
        }
    }

    Kit.Card {
        id: ottCard
        theme: panel.theme
        title: "OTT"
        note: panel.ott ? "DRIVING THREE BANDS" : "CLICK TO DRIVE THE BANDS"
        noteColor: panel.ott ? panel.theme.accent : panel.theme.inkFaint
        x: bandCard.x + bandCard.width + panel.gap
        y: panel.rowY
        width: panel.theme.ottCardWidth
        height: panel.rowHeight
        opacity: panel.ott ? 1 : panel.theme.ottIdleOpacity
        Kit.Knob {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.cardContentY
            controller: panel.controller
            theme: panel.theme
            parameterId: "strata.ott.depth"
            diameter: panel.theme.ottDepthDiameter
            readout: true
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.ottRowY
            spacing: panel.theme.knobColumnSpacing
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "strata.ott.time"
                diameter: panel.theme.smallKnobDiameter
                readout: true
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "strata.ott.balance"
                diameter: panel.theme.smallKnobDiameter
                readout: true
                accent: panel.theme.live
            }
        }
        MouseArea {
            objectName: "strataEngageOtt"
            anchors.fill: parent
            visible: !panel.ott
            enabled: panel.controller.editable
            onPressed: (mouse) => {
                panel.engageOtt()
                mouse.accepted = true
            }
        }
    }

    Kit.Card {
        id: globalCard
        theme: panel.theme
        title: "GLOBAL"
        note: panel.playing && Math.abs(panel.live.auto_gain_db) > 0.05
            ? "AUTO " + panel.signed(panel.live.auto_gain_db, 1) + " DB" : ""
        noteColor: panel.theme.live
        x: ottCard.x + ottCard.width + panel.gap
        y: panel.rowY
        width: panel.width - panel.margin - x
        height: panel.rowHeight
        Grid {
            id: globalKnobs
            x: panel.theme.cardPad
            y: panel.theme.cardContentY
            columns: 2
            columnSpacing: panel.theme.globalColumnSpacing
            rowSpacing: panel.theme.spaceXs
            Repeater {
                model: ["strata.input", "strata.output", "strata.mix", "strata.lookahead"]
                delegate: Kit.Knob {
                    required property string modelData
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData
                    diameter: panel.theme.smallKnobDiameter
                    readout: true
                }
            }
        }
        Column {
            x: panel.theme.cardPad
            y: globalKnobs.y + globalKnobs.height + panel.theme.spaceMd
            spacing: panel.theme.spaceMd
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "strata.auto_gain"
                label: "AUTO GAIN"
            }
            Row {
                spacing: panel.theme.spaceMd
                Kit.Switch {
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "strata.learn"
                    label: "AUTO THRESHOLD"
                    accent: panel.theme.key
                }
                Text {
                    objectName: "strataLearnProgress"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: panel.revision >= 0 && panel.controller.choice("strata.learn") === 1
                    text: panel.playing ? "LISTENING " + Math.round(100 * panel.live.learn_progress) + "%"
                                        : "PLAY TO LISTEN"
                    color: panel.theme.key
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeCaption
                }
            }
            Text {
                objectName: "strataPeaks"
                text: panel.playing
                    ? "IN " + panel.signed(Math.max(panel.live.input_peak[0], panel.live.input_peak[1]), 1)
                      + "  OUT " + panel.signed(Math.max(panel.live.output_peak[0], panel.live.output_peak[1]), 1)
                    : "IN —  OUT —"
                color: panel.theme.inkDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeSmall
            }
        }
    }

    Timer {
        objectName: "strataTelemetryPoll"
        interval: panel.playing ? 33 : 250
        repeat: true
        running: panel.visible
        onTriggered: {
            panel.live = panel.controller.telemetry()
            if (panel.playing && panel.live.learn_progress >= 1
                    && panel.controller.choice("strata.learn") === 1)
                panel.applyLearned()
        }
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
