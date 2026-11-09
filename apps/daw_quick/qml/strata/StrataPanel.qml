pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
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
    // The accent edge round a focused card-wide control.
    component FocusRing: Rectangle {
        anchors.fill: parent
        color: "transparent"
        radius: panel.theme.cardRadius
        border.color: panel.theme.accent
        border.width: panel.theme.focusEdgeWidth
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
        border.color: activeFocus ? panel.theme.accent : lit ? tone : panel.theme.plateHi
        border.width: activeFocus ? panel.theme.focusEdgeWidth : panel.theme.hairline
        opacity: panel.controller.editable ? 1 : panel.theme.buttonDisabledOpacity
        activeFocusOnTab: panel.controller.editable
        Accessible.role: Accessible.Button
        Accessible.name: label
        Accessible.checked: lit
        Accessible.onPressAction: clicked()
        Keys.onReturnPressed: clicked()
        Keys.onSpacePressed: clicked()
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

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        brand: Component {
            Item {
                implicitWidth: wordmark.implicitWidth
                implicitHeight: subtitle.y + subtitle.implicitHeight
                Text {
                    id: wordmark
                    objectName: "strataWordmark"
                    text: "STRATA"
                    color: panel.theme.wordmarkInk
                    font.family: panel.theme.wordmarkFont
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Row {
                    x: panel.theme.wordmarkInset
                    y: wordmark.height + panel.theme.underlineGap
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
                    id: subtitle
                    objectName: "strataSubtitle"
                    x: panel.theme.wordmarkInset
                    y: wordmark.height + panel.theme.subtitleGap
                    text: "MULTIBAND DYNAMICS · " + (panel.ott ? "OTT" : panel.bandCount + " BANDS")
                        + (panel.keyed ? " · KEYED" : "")
                    color: panel.theme.subtitleInk
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
        }

        // The mode, phase, latency and delta row, and what keys the bands.
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
            text: ms >= 0 ? Format.milliseconds(ms) + " LATENCY"
                          : "LATENCY WHEN PLAYING"
            color: panel.theme.inkFaint
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeCaption
        }
        Kit.Switch {
            anchors.verticalCenter: parent.verticalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: "strata.delta"
            label: "Δ LISTEN"
            accent: panel.theme.key
        }
        Rectangle {
            id: keyedBadge
            objectName: "strataKeyed"
            visible: panel.keyed
            anchors.verticalCenter: parent.verticalCenter
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
    }

    // ---- Band strip ------------------------------------------------------------------

    Item {
        id: strip
        // Over the spectrum's plot, right of its frame's level scale.
        x: panel.margin + panel.theme.stripInset + spectrumFrame.gutterLeft
        y: header.y + header.height + panel.theme.stripGap
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
                border.color: activeFocus ? panel.theme.accent : chosen ? tone : panel.theme.plateHi
                border.width: activeFocus ? panel.theme.focusEdgeWidth : panel.theme.hairline
                // Return or Space selects the band; Left and Right select its neighbours.
                activeFocusOnTab: true
                Accessible.role: Accessible.PageTab
                Accessible.name: "Band " + (index + 1)
                Accessible.selected: chosen
                Accessible.onPressAction: panel.selected = tab.index
                Keys.onReturnPressed: panel.selected = tab.index
                Keys.onSpacePressed: panel.selected = tab.index
                Keys.onLeftPressed: panel.selected = Math.max(0, panel.selected - 1)
                Keys.onRightPressed: panel.selected = Math.min(panel.bandCount - 1, panel.selected + 1)
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
                            border.color: panel.theme.accent
                            border.width: activeFocus ? panel.theme.focusEdgeWidth : 0
                            function toggle() {
                                panel.theme.lastTouched = flag.parameterId
                                panel.controller.set_choice(flag.parameterId, flag.on ? 0 : 1)
                            }
                            activeFocusOnTab: panel.controller.editable
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: panel.controller.parameter(parameterId).name
                            Accessible.checked: on
                            Accessible.onToggleAction: toggle()
                            Keys.onReturnPressed: toggle()
                            Keys.onSpacePressed: toggle()
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
                                onClicked: flag.toggle()
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
                        border.color: panel.theme.accent
                        border.width: activeFocus ? panel.theme.focusEdgeWidth : 0
                        activeFocusOnTab: panel.controller.editable && visible
                        Accessible.role: Accessible.Button
                        Accessible.name: "Remove band " + (tab.index + 1)
                        Accessible.onPressAction: panel.removeBand(tab.index)
                        Keys.onReturnPressed: panel.removeBand(tab.index)
                        Keys.onSpacePressed: panel.removeBand(tab.index)
                        Keys.onDeletePressed: panel.removeBand(tab.index)
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
        y: strip.y + strip.height + panel.theme.spectrumGap
        width: panel.width - 2 * panel.margin
        height: panel.theme.spectrumHeight
        Kit.PlotFrame {
            id: spectrumFrame
            x: panel.theme.spectrumInset
            y: panel.theme.spectrumInset
            width: spectrumCard.width - 2 * panel.theme.spectrumInset
            height: spectrumCard.height - panel.theme.spectrumFoot
            theme: panel.theme
            live: panel.playing
            xUnit: "Hz"
            yUnit: "dB"
            xTicks: spectrum.frequencyTicks
            yTicks: spectrum.levelTicks
            StrataSpectrum {
                id: spectrum
                anchors.fill: parent
                panel: panel
                controller: panel.controller
                theme: panel.theme
            }
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
        note: Format.hertz(spectrum.edgeLow(panel.selected)) + " – " + Format.hertz(spectrum.edgeHigh(panel.selected))
        noteColor: panel.bandTone
        x: panel.margin
        y: panel.rowY
        width: panel.theme.transferCardWidth
        height: panel.rowHeight
        // Input across, output up; LIVE while the band's level dot rides it.
        Kit.PlotFrame {
            x: panel.theme.cardPad
            y: panel.theme.cardContentY
            width: transferCard.width - 2 * panel.theme.cardPad
            height: transferCard.height - panel.theme.cardContentY - panel.theme.cardPad
            theme: panel.theme
            live: panel.playing
            xUnit: "dB"
            yUnit: "dB"
            xTicks: transfer.inputTicks
            yTicks: transfer.outputTicks
            StrataTransfer {
                id: transfer
                anchors.fill: parent
                panel: panel
                controller: panel.controller
                theme: panel.theme
                band: panel.selected
            }
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
                    // In OTT mode OTT drives the band: these are its own
                    // values until a touch takes over.
                    linked: panel.ott
                    linkReason: panel.ott ? "Set by OTT" : ""
                }
            }
        }
        Column {
            x: panel.theme.cardPad
            y: knobs.y + knobs.height + panel.theme.bandRowsGap
            spacing: panel.theme.spaceMd
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
            id: takeOverArea
            objectName: "strataTakeOver"
            anchors.fill: parent
            visible: panel.ott
            enabled: panel.controller.editable
            activeFocusOnTab: visible && enabled
            Accessible.role: Accessible.Button
            Accessible.name: "Take the bands over from OTT"
            Accessible.onPressAction: panel.takeOver()
            Keys.onReturnPressed: panel.takeOver()
            Keys.onSpacePressed: panel.takeOver()
            onPressed: (mouse) => {
                panel.takeOver()
                mouse.accepted = true
            }
            FocusRing { visible: takeOverArea.activeFocus }
        }
    }

    Kit.Card {
        id: ottCard
        theme: panel.theme
        title: "OTT"
        note: panel.ott ? "DRIVING THREE BANDS" : offNote
        longestNote: offNote
        readonly property string offNote: "CLICK TO DRIVE THE BANDS"
        noteColor: panel.ott ? panel.theme.accent : panel.theme.inkFaint
        x: bandCard.x + bandCard.width + panel.gap
        y: panel.rowY
        width: Math.max(panel.theme.ottCardWidth, Math.ceil(fitWidth))
        height: panel.rowHeight
        readonly property string idleReason: panel.ott ? "" : "OTT drives the bands only in OTT mode"
        Kit.Knob {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.cardContentY
            controller: panel.controller
            theme: panel.theme
            parameterId: "strata.ott.depth"
            diameter: panel.theme.ottDepthDiameter
            readout: true
            inactive: !panel.ott
            inactiveReason: ottCard.idleReason
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
                inactive: !panel.ott
                inactiveReason: ottCard.idleReason
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "strata.ott.balance"
                diameter: panel.theme.smallKnobDiameter
                readout: true
                inactive: !panel.ott
                inactiveReason: ottCard.idleReason
                accent: panel.theme.live
            }
        }
        MouseArea {
            id: engageArea
            objectName: "strataEngageOtt"
            anchors.fill: parent
            visible: !panel.ott
            enabled: panel.controller.editable
            activeFocusOnTab: visible && enabled
            Accessible.role: Accessible.Button
            Accessible.name: "Drive the bands with OTT"
            Accessible.onPressAction: panel.engageOtt()
            Keys.onReturnPressed: panel.engageOtt()
            Keys.onSpacePressed: panel.engageOtt()
            onPressed: (mouse) => {
                panel.engageOtt()
                mouse.accepted = true
            }
            FocusRing { visible: engageArea.activeFocus }
        }
    }

    Kit.Card {
        id: globalCard
        theme: panel.theme
        title: "GLOBAL"
        note: panel.playing && Math.abs(panel.live.auto_gain_db) > 0.05
            ? "AUTO " + Format.signed_decibels(panel.live.auto_gain_db) : ""
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
                    text: panel.playing ? "LISTENING " + Format.fraction(panel.live.learn_progress)
                                        : "PLAY TO LISTEN"
                    color: panel.theme.key
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeCaption
                }
            }
            Text {
                objectName: "strataPeaks"
                text: panel.playing
                    ? "IN " + Format.signed_decibels(Math.max(panel.live.input_peak[0], panel.live.input_peak[1]))
                      + "  OUT " + Format.signed_decibels(Math.max(panel.live.output_peak[0], panel.live.output_peak[1]))
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
