pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The Contour device panel: a precision lab instrument in graphite and
// phosphor teal. The screen shows the filter's response measured from the
// real effect, live while it runs; beside it the model bank (all eleven
// models in their families) and the main filter dials; below, the LFO,
// envelope follower and output sections as the parameter table groups them.
// Every control reads and writes through `controller`
// (NativeSynthDeviceController for the Contour engine); no project or audio
// state lives in QML.
Rectangle {
    id: panel
    required property var controller
    property ContourTheme theme: ContourTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.sectionGap
    // The model shown before the controller has values; the highest model;
    // the SEM (the one model Morph shapes); keys per row in the model bank.
    readonly property int restingModel: 1
    readonly property int lastModel: 10
    readonly property int semModel: 8
    readonly property int bankColumns: 3
    // Telemetry polling: fast while the effect runs, slow while idle; the
    // live curve is re-measured when the live cutoff moves more than
    // 1/remeasureDivisions of an octave.
    readonly property int livePollMs: 50
    readonly property int idlePollMs: 250
    readonly property real remeasureDivisions: 48
    readonly property int responseSamples: 240
    readonly property int model: controller.revision >= 0 ? controller.choice("contour.model") : restingModel
    readonly property bool synced: controller.revision >= 0 && controller.choice("contour.lfo.sync") === 1
    readonly property bool sidechain: controller.revision >= 0 && controller.choice("contour.env.source") === 1
    readonly property bool envelopeOn: controller.revision >= 0 && controller.value("contour.env.amount") !== 0
    readonly property bool lfoOn: controller.revision >= 0 && controller.value("contour.lfo.depth") > 0
    readonly property var families: [
        { title: "STATE VARIABLE", models: [0, 1, 2, 3, 4, 5] },
        { title: "CHARACTER", models: [6, 7, 8] },
        { title: "COMB", models: [9, 10] }]
    readonly property var familyNames: ["STATE VARIABLE", "STATE VARIABLE", "STATE VARIABLE",
        "STATE VARIABLE", "STATE VARIABLE", "STATE VARIABLE", "CHARACTER", "CHARACTER",
        "CHARACTER", "COMB", "COMB"]
    // Live effect state (ADR-0037), polled while the panel is visible.
    property var live: ({ live: false })
    property var liveResponse: []
    property real liveResponseHz: 0
    readonly property bool liveOn: live.live === true && live.cutoff_hz !== undefined

    objectName: "contourPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.bodyTop }
        GradientStop { position: 0.5; color: panel.theme.body }
        GradientStop { position: 1; color: panel.theme.bodyBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Contour filter"

    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        return info.name === undefined ? "" : (info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || controller.revision < 0 ? "" : controller.display(id)
    }
    function chooseModel(index) {
        if (!controller.editable)
            return
        controller.set_choice("contour.model", Math.max(0, Math.min(lastModel, index)))
        theme.lastTouched = "contour.model"
    }
    // Poll telemetry; re-measure the live curve when the live cutoff has
    // moved far enough.
    function poll() {
        live = controller.telemetry()
        if (!liveOn) {
            if (liveResponse.length > 0)
                liveResponse = []
            liveResponseHz = 0
            return
        }
        const hz = live.cutoff_hz
        if (liveResponseHz <= 0 || Math.abs(Math.log(hz / liveResponseHz)) > Math.LN2 / remeasureDivisions) {
            liveResponse = controller.contour_response(responseSamples, hz)
            liveResponseHz = hz
        }
    }

    Timer {
        interval: panel.liveOn ? panel.livePollMs : panel.idlePollMs
        running: panel.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: panel.poll()
    }
    Connections {
        target: panel.controller
        // A new patch changes the live curve's shape too.
        function onValues_changed() { panel.liveResponseHz = 0 }
    }

    // A section plate with an engraved title.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        property color titleColor: panel.theme.inkDim
        radius: panel.theme.radiusXl
        color: panel.theme.plate
        border.color: panel.theme.plateEdge
        Rectangle {
            x: panel.theme.hairline
            y: panel.theme.hairline
            width: parent.width - 2 * panel.theme.hairline
            height: panel.theme.hairline
            color: panel.theme.plateHi
        }
        Rectangle {
            x: panel.theme.spaceLg
            y: panel.theme.plateTickY
            width: panel.theme.plateTickWidth
            height: panel.theme.plateTickHeight
            radius: panel.theme.plateTickRadius
            color: plate.titleColor
        }
        Text {
            id: plateTitle
            x: panel.theme.plateTitleX
            y: panel.theme.plateTitleY
            text: plate.title
            color: plate.titleColor
            font.pixelSize: panel.theme.typeSmall
            font.bold: true
            font.letterSpacing: panel.theme.plateTitleSpacing
        }
        Text {
            x: plateTitle.x + plateTitle.implicitWidth + panel.theme.plateNoteGap
            y: panel.theme.plateNoteY
            visible: plate.note !== ""
            text: plate.note
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.tagSpacing
        }
    }

    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    component Dial: Kit.Knob {
        controller: panel.controller
        theme: panel.theme
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
            y: panel.theme.spaceXxs
            text: "CONTOUR"
            color: panel.theme.ink
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Rectangle {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height
            width: panel.theme.wordmarkRuleWidth
            height: panel.theme.strokeBold
            color: panel.theme.accent
        }
        Text {
            x: wordmark.x + panel.theme.subtitleX
            y: wordmark.y + wordmark.height - panel.theme.spaceXs
            text: "MULTIMODE FILTER · " + panel.familyNames[panel.model]
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.subtitleSpacing
        }

        // Preset browser and readout.
        Row {
            id: browser
            x: panel.theme.browserX
            y: panel.theme.spaceXs
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "contourPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "contourPresetDisplay"
                width: panel.theme.presetDisplayWidth
                height: panel.theme.presetDisplayHeight
                radius: panel.theme.presetDisplayRadius
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.presetFocusStroke : panel.theme.hairline
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
                    y: panel.theme.spaceXs
                    text: panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.tagSpacing
                }
                Text {
                    objectName: "contourPresetName"
                    x: panel.theme.spaceLg
                    y: panel.theme.presetNameY
                    width: panel.theme.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.pixelSize: panel.theme.typeHeading
                    font.bold: true
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
                    objectName: "contourReadout"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.spaceXs
                    width: display.width - x - panel.theme.space2xl
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "contourReadoutValue"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetValueY
                    width: display.width - x - panel.theme.space2xl
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSubtitle
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.presetChevronMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.accent
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
                objectName: "contourPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "contourStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.spaceLg
        }
    }

    // ---- Screen and filter ------------------------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Plate {
        id: screenPlate
        title: "RESPONSE"
        note: panel.liveOn ? "LIVE · MEASURED FROM THE FILTER" : "MEASURED FROM THE FILTER"
        titleColor: panel.liveOn ? panel.theme.accent : panel.theme.inkDim
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.screenPlateWidth
        height: panel.rowAHeight
        // While live: what the ruler's colours mean.
        Row {
            objectName: "contourLiveLegend"
            visible: panel.liveOn
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.plateInset
            y: panel.theme.plateTitleY
            spacing: panel.theme.legendSpacing
            Repeater {
                model: [{ label: "SET", colour: "accentDim" }, { label: "LFO", colour: "lfo" },
                        { label: "ENVELOPE", colour: "envelope" }, { label: "LIVE", colour: "ink" }]
                delegate: Row {
                    id: legend
                    required property var modelData
                    spacing: panel.theme.legendSwatchGap
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.legendSwatchWidth
                        height: panel.theme.legendSwatchHeight
                        radius: panel.theme.legendSwatchRadius
                        color: panel.theme[legend.modelData.colour]
                    }
                    Text {
                        text: legend.modelData.label
                        color: panel.theme.inkDim
                        font.pixelSize: panel.theme.typeMicro
                        font.bold: true
                        font.letterSpacing: panel.theme.tagSpacing
                    }
                }
            }
        }
        ContourResponseView {
            x: panel.theme.screenInset
            y: panel.theme.screenTop
            width: screenPlate.width - 2 * panel.theme.screenInset
            height: screenPlate.height - panel.theme.screenTop - panel.theme.screenInset
            controller: panel.controller
            theme: panel.theme
            live: panel.live
            liveResponse: panel.liveResponse
        }
    }

    Plate {
        id: filterPlate
        title: "FILTER"
        x: screenPlate.x + screenPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight

        // The model bank: every model, in its family.
        Column {
            id: bank
            objectName: "contourModels:contour.model"
            x: panel.theme.plateInset
            y: panel.theme.plateContentY
            spacing: panel.theme.spaceSm
            activeFocusOnTab: panel.controller.editable
            Accessible.role: Accessible.PageTabList
            Accessible.name: "Model"
            Keys.onLeftPressed: panel.chooseModel(panel.model - 1)
            Keys.onRightPressed: panel.chooseModel(panel.model + 1)
            Keys.onUpPressed: panel.chooseModel(panel.model - panel.bankColumns)
            Keys.onDownPressed: panel.chooseModel(panel.model + panel.bankColumns)
            Keys.onDeletePressed: {
                panel.controller.reset_to_default("contour.model")
                panel.theme.lastTouched = "contour.model"
            }
            Repeater {
                model: panel.families
                delegate: Column {
                    id: family
                    required property var modelData
                    spacing: panel.theme.familyGap
                    Caption {
                        text: family.modelData.title
                        color: panel.familyNames[panel.model] === family.modelData.title
                            ? panel.theme.inkDim : panel.theme.inkFaint
                    }
                    Grid {
                        columns: panel.bankColumns
                        spacing: panel.theme.spaceXs
                        Repeater {
                            model: family.modelData.models
                            delegate: Rectangle {
                                id: key
                                required property int modelData
                                readonly property bool on: modelData === panel.model
                                readonly property var names: panel.controller.parameter("contour.model").choices
                                objectName: "contourModel:" + modelData
                                width: (filterPlate.width - 2 * panel.theme.plateInset
                                        - (panel.bankColumns - 1) * panel.theme.spaceXs) / panel.bankColumns
                                height: panel.theme.modelKeyHeight
                                radius: panel.theme.radiusSm
                                color: on ? Qt.rgba(panel.theme.accent.r, panel.theme.accent.g,
                                                    panel.theme.accent.b, panel.theme.modelKeyWashAlpha)
                                          : keyArea.containsMouse ? panel.theme.buttonHi : panel.theme.button
                                border.color: on ? panel.theme.accent
                                                 : bank.activeFocus && on ? panel.theme.accent
                                                                          : panel.theme.knobRing
                                border.width: on ? panel.theme.modelKeyStroke : panel.theme.hairline
                                Accessible.role: Accessible.PageTab
                                Accessible.name: names[modelData]
                                Accessible.checked: on
                                Rectangle {
                                    x: panel.theme.modelLedX
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: panel.theme.modelLedSize
                                    height: panel.theme.modelLedSize
                                    radius: panel.theme.modelLedSize / 2
                                    color: key.on ? panel.theme.accent : panel.theme.ledOff
                                }
                                Text {
                                    anchors.centerIn: parent
                                    anchors.horizontalCenterOffset: panel.theme.modelLabelOffset
                                    text: key.names[key.modelData].toUpperCase()
                                    color: key.on ? panel.theme.ink : panel.theme.inkDim
                                    font.pixelSize: panel.theme.typeSmall
                                    font.bold: true
                                    font.letterSpacing: panel.theme.tagSpacing
                                }
                                MouseArea {
                                    id: keyArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: panel.controller.editable
                                    onClicked: {
                                        bank.forceActiveFocus()
                                        panel.chooseModel(key.modelData)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            x: panel.theme.plateInset
            y: bank.y + bank.height + panel.theme.bankDividerGap
            width: filterPlate.width - 2 * panel.theme.plateInset
            height: panel.theme.hairline
            color: panel.theme.plateEdge
        }

        Row {
            x: panel.theme.filterDialX
            y: bank.y + bank.height + panel.theme.filterDialGap
            spacing: panel.theme.spaceSm
            Dial {
                parameterId: "contour.cutoff"
                diameter: panel.theme.cutoffDiameter
            }
            Dial {
                parameterId: "contour.resonance"
                diameter: panel.theme.knobDiameter
                y: panel.smallDialDrop
            }
            Dial {
                parameterId: "contour.drive"
                diameter: panel.theme.knobDiameter
                y: panel.smallDialDrop
            }
            Dial {
                parameterId: "contour.morph"
                diameter: panel.theme.knobDiameter
                y: panel.smallDialDrop
                // Morph shapes the SEM only.
                inactive: panel.model !== panel.semModel
            }
        }
    }

    // ---- Modulation and output ------------------------------------------------------------

    // The smaller filter dials sit centred on the cutoff dial.
    readonly property real smallDialDrop: (theme.cutoffDiameter - theme.knobDiameter) / 2
    readonly property real rowBY: panel.rowAY + panel.rowAHeight + panel.gap
    readonly property real rowBHeight: panel.height - panel.margin - rowBY

    Plate {
        id: lfoPlate
        title: "LFO"
        titleColor: panel.lfoOn ? panel.theme.lfo : panel.theme.inkDim
        note: panel.synced ? "TEMPO SYNC" : "FREE"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.lfoPlateWidth
        height: panel.rowBHeight

        ContourLfoView {
            x: panel.theme.lfoViewX
            y: panel.theme.plateContentY
            width: panel.theme.lfoViewWidth
            height: panel.theme.lfoViewHeight
            controller: panel.controller
            theme: panel.theme
            live: panel.live
        }
        Caption {
            x: panel.theme.lfoControlsX
            y: panel.theme.lfoShapeCaptionY
            text: "SHAPE"
        }
        Kit.Segment {
            x: panel.theme.lfoControlsX
            y: panel.theme.lfoShapeY
            width: panel.theme.lfoShapeWidth
            height: panel.theme.compactControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "contour.lfo.shape"
            accent: panel.theme.lfo
            labels: ["SIN", "TRI", "SAW↑", "SAW↓", "SQR", "S&H"]
        }
        Caption {
            x: panel.theme.lfoControlsX
            y: panel.theme.lfoTimingCaptionY
            text: "TIMING"
        }
        Kit.Segment {
            x: panel.theme.lfoControlsX
            y: panel.theme.lfoTimingY
            width: panel.theme.lfoSyncWidth
            height: panel.theme.compactControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "contour.lfo.sync"
            accent: panel.theme.lfo
            labels: ["FREE", "SYNC"]
        }
        Kit.Dropdown {
            x: panel.theme.lfoDivisionX
            y: panel.theme.lfoTimingY
            width: panel.theme.lfoDivisionWidth
            height: panel.theme.compactControlHeight
            controller: panel.controller
            theme: panel.theme
            menu: menu
            parameterId: "contour.lfo.division"
            accent: panel.theme.lfo
            inactive: !panel.synced
        }
        Row {
            x: panel.theme.dialRowX
            y: panel.theme.dialRowY
            spacing: panel.theme.dialRowSpacing
            Dial {
                parameterId: "contour.lfo.rate"
                accent: panel.theme.lfo
                inactive: panel.synced
            }
            Dial {
                parameterId: "contour.lfo.depth"
                accent: panel.theme.lfo
            }
            Dial {
                parameterId: "contour.lfo.stereo"
                accent: panel.theme.lfo
            }
        }
    }

    Plate {
        id: envelopePlate
        title: "ENVELOPE"
        titleColor: panel.envelopeOn ? panel.theme.envelope : panel.theme.inkDim
        note: panel.sidechain ? "FOLLOWS THE SIDECHAIN" : "FOLLOWS THE INPUT"
        x: lfoPlate.x + lfoPlate.width + panel.gap
        y: panel.rowBY
        width: panel.theme.envelopePlateWidth
        height: panel.rowBHeight

        Caption {
            x: panel.theme.plateInset
            y: panel.theme.plateContentY
            text: "SOURCE"
        }
        Kit.Segment {
            x: panel.theme.plateInset
            y: panel.theme.sourceY
            width: panel.theme.sourceWidth
            controller: panel.controller
            theme: panel.theme
            parameterId: "contour.env.source"
            accent: panel.theme.envelope
            labels: ["INPUT", "SIDECHAIN"]
        }
        // The follower's level, -60 to 0 dB, live.
        Item {
            id: meter
            objectName: "contourEnvelopeMeter"
            // The meter's range in dB below full scale, its divisions, and
            // the floor that keeps silence off the logarithm.
            readonly property real rangeDb: 60
            readonly property int divisions: 6
            readonly property real silence: 1e-9
            readonly property real level: panel.liveOn && panel.live.envelope !== undefined
                ? Math.max(0, Math.min(1, (20 * Math.log10(panel.live.envelope + silence) + rangeDb) / rangeDb)) : 0
            x: panel.theme.plateInset
            y: panel.theme.lowerRowY
            width: panel.theme.sourceWidth
            height: panel.theme.meterHeight
            Caption {
                y: panel.theme.meterCaptionY
                text: "LEVEL"
            }
            Caption {
                anchors.right: parent.right
                y: panel.theme.meterCaptionY
                text: Format.decibels(meter.level > 0 ? meter.level * meter.rangeDb - meter.rangeDb : -Infinity)
                color: meter.level > 0 ? panel.theme.envelope : panel.theme.inkFaint
                font.family: panel.theme.mono
            }
            Rectangle {
                y: panel.theme.meterBarY
                width: parent.width
                height: panel.theme.meterBarHeight
                radius: panel.theme.radiusXs
                color: panel.theme.well
                border.color: panel.theme.wellEdge
                Rectangle {
                    x: panel.theme.hairline
                    y: panel.theme.hairline
                    height: parent.height - 2 * panel.theme.hairline
                    width: (parent.width - 2 * panel.theme.hairline) * meter.level
                    radius: panel.theme.barFillRadius
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: Qt.rgba(panel.theme.envelope.r, panel.theme.envelope.g, panel.theme.envelope.b, panel.theme.meterFadeAlpha) }
                        GradientStop { position: 1; color: panel.theme.envelope }
                    }
                }
                Repeater {
                    model: meter.divisions - 1
                    delegate: Rectangle {
                        required property int index
                        x: (index + 1) * parent.width / meter.divisions
                        y: panel.theme.spaceXxs
                        width: panel.theme.hairline
                        height: parent.height - 2 * panel.theme.spaceXxs
                        color: panel.theme.wellEdge
                    }
                }
            }
        }
        // The follower's answer to a burst: it rises with the attack while
        // the burst lasts and falls with the release after, on a time base
        // wide enough for both, scaled by the amount's sweep in semitones
        // (up or down).
        Kit.EnvelopeView {
            id: response
            readonly property real amount: panel.controller.revision >= 0 ? panel.controller.value("contour.env.amount") : 0
            readonly property real attack: panel.controller.revision >= 0
                ? panel.controller.value("contour.env.attack") : restingAttack
            readonly property real release: panel.controller.revision >= 0
                ? panel.controller.value("contour.env.release") : restingRelease
            // Times (ms) shown before the controller has values.
            readonly property real restingAttack: 5
            readonly property real restingRelease: 120
            // The burst lasts long enough to settle (this many attack times,
            // and at least shortestBurst ms); the view then shows this many
            // release times.
            readonly property real settleTimes: 4
            readonly property real shortestBurst: 20
            readonly property real burstMs: Math.max(settleTimes * attack, shortestBurst)
            readonly property real spanMs: burstMs + settleTimes * release
            // The sweep's height: full at fullSweep semitones, never below
            // smallestSweep so the curve still reads.
            readonly property real fullSweep: 48
            readonly property real smallestSweep: 0.12
            readonly property int samples: 160
            readonly property bool up: amount >= 0

            objectName: "contourEnvelopeView"
            x: panel.theme.envelopeViewX
            y: panel.theme.plateContentY
            width: envelopePlate.width - x - panel.theme.plateInset
            height: panel.theme.envelopeViewHeight
            controller: panel.controller
            theme: panel.theme
            accent: panel.theme.envelope
            lineColor: amount === 0 ? panel.theme.inkFaint : accent
            glowColor: Qt.rgba(accent.r, accent.g, accent.b,
                               amount === 0 ? panel.theme.envelopeIdleGlowAlpha : panel.theme.envelopeGlowAlpha)
            levels: curve()
            bandEnd: burstMs / spanMs
            baselineLevel: up ? 0 : 1
            Accessible.name: "Envelope follower response"

            function curve() {
                const scale = Math.max(smallestSweep, Math.abs(amount) / fullSweep)
                const out = []
                let level = 0
                let peak = 0
                for (let i = 0; i <= samples; ++i) {
                    const t = i / samples * spanMs
                    if (t <= burstMs) {
                        level = 1 - Math.exp(-t / attack)
                        peak = level
                    } else {
                        level = peak * Math.exp(-(t - burstMs) / release)
                    }
                    out.push(up ? level * scale : 1 - level * scale)
                }
                return out
            }

            Text {
                x: panel.theme.envelopeLabelInset
                y: response.height - panel.theme.envelopeLabelLift
                text: "ATTACK"
                color: panel.theme.envelopeLabelInk
                font.pixelSize: panel.theme.envelopeLabelSize
                font.bold: true
                font.letterSpacing: panel.theme.envelopeLabelSpacing
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: panel.theme.envelopeLabelInset
                y: response.height - panel.theme.envelopeLabelLift
                text: "RELEASE · " + Format.milliseconds(response.spanMs)
                color: panel.theme.envelopeLabelInk
                font.pixelSize: panel.theme.envelopeLabelSize
                font.bold: true
                font.letterSpacing: panel.theme.envelopeLabelSpacing
            }
        }
        Row {
            x: panel.theme.dialRowX
            y: panel.theme.dialRowY
            spacing: panel.theme.dialRowSpacing
            Dial {
                parameterId: "contour.env.amount"
                accent: panel.theme.envelope
            }
            Dial {
                parameterId: "contour.env.attack"
                accent: panel.theme.envelope
                inactive: !panel.envelopeOn
            }
            Dial {
                parameterId: "contour.env.release"
                accent: panel.theme.envelope
                inactive: !panel.envelopeOn
            }
        }
    }

    Plate {
        id: outputPlate
        title: "OUTPUT"
        x: envelopePlate.x + envelopePlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        // Dry against wet, as the mix sets them.
        Item {
            id: balance
            objectName: "contourBalance"
            readonly property real wet: panel.controller.revision >= 0 ? panel.controller.value("contour.mix") : 1
            x: panel.theme.plateInset
            y: panel.theme.balanceY
            width: outputPlate.width - 2 * panel.theme.plateInset
            height: panel.theme.balanceHeight
            Caption {
                text: "DRY"
                color: balance.wet < 1 ? panel.theme.inkDim : panel.theme.inkFaint
            }
            Caption {
                anchors.right: parent.right
                text: "WET"
                color: panel.theme.accent
            }
            Rectangle {
                y: panel.theme.balanceBarY
                width: parent.width
                height: panel.theme.balanceBarHeight
                radius: panel.theme.radiusXs
                color: panel.theme.well
                border.color: panel.theme.wellEdge
                Rectangle {
                    x: panel.theme.hairline
                    y: panel.theme.hairline
                    width: (parent.width - 2 * panel.theme.hairline) * (1 - balance.wet)
                    height: parent.height - 2 * panel.theme.hairline
                    radius: panel.theme.barFillRadius
                    color: panel.theme.inkFaint
                }
                Rectangle {
                    x: panel.theme.hairline + (parent.width - 2 * panel.theme.hairline) * (1 - balance.wet)
                    y: panel.theme.hairline
                    width: (parent.width - 2 * panel.theme.hairline) * balance.wet
                    height: parent.height - 2 * panel.theme.hairline
                    radius: panel.theme.barFillRadius
                    color: panel.theme.accentDim
                }
            }
        }
        Caption {
            x: panel.theme.plateInset
            y: panel.theme.lowerRowY
            width: outputPlate.width - 2 * panel.theme.plateInset
            wrapMode: Text.WordWrap
            text: "DRY PATH PHASE-ALIGNED · 2× OVERSAMPLED"
        }
        Row {
            x: panel.theme.dialRowX
            y: panel.theme.dialRowY
            spacing: panel.theme.spaceLg
            Dial {
                parameterId: "contour.mix"
            }
            Dial {
                parameterId: "contour.output"
            }
        }
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
