pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Ember device panel: a warm vintage poly between walnut cheeks. Every
// control reads and writes through `controller` (NativeSynthDeviceController
// for the Ember engine); no project or audio state lives in QML.
Rectangle {
    id: panel
    required property var controller
    property EmberTheme theme: EmberTheme {}

    readonly property real cheek: theme.cheekWidth
    readonly property real margin: theme.panelMargin
    readonly property real gap: theme.spaceMd
    readonly property real originX: cheek + margin
    readonly property real inner: width - 2 * originX
    readonly property real headerHeight: theme.headerHeight
    readonly property real row1Y: margin + headerHeight + theme.spaceMd
    readonly property real row1Height: theme.row1Height
    readonly property real row2Y: row1Y + row1Height + gap
    readonly property real row2Height: theme.row2Height

    // Conditional controls follow the mode parameters.
    readonly property int filterType: controller.revision >= 0 ? controller.choice("ember.filter.type") : 0
    // Choice indices of the mode parameters.
    readonly property int semFilter: 1
    readonly property int unisonMode: 1
    readonly property int chordMode: 2
    // The amp mode whose own ADSR shapes the sound.
    readonly property int ampOwnMode: 2
    readonly property int ampMode: controller.revision >= 0 ? controller.choice("ember.amp.mode") : ampOwnMode
    readonly property int voiceMode: controller.revision >= 0 ? controller.choice("ember.voice.mode") : 0
    readonly property bool lfoSynced: controller.revision >= 0 && controller.choice("ember.lfo.sync") === 1
    readonly property bool delaySynced: controller.revision >= 0 && controller.choice("ember.fx.delay_sync") === 1
    // The engine's telemetry (ember.hpp): the voice cards' pitch errors.
    property var live: ({})
    // Telemetry poll intervals while notes sound and while idle.
    readonly property int livePollMs: 33
    readonly property int idlePollMs: 250

    objectName: "emberPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: row2Y + row2Height + margin
    radius: theme.radiusXl
    color: theme.canvas
    Accessible.role: Accessible.Pane
    Accessible.name: "Ember synthesizer"


    EmberWood {
        theme: panel.theme
        x: 0
        y: 0
        width: panel.cheek
        height: panel.height
        leftSide: true
    }
    EmberWood {
        theme: panel.theme
        x: panel.width - panel.cheek
        y: 0
        width: panel.cheek
        height: panel.height
        leftSide: false
    }

    // ---- Header ---------------------------------------------------------

    Item {
        id: header
        x: panel.originX
        y: panel.margin
        width: panel.inner
        height: panel.headerHeight

        Canvas {
            id: flame
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            width: panel.theme.flameWidth
            height: panel.theme.flameHeight
            // The flame outline in its box: a start point, then cubic Bezier
            // segments as [control 1, control 2, end] coordinates.
            readonly property var outlineStart: [13, 1]
            readonly property var outline: [[20, 10, 25, 16, 22, 24], [20, 30, 6, 31, 4, 23],
                                             [2, 17, 8, 14, 9, 8], [11, 12, 12, 14, 14, 15],
                                             [15, 10, 14, 5, 13, 1]]
            // The hollow at the flame's heart: centre x, y and radii.
            readonly property var hollow: [10, 20, 7, 8]
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const glow = ctx.createLinearGradient(0, height, 0, 0)
                glow.addColorStop(0, panel.theme.ember)
                glow.addColorStop(1, panel.theme.gold)
                ctx.fillStyle = glow
                ctx.beginPath()
                ctx.moveTo(outlineStart[0], outlineStart[1])
                for (const segment of outline)
                    ctx.bezierCurveTo(segment[0], segment[1], segment[2], segment[3], segment[4], segment[5])
                ctx.fill()
                ctx.fillStyle = panel.theme.canvas
                ctx.beginPath()
                ctx.ellipse(hollow[0], hollow[1], hollow[2], hollow[3])
                ctx.fill()
            }
        }
        Text {
            id: wordmark
            x: flame.width + panel.theme.wordmarkGap
            y: panel.theme.wordmarkTop
            text: "EMBER"
            color: panel.theme.cream
            font.pixelSize: panel.theme.wordmarkSize
            font.bold: true
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            x: wordmark.x + 1
            y: wordmark.y + wordmark.height - 1
            text: "VINTAGE POLY SYNTHESIZER"
            color: panel.theme.amber
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }

        Row {
            id: browser
            anchors.centerIn: parent
            spacing: panel.theme.spaceSm
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "emberPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: presetBox
                objectName: "emberPresetBox"
                width: panel.theme.presetBoxWidth
                height: panel.theme.presetBoxHeight
                radius: panel.theme.radiusSm
                color: panel.theme.engraving
                border.color: activeFocus ? panel.theme.amber : panel.theme.line
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", modified" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(presetBox, panel.controller, panel.theme.amber)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                // A small amber "display" look, like a patch readout.
                Text {
                    x: panel.theme.spaceMd
                    anchors.verticalCenter: parent.verticalCenter
                    text: panel.controller.preset_category.toUpperCase()
                    color: panel.theme.amber
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                    font.letterSpacing: panel.theme.presetCategorySpacing
                }
                Text {
                    objectName: "emberPresetName"
                    anchors.centerIn: parent
                    width: parent.width - panel.theme.presetNameReserve
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.controller.preset_name
                    color: panel.theme.gold
                    font.pixelSize: panel.theme.typeSubtitle
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.presetEditedRight
                    anchors.verticalCenter: parent.verticalCenter
                    visible: panel.controller.preset_modified
                    text: "EDITED"
                    color: panel.theme.dim
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.spaceMd
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.dim
                    font.pixelSize: panel.theme.typeBody
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: panel.controller.editable
                    onClicked: {
                        presetBox.forceActiveFocus()
                        presetBox.openPresets()
                    }
                }
            }
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "emberPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "emberStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.spaceXs
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.spaceXxs
            y: status.y + status.height + panel.theme.noteHintGap
            spacing: panel.theme.spaceXs
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.noteHintMarkSize
                height: panel.theme.noteHintMarkSize
                rotation: panel.theme.noteHintMarkRotation
                color: "transparent"
                border.color: panel.theme.dim
            }
            Text {
                text: "Applies to new notes"
                color: panel.theme.faint
                font.pixelSize: panel.theme.typeCaption
            }
        }
    }

    // ---- Row 1: DCO | VCO B | POLY-MOD | FILTER | ENV | AMP -------------

    EmberSection {
        id: dco
        theme: panel.theme
        title: "DCO"
        accent: panel.theme.amber
        x: panel.originX
        y: panel.row1Y
        width: panel.theme.dcoWidth
        height: panel.row1Height
        trailing: EmberButtons {
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.dco.range"
            accent: panel.theme.amber
            width: panel.theme.dcoRangeWidth
            height: panel.theme.stripControlHeight
        }
        Row {
            y: panel.theme.spaceXs
            spacing: 0
            Repeater {
                model: ["ember.dco.saw", "ember.dco.pulse", "ember.dco.sub", "ember.dco.noise",
                        "ember.dco.pw", "ember.dco.pwm"]
                delegate: Kit.Fader {
                    required property string modelData
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData
                    accent: panel.theme.amber
                    width: panel.theme.dcoFaderWidth
                    height: panel.theme.tallFaderHeight
                }
            }
        }
        Text {
            x: 0
            y: panel.theme.dcoPwmCaptionY
            text: "PWM"
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        EmberButtons {
            x: panel.theme.dcoPwmButtonsX
            y: panel.theme.footRowY
            width: parent.width - panel.theme.dcoPwmButtonsX
            height: panel.theme.buttonRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.dco.pwm_source"
            labels: ["Man", "LFO", "Env"]
        }
    }

    EmberSection {
        id: vco
        theme: panel.theme
        title: "VCO B"
        accent: panel.theme.gold
        x: dco.x + dco.width + panel.gap
        y: panel.row1Y
        width: panel.theme.vcoWidth
        height: panel.row1Height
        EmberButtons {
            y: 0
            width: parent.width
            height: panel.theme.buttonRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.vco.wave"
            accent: panel.theme.gold
            labels: ["Saw", "Pulse", "Tri"]
        }
        Row {
            y: panel.theme.belowButtonsY
            Repeater {
                model: ["ember.vco.level", "ember.vco.semi", "ember.vco.fine", "ember.vco.pw"]
                delegate: Kit.Fader {
                    required property string modelData
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData
                    accent: panel.theme.gold
                    width: panel.theme.wideFaderWidth
                    height: panel.theme.vcoFaderHeight
                }
            }
        }
        Row {
            y: panel.theme.footRowY
            spacing: panel.theme.spaceSm
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.vco.sync"
                accent: panel.theme.gold
                height: panel.theme.buttonRowHeight
            }
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.vco.low"
                accent: panel.theme.gold
                height: panel.theme.buttonRowHeight
            }
        }
    }

    EmberSection {
        id: polymod
        theme: panel.theme
        title: "Poly-Mod"
        accent: panel.theme.ember
        x: vco.x + vco.width + panel.gap
        y: panel.row1Y
        width: panel.theme.polyModWidth
        height: panel.row1Height
        Text {
            x: panel.theme.spaceXs
            y: 0
            text: "SOURCES"
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        Row {
            y: panel.theme.belowCaptionY
            Repeater {
                model: ["ember.pm.env", "ember.pm.osc"]
                delegate: Kit.Fader {
                    required property string modelData
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData
                    accent: panel.theme.ember
                    label: modelData === "ember.pm.env" ? "F. Env" : "VCO B"
                    width: panel.theme.wideFaderWidth
                    height: panel.theme.polyFaderHeight
                }
            }
        }
        Text {
            x: panel.theme.polyTargetCaptionX
            y: 0
            text: "TO"
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        Column {
            x: panel.theme.polyTargetX
            y: panel.theme.belowCaptionY
            spacing: panel.theme.spaceMd
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.pm.freq"
                accent: panel.theme.ember
                label: "Freq A"
                width: panel.theme.polyTargetWidth
            }
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.pm.pw"
                accent: panel.theme.ember
                label: "Width"
                width: panel.theme.polyTargetWidth
            }
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.pm.cutoff"
                accent: panel.theme.ember
                label: "Filter"
                width: panel.theme.polyTargetWidth
            }
        }
        Text {
            x: 0
            y: panel.theme.polyNoteY
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Filter envelope and VCO B modulate the DCO and filter."
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
        }
    }

    EmberSection {
        id: filter
        theme: panel.theme
        title: "HPF · VCF"
        accent: panel.theme.amber
        x: polymod.x + polymod.width + panel.gap
        y: panel.row1Y
        width: panel.theme.filterWidth
        height: panel.row1Height
        trailing: EmberButtons {
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.filter.type"
            accent: panel.theme.amber
            width: panel.theme.filterTypeWidth
            height: panel.theme.stripControlHeight
        }
        Kit.FilterScope {
            objectName: "emberFilterView"
            controller: panel.controller
            theme: panel.theme
            x: 0
            y: 0
            width: parent.width
            height: panel.theme.filterScopeHeight
        }
        Row {
            y: panel.theme.filterFaderY
            Kit.Fader {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.hpf"
                accent: panel.theme.teal
                width: panel.theme.faderWidth
                height: panel.theme.filterFaderHeight
            }
            Rectangle {
                width: panel.theme.hairline
                height: panel.theme.filterDividerHeight
                color: panel.theme.line
            }
            Repeater {
                model: ["ember.filter.cutoff", "ember.filter.resonance", "ember.filter.morph",
                        "ember.filter.env", "ember.filter.lfo", "ember.filter.keytrack",
                        "ember.filter.velocity", "ember.filter.drive"]
                delegate: Kit.Fader {
                    required property string modelData
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData
                    accent: panel.theme.amber
                    label: modelData === "ember.filter.resonance" ? "Res"
                        : modelData === "ember.filter.velocity" ? "Vel"
                        : modelData === "ember.filter.keytrack" ? "Key"
                        : panel.controller.parameter(modelData).name
                    // Morph only applies to the SEM filter.
                    visible: modelData !== "ember.filter.morph" || panel.filterType === panel.semFilter
                    width: panel.theme.filterFaderWidth
                    height: panel.theme.filterFaderHeight
                }
            }
        }
    }

    EmberSection {
        id: env
        theme: panel.theme
        title: "Env"
        accent: panel.theme.cream
        x: filter.x + filter.width + panel.gap
        y: panel.row1Y
        width: panel.theme.envWidth
        height: panel.row1Height
        Kit.EnvelopeView {
            controller: panel.controller
            theme: panel.theme
            prefix: "ember.env"
            accent: panel.theme.cream
            width: parent.width
            height: panel.theme.envViewHeight
        }
        Row {
            y: panel.theme.envFaderY
            Repeater {
                model: ["ember.env.attack", "ember.env.decay", "ember.env.sustain", "ember.env.release"]
                delegate: Kit.Fader {
                    required property string modelData
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData
                    accent: panel.theme.cream
                    label: modelData.charAt(10).toUpperCase()
                    width: panel.theme.envFaderWidth
                    height: panel.theme.envFaderHeight
                }
            }
        }
    }

    EmberSection {
        id: amp
        theme: panel.theme
        title: "Amp"
        accent: panel.theme.teal
        x: env.x + env.width + panel.gap
        y: panel.row1Y
        width: panel.originX + panel.inner - x
        height: panel.row1Height
        EmberButtons {
            width: parent.width
            height: panel.theme.buttonRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.amp.mode"
            accent: panel.theme.teal
        }
        Row {
            y: panel.theme.belowButtonsY
            Repeater {
                id: ampFaders
                model: ["ember.amp.attack", "ember.amp.decay", "ember.amp.sustain", "ember.amp.release",
                        "ember.amp.velocity"]
                delegate: Kit.Fader {
                    required property string modelData
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData
                    accent: panel.theme.teal
                    label: modelData === "ember.amp.velocity" ? "Vel" : modelData.charAt(10).toUpperCase()
                    // The amp's own ADSR only shapes sound in Own mode.
                    active: modelData === "ember.amp.velocity" || panel.ampMode === panel.ampOwnMode
                    width: amp.bodyWidth / ampFaders.count
                    height: panel.theme.tallFaderHeight
                }
            }
        }
    }

    // ---- Row 2: LFO | VOICE | AGE | EFFECTS | OUTPUT --------------------

    EmberSection {
        id: lfo
        theme: panel.theme
        title: "LFO"
        accent: panel.theme.gold
        x: panel.originX
        y: panel.row2Y
        width: panel.theme.lfoWidth
        height: panel.row2Height
        trailing: Kit.Switch {
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.lfo.sync"
            accent: panel.theme.gold
            height: panel.theme.stripControlHeight
        }
        EmberButtons {
            width: parent.width
            height: panel.theme.buttonRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.lfo.shape"
            accent: panel.theme.gold
            labels: ["Tri", "Sqr", "S&H", "Saw"]
        }
        EmberLfoView {
            controller: panel.controller
            theme: panel.theme
            accent: panel.theme.gold
            y: panel.theme.lfoViewY
            width: parent.width
            height: panel.theme.displayWellHeight
        }
        Kit.Knob {
            id: lfoRate
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.lfo.rate"
            accent: panel.theme.gold
            visible: !panel.lfoSynced
            x: 0
            y: panel.theme.lfoKnobY
        }
        Item {
            x: 0
            y: panel.theme.lfoKnobY
            width: lfoRate.implicitWidth + panel.theme.spaceSm
            height: lfoRate.implicitHeight
            visible: panel.lfoSynced
            Kit.Dropdown {
                controller: panel.controller
                theme: panel.theme
                menu: menu
                parameterId: "ember.lfo.division"
                accent: panel.theme.gold
                y: panel.theme.divisionDropdownY
                width: parent.width
            }
            Text {
                y: panel.theme.divisionCaptionY
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "Division"
                color: panel.theme.dim
                font.pixelSize: panel.theme.typeCaption
                font.bold: true
            }
        }
        Kit.Knob {
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.lfo.delay"
            accent: panel.theme.gold
            x: panel.theme.lfoKnobPitch
            y: panel.theme.lfoKnobY
        }
        Kit.Knob {
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.lfo.pitch"
            accent: panel.theme.gold
            x: 2 * panel.theme.lfoKnobPitch
            y: panel.theme.lfoKnobY
        }
        Text {
            y: panel.theme.lfoNoteY
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Pitch depth rises with the mod wheel. Filter depth is on the VCF."
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
        }
    }

    EmberSection {
        id: voice
        theme: panel.theme
        title: "Voice"
        accent: panel.theme.teal
        x: lfo.x + lfo.width + panel.gap
        y: panel.row2Y
        width: panel.theme.voiceWidth
        height: panel.row2Height
        EmberButtons {
            width: panel.theme.voiceModeWidth
            height: panel.theme.buttonRowHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.voice.mode"
            accent: panel.theme.teal
        }
        Kit.Dropdown {
            controller: panel.controller
            theme: panel.theme
            menu: menu
            parameterId: "ember.voice.chord"
            accent: panel.theme.teal
            visible: panel.voiceMode === panel.chordMode
            x: panel.theme.voiceChordX
            width: parent.width - panel.theme.voiceChordX
        }
        Row {
            y: panel.theme.belowButtonsY
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.voice.detune"
                accent: panel.theme.teal
                label: "Detune"
                active: panel.voiceMode === panel.unisonMode
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.voice.glide"
                accent: panel.theme.teal
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.voice.bend"
                accent: panel.theme.teal
                label: "Bend"
            }
        }
        Text {
            y: panel.theme.perfCaptionY
            text: "PERFORMANCE"
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        Row {
            y: panel.theme.perfWheelsY
            x: panel.theme.spaceXs
            spacing: panel.theme.wheelSpacing
            EmberWheel {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.perf.bend"
                label: "Bend"
                spring: true
                height: panel.theme.wheelHeight
            }
            EmberWheel {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.perf.wheel"
                label: "Mod"
                height: panel.theme.wheelHeight
            }
            EmberWheel {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.perf.pressure"
                label: "Press"
                height: panel.theme.wheelHeight
            }
        }
        Text {
            x: panel.theme.voiceNoteX
            y: panel.theme.voiceNoteY
            width: parent.width - panel.theme.voiceNoteX
            wrapMode: Text.WordWrap
            text: panel.voiceMode === panel.unisonMode ? "Unison stacks all eight voice cards, spread in stereo."
                : panel.voiceMode === panel.chordMode ? "Each key plays the chosen chord shape."
                : "Eight voices, round-robin across the voice cards."
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
        }
    }

    EmberSection {
        id: age
        theme: panel.theme
        title: "Circuit"
        accent: panel.theme.ember
        x: voice.x + voice.width + panel.gap
        y: panel.row2Y
        width: panel.theme.circuitWidth
        height: panel.row2Height
        // Wear, calibration and leakage: how this unit falls short of its
        // schematic (ADR-0066).
        Row {
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.age"
                accent: panel.theme.ember
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.circuit.tuning"
                accent: panel.theme.ember
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.circuit.bleed"
                accent: panel.theme.ember
            }
        }
        Row {
            id: humRow
            y: panel.theme.humRowY
            width: parent.width
            spacing: panel.theme.spaceSm
            Kit.Knob {
                id: humKnob
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.circuit.hum"
                accent: panel.theme.ember
            }
            Column {
                width: humRow.width - humKnob.width - humRow.spacing
                spacing: panel.theme.spaceSm
                EmberButtons {
                    width: parent.width
                    height: panel.theme.buttonRowHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "ember.circuit.mains"
                    accent: panel.theme.ember
                }
                Kit.Stepper {
                    width: parent.width
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "ember.circuit.unit"
                    accent: panel.theme.ember
                    prefix: "Unit "
                    plain: true
                }
            }
        }
        EmberCircuitView {
            objectName: "emberCircuitView"
            theme: panel.theme
            live: panel.live
            y: panel.theme.circuitViewY
            width: parent.width
            // The well gives up height so the note below it stays whole.
            height: Math.min(panel.theme.displayWellHeight,
                             circuitNote.y - panel.theme.circuitViewY - panel.theme.spaceSm)
        }
        Text {
            id: circuitNote
            objectName: "emberCircuitNote"
            // At the card's foot, however many lines it wraps to.
            y: parent.height - implicitHeight
            width: parent.width
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: "Each card's VCO B (bars) and crystal DCO A (ticks) against perfect pitch."
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
        }
    }

    EmberSection {
        id: effects
        theme: panel.theme
        title: "Effects"
        accent: panel.theme.amber
        x: age.x + age.width + panel.gap
        y: panel.row2Y
        width: panel.theme.effectsWidth
        height: panel.row2Height

        component Caption: Text {
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
        }
        Caption { x: panel.theme.spaceXxs; y: 0; text: "ENSEMBLE" }
        Kit.Knob {
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.fx.ensemble"
            label: "Amount"
            y: panel.theme.belowCaptionY
        }
        Caption { x: panel.theme.fxChorusX; y: 0; text: "CHORUS" }
        EmberButtons {
            x: panel.theme.fxChorusX
            y: panel.theme.fxChorusY
            width: panel.theme.fxChorusWidth
            height: panel.theme.fxChorusHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.fx.chorus"
        }
        Kit.Knob {
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.fx.hiss"
            label: "Hiss"
            x: panel.theme.fxHissX
            y: panel.theme.belowCaptionY
        }
        Rectangle {
            y: panel.theme.fxDividerY
            width: parent.width
            height: panel.theme.hairline
            color: panel.theme.line
        }
        Caption { x: panel.theme.spaceXxs; y: panel.theme.fxDelayCaptionY; text: "BBD DELAY" }
        Kit.Switch {
            x: panel.theme.fxSyncX
            y: panel.theme.fxSwitchY
            height: panel.theme.stripControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.fx.delay_sync"
        }
        Kit.Switch {
            id: pingPong
            x: panel.theme.fxPingPongX
            y: panel.theme.fxSwitchY
            height: panel.theme.stripControlHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.fx.delay_pingpong"
        }
        Row {
            y: panel.theme.fxKnobY
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.fx.delay"
                label: "Mix"
            }
            Item {
                width: delayTime.implicitWidth
                height: delayTime.implicitHeight
                Kit.Knob {
                    id: delayTime
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "ember.fx.delay_time"
                    visible: !panel.delaySynced
                }
                Item {
                    anchors.fill: parent
                    visible: panel.delaySynced
                    Kit.Dropdown {
                        controller: panel.controller
                        theme: panel.theme
                        menu: menu
                        parameterId: "ember.fx.delay_division"
                        x: panel.theme.hairline
                        y: panel.theme.divisionDropdownY
                        width: parent.width - 2 * panel.theme.hairline
                    }
                    Text {
                        y: panel.theme.divisionCaptionY
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: "Division"
                        color: panel.theme.dim
                        font.pixelSize: panel.theme.typeCaption
                        font.bold: true
                    }
                }
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.fx.delay_feedback"
            }
        }
        Rectangle {
            x: panel.theme.fxTapeDividerX
            y: panel.theme.fxTapeDividerY
            width: panel.theme.hairline
            height: panel.theme.fxTapeDividerHeight
            color: panel.theme.line
        }
        Caption { x: panel.theme.fxTapeX; y: panel.theme.fxDelayCaptionY; text: "TAPE" }
        Row {
            x: panel.theme.fxTapeX
            y: panel.theme.fxKnobY
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.fx.tape"
                label: "Drive"
            }
            Kit.Knob {
                controller: panel.controller
                theme: panel.theme
                parameterId: "ember.fx.wobble"
                label: "Wow"
            }
        }
        Text {
            y: panel.theme.fxNoteY
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Signal flow: ensemble → chorus → delay → tape. Ping-pong repeats darken like a bucket brigade."
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
        }
    }

    EmberSection {
        id: output
        theme: panel.theme
        title: "Out"
        accent: panel.theme.cream
        x: effects.x + effects.width + panel.gap
        y: panel.row2Y
        width: panel.originX + panel.inner - x
        height: panel.row2Height
        Kit.Knob {
            controller: panel.controller
            theme: panel.theme
            parameterId: "ember.out.level"
            accent: panel.theme.cream
            label: "Level"
            diameter: panel.theme.outKnobDiameter
            x: (parent.width - width) / 2
            y: panel.theme.spaceSm
        }
        // A maker's badge: engine name, voice count and a power lamp.
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: panel.theme.spaceXs
            spacing: panel.theme.spaceSm
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: panel.theme.lampSize
                height: panel.theme.lampSize
                radius: width / 2
                color: panel.theme.ember
                border.color: panel.theme.lampRim
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "8 VOICE"
                color: panel.theme.dim
                font.pixelSize: panel.theme.typeMicro
                font.bold: true
                font.letterSpacing: panel.theme.badgeVoiceSpacing
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: panel.theme.badgeWidth
                height: panel.theme.badgeHeight
                radius: panel.theme.radiusXs
                color: panel.theme.engraving
                border.color: panel.theme.gold
                Text {
                    anchors.centerIn: parent
                    text: "EMBER"
                    color: panel.theme.gold
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                    font.letterSpacing: panel.theme.badgeNameSpacing
                }
            }
        }
    }

    Timer {
        objectName: "emberTelemetryPoll"
        interval: panel.live.live === true ? panel.livePollMs : panel.idlePollMs
        repeat: true
        running: panel.visible
        onTriggered: panel.live = panel.controller.telemetry()
    }

    Kit.Menu {
        id: menu
        theme: panel.theme
        anchors.fill: parent
    }
}
