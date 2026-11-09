pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Torque device panel: a matte black bass faceplate whose neon accent
// follows the model. Model tabs pick Acid, Moog, Reese or Sub; a display
// draws the model's filter and oscillator; the main row holds the bass
// controls (relabelled per model); a 16-step strip edits the built-in
// sequencer. Every control reads and writes through `controller`
// (NativeSynthDeviceController for the Torque engine); no project or audio
// state lives in QML.
Rectangle {
    id: panel
    required property var controller
    property TorqueTheme theme: TorqueTheme {
        accent: modelColors[panel.model]
    }

    // The current model's filter, as the filter view names it.
    readonly property string filterCaption: {
        const model = controller.revision >= 0 ? controller.choice("torque.model") : 0
        return model === 0 ? "DIODE LADDER · 24 dB"
            : model === 1 ? "TRANSISTOR LADDER · 24 dB"
            : model === 2 ? ((controller.revision >= 0 && controller.choice("torque.reese.filter") === 1)
                             ? "BAND-PASS 12 + COMB @ C2" : "LOW-PASS 24 + COMB @ C2")
            : "LOW-PASS 12"
    }
    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int model: controller.revision >= 0 ? controller.choice("torque.model") : 0
    readonly property int seqLength: controller.revision >= 0 ? Math.round(controller.value("torque.seq.length")) : 16
    readonly property bool sequencerOn: controller.revision >= 0 && controller.choice("torque.seq.on") === 1
    // Live engine state (ADR-0037), polled while the panel is visible: fast
    // while the synth publishes, slowly while it does not.
    property var live: ({ live: false })
    readonly property bool liveRunning: live.live === true && live.running === true
    readonly property int playingStep: liveRunning ? live.step : -1
    readonly property bool liveGate: liveRunning && live.gate === true

    objectName: "torquePanel"
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
    Accessible.name: "Torque bass synthesizer"

    // The main controls keep their ids across models; their names follow
    // what each model does with them.
    function mainLabel(id) {
        const m = model
        if (id === "torque.cutoff")
            return m === 3 ? "Tone" : "Cutoff"
        if (id === "torque.resonance")
            return m === 1 ? "Emphasis" : "Resonance"
        if (id === "torque.envmod")
            return m === 1 ? "Contour" : "Env Mod"
        if (id === "torque.drive")
            return m === 3 ? "Saturate" : "Drive"
        if (id === "torque.slide")
            return m === 0 ? "Slide" : "Glide"
        const info = controller.parameter(id)
        return info.name !== undefined ? info.name : ""
    }

    // A dark section with a title.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        property color titleColor: panel.theme.ink
        radius: panel.theme.radiusXl
        color: panel.theme.plate
        border.color: panel.theme.plateEdge
        Text {
            id: plateTitle
            x: panel.theme.spaceLg
            y: panel.theme.plateTitleY
            text: plate.title
            color: plate.titleColor
            font.pixelSize: panel.theme.typeSmall
            font.bold: true
            font.letterSpacing: panel.theme.plateTitleSpacing
        }
        Text {
            x: plateTitle.x + plateTitle.implicitWidth + panel.theme.spaceMd
            y: panel.theme.plateNoteY
            visible: plate.note !== ""
            text: plate.note
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
    }

    component Cell: Item {
        id: cell
        required property string parameterId
        property int diameter: panel.theme.cellDiameter
        property bool inactive: false
        property string inactiveReason: ""
        property string label: ""
        width: Math.max(diameter + panel.theme.cellWidthPad, panel.theme.cellMinWidth)
        height: diameter + panel.theme.cellHeightPad
        Kit.Knob {
            id: cellKnob
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: cell.parameterId
            diameter: cell.diameter
            inactive: cell.inactive
            inactiveReason: cell.inactiveReason
            label: cell.label !== "" ? cell.label : cellKnob.info.name !== undefined ? cellKnob.info.name : ""
        }
    }

    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    // ---- Header ---------------------------------------------------------------------

    Kit.DeviceHeader {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        controller: panel.controller
        theme: panel.theme
        menu: menu
        // The wordmark over a neon rule in the model's colour, the model's
        // tagline under it.
        brand: Component {
            Column {
                spacing: panel.theme.subtitleGap
                Text {
                    id: wordmark
                    objectName: "torqueWordmark"
                    text: "TORQUE"
                    color: panel.theme.wordmarkInk
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: panel.theme.wordmarkWeight
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Rectangle {
                    x: panel.theme.spaceXxs
                    width: wordmark.implicitWidth - panel.theme.wordmarkRuleInset
                    height: panel.theme.wordmarkRuleHeight
                    radius: height / 2
                    color: panel.theme.accent
                }
                Text {
                    objectName: "torqueSubtitle"
                    x: panel.theme.spaceXxs
                    text: "BASS · " + panel.theme.modelTaglines[panel.model]
                    color: panel.theme.accent
                    font.pixelSize: panel.theme.subtitleSize
                    font.bold: true
                    font.letterSpacing: panel.theme.subtitleSpacing
                }
            }
        }

        // The model tabs, in the header's second row.
        Caption {
            anchors.verticalCenter: tabs.verticalCenter
            text: "MODEL"
        }
        Row {
            id: tabs
            objectName: "torqueTabs:torque.model"
            spacing: panel.theme.spaceXs
            activeFocusOnTab: panel.controller.editable
            Accessible.role: Accessible.PageTabList
            Accessible.name: "Model"
            Accessible.description: panel.theme.modelNames[panel.model]
            function choose(index) {
                if (!panel.controller.editable)
                    return
                panel.controller.set_choice("torque.model", Math.max(0, Math.min(3, index)))
                panel.theme.lastTouched = "torque.model"
            }
            Keys.onLeftPressed: choose(panel.model - 1)
            Keys.onRightPressed: choose(panel.model + 1)
            Keys.onDeletePressed: {
                panel.controller.reset_to_default("torque.model")
                panel.theme.lastTouched = "torque.model"
            }
            Repeater {
                model: 4
                delegate: Rectangle {
                    id: tab
                    required property int index
                    readonly property bool on: index === panel.model
                    readonly property color tone: panel.theme.modelColors[index]
                    objectName: "torqueModel:" + index
                    width: panel.theme.modelTabWidth
                    height: panel.theme.modelTabHeight
                    radius: panel.theme.radiusLg
                    color: on ? Qt.rgba(tone.r, tone.g, tone.b, panel.theme.tabWashAlpha) : tabArea.containsMouse ? panel.theme.buttonHi : panel.theme.button
                    border.color: on ? tone : tabs.activeFocus && index === panel.model ? tone : panel.theme.wellEdge
                    border.width: on ? panel.theme.focusStroke : panel.theme.hairline
                    Accessible.role: Accessible.PageTab
                    Accessible.name: panel.theme.modelNames[index]
                    Accessible.checked: on
                    Rectangle {
                        x: panel.theme.modelLedX
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.modelLedSize
                        height: panel.theme.modelLedSize
                        radius: width / 2
                        color: tab.on ? tab.tone : panel.theme.ledOff
                    }
                    Text {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: panel.theme.modelTabTextOffset
                        text: panel.theme.modelNames[tab.index]
                        color: tab.on ? tab.tone : panel.theme.inkDim
                        font.pixelSize: panel.theme.typeLabel
                        font.weight: Font.Black
                        font.letterSpacing: panel.theme.modelTabLetterSpacing
                    }
                    MouseArea {
                        id: tabArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: panel.controller.editable
                        onClicked: {
                            tabs.forceActiveFocus()
                            tabs.choose(tab.index)
                        }
                    }
                }
            }
        }
    }

    // ---- Display and main controls ------------------------------------------------------

    readonly property real rowAY: header.y + header.height + theme.headerContentGap
    readonly property real rowAHeight: theme.rowAHeight

    Plate {
        id: screen
        title: "SCOPE"
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.scopePlateWidth
        height: panel.rowAHeight
        // Both views draw the settings ahead of the voice: PREVIEW. The
        // filter's level axis comes from the kit view's own mapping (its
        // response between the caption rows and the frequency labels).
        Kit.PlotFrame {
            id: filterFrame
            // The levels the axis labels, as [dB, label]; the first carries the unit.
            readonly property var levelMarks: [[0, "0"], [12, "+12"], [-24, "-24"]]
            readonly property real responseHeight: plotArea.height - panel.theme.filterViewTop
                - panel.theme.filterViewBottom
            x: panel.theme.scopeInset
            y: panel.theme.scopeTop
            width: panel.theme.filterViewWidth
            height: screen.height - panel.theme.scopeTop - panel.theme.scopeInset
            theme: panel.theme
            yUnit: "dB"
            yTicks: plotArea.height > 0 ? levelMarks.map(mark => ({
                at: 1 - (panel.theme.filterViewTop + (panel.theme.filterViewCeilingDb - mark[0])
                         / (panel.theme.filterViewCeilingDb - panel.theme.filterViewFloorDb)
                         * responseHeight) / plotArea.height,
                label: mark[1] })) : []
            Kit.FilterView {
                anchors.fill: parent
                cutoffId: "torque.cutoff"
                caption: panel.filterCaption
                controller: panel.controller
                theme: panel.theme
            }
        }
        Kit.PlotFrame {
            x: panel.theme.scopeInset + panel.theme.filterViewWidth + panel.theme.spaceMd
            y: panel.theme.scopeTop
            width: screen.width - x - panel.theme.scopeInset
            height: screen.height - panel.theme.scopeTop - panel.theme.scopeInset
            theme: panel.theme
            xTicks: oscillatorScope.timeTicks
            xUnit: oscillatorScope.timeUnit
            TorqueScopeView {
                id: oscillatorScope
                anchors.fill: parent
                controller: panel.controller
                theme: panel.theme
            }
        }
    }

    Plate {
        id: main
        title: panel.theme.modelNames[panel.model]
        titleColor: panel.theme.accent
        note: panel.theme.modelTaglines[panel.model]
        x: screen.x + screen.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight

        Row {
            id: bigRow
            x: (main.width - width) / 2
            y: panel.theme.plateContentY
            spacing: panel.theme.spaceSm
            Repeater {
                model: ["torque.cutoff", "torque.resonance", "torque.envmod", "torque.decay",
                        "torque.accent", "torque.drive", "torque.slide"]
                delegate: Cell {
                    required property string modelData
                    parameterId: modelData
                    diameter: panel.theme.mainKnobDiameter
                    width: panel.theme.mainCellWidth
                    label: panel.mainLabel(modelData)
                }
            }
        }
        Rectangle {
            x: panel.theme.spaceLg
            y: panel.theme.mainDividerY
            width: main.width - 2 * panel.theme.spaceLg
            height: panel.theme.hairline
            color: panel.theme.plateEdge
        }
        Caption {
            x: panel.theme.spaceLg
            y: panel.theme.mainCaptionY
            text: panel.theme.modelNames[panel.model] + " CONTROLS"
            color: panel.theme.accent
        }

        // Acid: waveform and the square's sag.
        Row {
            objectName: "torqueModelRow:0"
            visible: panel.model === 0
            x: panel.theme.plateContentX
            y: panel.theme.modelRowY
            spacing: panel.theme.modelRowSpacing
            readonly property bool square: panel.controller.revision >= 0 && panel.controller.choice("torque.acid.wave") === 1
            Column {
                spacing: panel.theme.spaceSm
                topPadding: panel.theme.segmentColumnPad
                Kit.Segment {
                    width: panel.theme.acidWaveWidth
                    height: panel.theme.largeSegmentHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "torque.acid.wave"
                    labels: ["SAW", "SQUARE"]
                }
                Caption { text: "WAVE" }
            }
            Cell {
                parameterId: "torque.acid.tilt"
                inactive: !parent.square
                inactiveReason: inactive ? "Sag applies to the Square wave" : ""
                label: "Sag"
            }
        }

        // Moog: three oscillators.
        Row {
            objectName: "torqueModelRow:1"
            visible: panel.model === 1
            x: panel.theme.plateContentX
            y: panel.theme.moogRowY
            spacing: panel.theme.moogRowSpacing
            Repeater {
                model: 3
                delegate: Column {
                    id: osc
                    required property int index
                    spacing: panel.theme.oscColumnSpacing
                    Caption { text: "OSC " + (osc.index + 1) }
                    Kit.Dropdown {
                        width: panel.theme.oscControlWidth
                        height: panel.theme.dropdownHeight
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: "torque.moog.wave" + (osc.index + 1)
                        menu: menu
                    }
                    Kit.Stepper {
                        visible: osc.index > 0
                        width: panel.theme.oscControlWidth
                        height: panel.theme.stepperHeight
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: "torque.moog.interval" + (osc.index + 1)
                    }
                }
            }
            Cell { parameterId: "torque.moog.level2"; label: "Level 2" }
            Cell { parameterId: "torque.moog.level3"; label: "Level 3" }
            Cell { parameterId: "torque.moog.detune" }
            Cell { parameterId: "torque.moog.noise" }
        }

        // Reese: the stack and its comb.
        Row {
            objectName: "torqueModelRow:2"
            visible: panel.model === 2
            x: panel.theme.plateContentX
            y: panel.theme.modelRowY
            spacing: panel.theme.reeseRowSpacing
            Column {
                spacing: panel.theme.spaceSm
                topPadding: panel.theme.spaceXs
                Kit.Stepper {
                    width: panel.theme.reeseVoicesWidth
                    height: panel.theme.tallStepperHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "torque.reese.voices"
                    plain: true
                }
                Caption { text: "VOICES" }
            }
            Column {
                spacing: panel.theme.spaceSm
                topPadding: panel.theme.spaceXs
                Kit.Segment {
                    width: panel.theme.reeseFilterWidth
                    height: panel.theme.compactSegmentHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "torque.reese.filter"
                }
                Caption { text: "FILTER" }
            }
            Cell { parameterId: "torque.reese.detune" }
            Cell { parameterId: "torque.reese.spread" }
            Cell { parameterId: "torque.reese.sub" }
            Cell { parameterId: "torque.reese.comb" }
        }

        // Sub/808: the drop and the layer.
        Row {
            objectName: "torqueModelRow:3"
            visible: panel.model === 3
            x: panel.theme.plateContentX
            y: panel.theme.modelRowY
            spacing: panel.theme.modelRowSpacing
            readonly property bool layered: panel.controller.revision >= 0 && panel.controller.choice("torque.sub.layer") > 0
            Cell { parameterId: "torque.sub.drop" }
            Cell { parameterId: "torque.sub.drop_time" }
            Column {
                spacing: panel.theme.spaceSm
                topPadding: panel.theme.segmentColumnPad
                Kit.Segment {
                    width: panel.theme.subLayerWidth
                    height: panel.theme.largeSegmentHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "torque.sub.layer"
                    labels: ["OFF", "FIFTH", "OCTAVE"]
                }
                Caption { text: "LAYER" }
            }
            Cell {
                parameterId: "torque.sub.layer_level"
                inactive: !parent.layered
                inactiveReason: inactive ? "Layer Level applies with a Layer on" : ""
                label: "Layer Level"
            }
        }
    }

    // ---- Envelopes, voice, LFO -------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: theme.rowBHeight

    Plate {
        id: envelopes
        title: "ENVELOPES"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.envelopesWidth
        height: panel.rowBHeight
        Caption {
            x: panel.theme.plateContentX
            y: panel.theme.plateContentY
            text: "FILTER"
        }
        Caption {
            x: panel.theme.ampCaptionX
            y: panel.theme.plateContentY
            text: "AMP"
        }
        Row {
            x: panel.theme.spaceSm
            y: panel.theme.envKnobY
            Cell { parameterId: "torque.env.attack" }
            Cell { parameterId: "torque.env.sustain" }
            Cell {
                parameterId: "torque.keytrack"
                label: "Key Track"
            }
        }
        Rectangle {
            x: panel.theme.envDividerX
            y: panel.theme.envDividerTop
            width: panel.theme.hairline
            height: envelopes.height - panel.theme.envDividerTop - panel.theme.spaceLg
            color: panel.theme.plateEdge
        }
        Row {
            x: panel.theme.ampRowX
            y: panel.theme.envKnobY
            Cell { parameterId: "torque.amp.attack" }
            Cell { parameterId: "torque.amp.decay" }
            Cell { parameterId: "torque.amp.sustain" }
            Cell { parameterId: "torque.amp.release" }
        }
    }

    Plate {
        id: voice
        title: "VOICE"
        x: envelopes.x + envelopes.width + panel.gap
        y: panel.rowBY
        width: panel.theme.voicePlateWidth
        height: panel.rowBHeight
        Grid {
            x: panel.theme.spaceLg
            y: panel.theme.voiceGridY
            columns: 3
            rowSpacing: panel.theme.voiceGridRowSpacing
            columnSpacing: panel.theme.spaceMd
            Kit.Stepper {
                width: panel.theme.voiceStepperWidth
                height: panel.theme.stepperHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.accent_threshold"
                label: "Accent At"
                plain: true
            }
            Kit.Stepper {
                width: panel.theme.voiceStepperWidth
                height: panel.theme.stepperHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.tune"
                label: "Tune"
            }
            Kit.Stepper {
                width: panel.theme.voiceStepperWidth
                height: panel.theme.stepperHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.bend"
                label: "Bend Range"
            }
        }
        Cell {
            x: panel.theme.fineKnobX
            y: panel.theme.plateContentY
            parameterId: "torque.fine"
            diameter: panel.theme.fineKnobDiameter
        }
        Caption {
            x: panel.theme.spaceLg
            y: voice.height - panel.theme.plateFootRise
            text: "ACCENT FROM VELOCITY · OVERLAP SLIDES"
        }
    }

    Plate {
        id: lfo
        title: "LFO"
        x: voice.x + voice.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        readonly property bool synced: panel.controller.revision >= 0 && panel.controller.choice("torque.lfo.sync") === 1
        Column {
            x: panel.theme.spaceLg
            y: panel.theme.plateControlY
            spacing: panel.theme.spaceSm
            Kit.Dropdown {
                width: panel.theme.lfoControlWidth
                height: panel.theme.dropdownHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.lfo.shape"
                menu: menu
            }
            Row {
                spacing: panel.theme.spaceXs
                Kit.Segment {
                    width: panel.theme.lfoControlWidth
                    height: panel.theme.segmentHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "torque.lfo.sync"
                    labels: ["FREE", "SYNC"]
                }
            }
            Kit.Dropdown {
                width: panel.theme.lfoControlWidth
                height: panel.theme.dropdownHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.lfo.division"
                menu: menu
                inactive: !lfo.synced
                inactiveReason: inactive ? "Division applies with Sync on" : ""
            }
        }
        Row {
            x: panel.theme.lfoKnobsX + (lfo.width - panel.theme.lfoKnobsX - panel.theme.spaceLg - width) / 2
            y: panel.theme.lfoKnobsY
            Cell {
                parameterId: "torque.lfo.rate"
                inactive: lfo.synced
                inactiveReason: inactive ? "Rate follows Sync" : ""
            }
            Cell {
                parameterId: "torque.lfo.cutoff"
                label: "To Cutoff"
            }
            Cell {
                parameterId: "torque.lfo.pitch"
                label: "To Pitch"
            }
            Cell { parameterId: "torque.lfo.movement" }
        }
    }

    // ---- Effects and output --------------------------------------------------------------------

    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: theme.rowCHeight

    Plate {
        id: effects
        title: "EFFECTS"
        x: panel.margin
        y: panel.rowCY
        width: panel.theme.effectsWidth
        height: panel.rowCHeight
        readonly property bool synced: panel.controller.revision >= 0 && panel.controller.choice("torque.fx.delay_sync") === 1
        readonly property bool distorted: panel.controller.revision >= 0 && panel.controller.value("torque.fx.dist_mix") > 0
        Column {
            x: panel.theme.spaceLg
            y: panel.theme.plateControlY
            spacing: panel.theme.spaceSm
            Kit.Segment {
                width: panel.theme.distModeWidth
                height: panel.theme.compactSegmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.fx.dist_mode"
                labels: ["TUBE", "FUZZ", "CRUSH"]
            }
            Caption { text: "DISTORTION" }
        }
        Row {
            x: panel.theme.distKnobsX
            y: panel.theme.plateContentY
            Cell {
                parameterId: "torque.fx.dist_drive"
                label: "Drive"
                inactive: !effects.distorted
                inactiveReason: inactive ? "Drive applies with Mix above 0" : ""
            }
            Cell {
                parameterId: "torque.fx.dist_tone"
                inactive: !effects.distorted
                inactiveReason: inactive ? "Tone applies with Mix above 0" : ""
            }
            Cell {
                parameterId: "torque.fx.dist_mix"
                label: "Mix"
            }
        }
        Rectangle {
            x: panel.theme.chorusDividerX
            y: panel.theme.plateContentY
            width: panel.theme.hairline
            height: effects.height - panel.theme.plateContentY - panel.theme.spaceLg
            color: panel.theme.plateEdge
        }
        Column {
            x: panel.theme.chorusX
            y: panel.theme.plateControlY
            spacing: panel.theme.spaceSm
            Kit.Segment {
                width: panel.theme.chorusWidth
                height: panel.theme.compactSegmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.fx.chorus"
                labels: ["OFF", "I", "II", "I+II"]
            }
            Caption { text: "CHORUS" }
        }
        Rectangle {
            x: panel.theme.delayDividerX
            y: panel.theme.plateContentY
            width: panel.theme.hairline
            height: effects.height - panel.theme.plateContentY - panel.theme.spaceLg
            color: panel.theme.plateEdge
        }
        Column {
            x: panel.theme.delayX
            y: panel.theme.plateControlY
            spacing: panel.theme.spaceSm
            Kit.Segment {
                width: panel.theme.delayControlWidth
                height: panel.theme.compactSegmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.fx.delay_sync"
                labels: ["FREE", "SYNC"]
            }
            Kit.Dropdown {
                width: panel.theme.delayControlWidth
                height: panel.theme.dropdownHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.fx.delay_division"
                menu: menu
                inactive: !effects.synced
                inactiveReason: inactive ? "Division applies with Sync on" : ""
            }
        }
        Row {
            x: panel.theme.delayKnobsX
            y: panel.theme.plateContentY
            Cell {
                parameterId: "torque.fx.delay"
                label: "Delay"
            }
            Cell {
                parameterId: "torque.fx.delay_time"
                inactive: effects.synced
                inactiveReason: inactive ? "Time follows Sync" : ""
            }
            Cell {
                parameterId: "torque.fx.delay_feedback"
                label: "Repeats"
            }
        }
    }

    Plate {
        id: output
        title: "PLAY · OUT"
        x: effects.x + effects.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        Row {
            x: (output.width - width) / 2
            y: panel.theme.plateContentY
            spacing: panel.theme.outputSpacing
            Cell {
                parameterId: "torque.perf.bend"
                label: "Pitch Bend"
            }
            Cell {
                parameterId: "torque.perf.wheel"
                label: "Mod Wheel"
            }
            Rectangle {
                width: panel.theme.hairline
                height: panel.theme.outputDividerHeight
                color: panel.theme.plateEdge
            }
            Cell {
                parameterId: "torque.out.level"
                diameter: panel.theme.outputKnobDiameter
                label: "Output"
            }
        }
    }

    // ---- Sequencer -----------------------------------------------------------------------------

    readonly property real rowDY: rowCY + rowCHeight + gap

    Plate {
        id: sequencer
        objectName: "torqueSequencer"
        title: "SEQUENCER"
        note: "RUNS WHILE A NOTE IS HELD · ROOTED ON IT"
        titleColor: panel.sequencerOn ? panel.theme.accent : panel.theme.ink
        x: panel.margin
        y: panel.rowDY
        width: panel.width - 2 * panel.margin
        height: panel.height - y - panel.theme.spaceLg

        Column {
            x: panel.theme.spaceLg
            y: panel.theme.plateControlY
            spacing: panel.theme.spaceMd
            Kit.Segment {
                width: panel.theme.seqOnWidth
                height: panel.theme.largeSegmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "torque.seq.on"
                labels: ["OFF", "ON"]
            }
            Row {
                spacing: panel.theme.spaceSm
                Kit.Stepper {
                    width: panel.theme.seqLengthWidth
                    height: panel.theme.stepperHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "torque.seq.length"
                    label: "Length"
                    plain: true
                }
                Kit.Dropdown {
                    width: panel.theme.seqRateWidth
                    height: panel.theme.dropdownHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "torque.seq.rate"
                    menu: menu
                }
            }
            Row {
                topPadding: panel.theme.seqKnobsPad
                Cell {
                    parameterId: "torque.seq.swing"
                    diameter: panel.theme.seqKnobDiameter
                    width: panel.theme.seqCellWidth
                }
                Cell {
                    parameterId: "torque.seq.gate"
                    diameter: panel.theme.seqKnobDiameter
                    width: panel.theme.seqCellWidth
                }
            }
        }

        // The steps are one Tab stop: the focused step is the cursor (its
        // edge lit), Left/Right move it along the strip.
        FocusScope {
            id: stepStrip
            objectName: "torqueSteps"
            readonly property int stepCount: 16
            x: panel.theme.stepsX
            y: panel.theme.plateContentY
            width: sequencer.width - x - panel.theme.spaceLg
            height: sequencer.height - panel.theme.plateContentY - panel.theme.spaceLg
            activeFocusOnTab: panel.controller.editable
            opacity: panel.sequencerOn ? 1 : panel.theme.seqOffOpacity
            Accessible.role: Accessible.List
            Accessible.name: "Sequencer steps"

            // Moves the cursor to step `index` (from 0), kept on the strip.
            function moveTo(index) {
                const next = stepRepeater.itemAt(Math.max(0, Math.min(stepCount - 1, index)))
                if (next)
                    next.forceActiveFocus()
            }

            Row {
                id: steps
                readonly property real cellWidth: (width - (stepStrip.stepCount - 1) * spacing) / stepStrip.stepCount
                anchors.fill: parent
                spacing: panel.theme.stepSpacing
                Repeater {
                    id: stepRepeater
                    model: stepStrip.stepCount
                    delegate: TorqueStepCell {
                        required property int index
                        width: steps.cellWidth
                        height: steps.height
                        // The cursor starts on the first step.
                        focus: index === 0
                        controller: panel.controller
                        theme: panel.theme
                        step: index + 1
                        inPattern: index < panel.seqLength
                        playing: panel.playingStep === index
                        gateOpen: panel.liveGate
                        onMoveRequested: delta => stepStrip.moveTo(index + delta)
                    }
                }
            }
        }
    }

    Timer {
        objectName: "torqueTelemetryPoll"
        interval: panel.live.live === true ? 33 : 250
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
