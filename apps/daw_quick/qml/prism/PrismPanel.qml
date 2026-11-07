pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// The Prism device panel. Everything here reads and writes through
// `controller` (NativeSynthDeviceController for the Prism engine); no project or audio state lives in QML.
Rectangle {
    id: panel
    required property var controller
    property PrismTheme theme: PrismTheme {}

    readonly property real margin: theme.spaceLg
    readonly property real gap: theme.sectionGap
    readonly property real inner: width - 2 * margin
    readonly property real headerHeight: theme.headerHeight
    readonly property real row1Height: theme.sourceRowHeight
    readonly property real row2Height: theme.shapeRowHeight
    readonly property real row3Height: theme.playRowHeight
    // Sections per row, and each section's share of a row's width less the
    // gaps between them.
    readonly property int columns: 4
    readonly property real columnsWidth: inner - (columns - 1) * gap
    readonly property real oscShare: 0.25
    readonly property real mixerShare: 0.15
    readonly property real matrixShare: 0.37
    readonly property real voiceShare: 0.22
    readonly property real playShare: 0.15
    readonly property real row1Y: margin + headerHeight + gap
    readonly property real row2Y: row1Y + row1Height + gap
    readonly property real row3Y: row2Y + row2Height + gap

    objectName: "prismPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: row3Y + row3Height + margin
    radius: theme.panelRadius
    color: theme.canvas
    border.color: theme.line
    Accessible.role: Accessible.Pane
    Accessible.name: "Prism synthesizer"


    // ---- Header ---------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.margin
        width: panel.inner
        height: panel.headerHeight

        Canvas {
            id: glyph
            // The prism's corners, and each coloured beam's start and end
            // (the first beam's, then the step down per beam), in pixels.
            readonly property var corners: [[13, 2], [24, 26], [2, 26]]
            readonly property point beamFrom: Qt.point(18, 14)
            readonly property real beamFromStep: 0.8
            readonly property point beamTo: Qt.point(33, 8)
            readonly property real beamToStep: 4.2
            x: panel.theme.spaceXs
            anchors.verticalCenter: parent.verticalCenter
            width: panel.theme.glyphWidth
            height: panel.theme.glyphHeight
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                // A triangle splitting a beam into the section colours.
                ctx.strokeStyle = panel.theme.text
                ctx.lineWidth = panel.theme.glyphStroke
                ctx.lineJoin = "round"
                ctx.beginPath()
                ctx.moveTo(corners[0][0], corners[0][1]); ctx.lineTo(corners[1][0], corners[1][1])
                ctx.lineTo(corners[2][0], corners[2][1]); ctx.closePath()
                ctx.stroke()
                const colours = [panel.theme.osc1, panel.theme.osc2, panel.theme.mixer,
                                 panel.theme.amp, panel.theme.filter]
                ctx.lineWidth = panel.theme.glyphStroke
                for (let i = 0; i < colours.length; ++i) {
                    ctx.strokeStyle = colours[i]
                    ctx.beginPath()
                    ctx.moveTo(beamFrom.x, beamFrom.y + i * beamFromStep)
                    ctx.lineTo(beamTo.x, beamTo.y + i * beamToStep)
                    ctx.stroke()
                }
            }
        }
        Text {
            id: wordmark
            x: glyph.x + glyph.width + panel.theme.spaceMd
            y: panel.theme.wordmarkY
            text: "PRISM"
            color: panel.theme.text
            font.pixelSize: panel.theme.wordmarkSize
            font.bold: true
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            x: wordmark.x + panel.theme.taglineX
            y: wordmark.y + wordmark.height - panel.theme.taglineLift
            text: "POLYPHONIC SYNTHESIZER"
            color: panel.theme.faint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }

        // Preset browser.
        Row {
            id: browser
            anchors.centerIn: parent
            spacing: panel.theme.spaceSm
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "prismPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: presetBox
                objectName: "prismPresetBox"
                width: panel.theme.presetBoxWidth
                height: panel.theme.presetBoxHeight
                radius: panel.theme.radiusXl
                color: presetArea.containsMouse ? panel.theme.raised : panel.theme.panel
                border.color: activeFocus ? panel.theme.osc1 : panel.theme.line
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", modified" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(presetBox, panel.controller, panel.theme.osc1)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                Rectangle {
                    x: panel.theme.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    width: category.implicitWidth + panel.theme.spaceLg
                    height: panel.theme.presetChipHeight
                    radius: panel.theme.radiusMd
                    color: panel.theme.presetChipFill
                    Text {
                        id: category
                        anchors.centerIn: parent
                        text: panel.controller.preset_category.toUpperCase()
                        color: panel.theme.osc1
                        font.pixelSize: panel.theme.typeCaption
                        font.bold: true
                        font.letterSpacing: panel.theme.presetTagSpacing
                    }
                }
                Text {
                    objectName: "prismPresetName"
                    anchors.centerIn: parent
                    width: parent.width - panel.theme.presetNameClearance
                    horizontalAlignment: Text.AlignHCenter
                    text: panel.controller.preset_name
                    color: panel.theme.text
                    font.pixelSize: panel.theme.typeSubtitle
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.space2xl
                    anchors.verticalCenter: parent.verticalCenter
                    visible: panel.controller.preset_modified
                    text: "EDITED"
                    color: panel.theme.dim
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.presetTagSpacing
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.presetChevronRight
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.dim
                    font.pixelSize: panel.theme.typeBody
                }
                MouseArea {
                    id: presetArea
                    anchors.fill: parent
                    hoverEnabled: true
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
                objectName: "prismPresetNext"
                direction: 1
            }
        }

        // Audible status, with the next-note legend beneath.
        Kit.StatusBadge {
            id: status
            objectName: "prismStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.spaceSm
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.spaceXxs
            y: status.y + status.height + panel.theme.spaceSm
            spacing: panel.theme.legendSpacing
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.knobNextNoteSize
                height: panel.theme.knobNextNoteSize
                rotation: panel.theme.nextNoteMarkRotation
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

    // ---- Shared pieces ------------------------------------------------------

    component Caption: Text {
        color: panel.theme.faint
        font.pixelSize: panel.theme.typeCaption
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    component Oscillator: PrismSection {
        id: osc
        required property string prefix
        required property int number
        theme: panel.theme
        title: "Osc " + number
        accent: number === 1 ? panel.theme.osc1 : panel.theme.osc2
        readonly property real knobWidth: bodyWidth / 5
        // The wave picture under the shape choice, then the two knob rows.
        readonly property real waveY: 30
        readonly property real waveHeight: 52
        readonly property real tuneRowY: 90
        readonly property real voicesRowY: 156
        trailing: [
            Kit.Switch {
                visible: osc.number === 2
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.osc2.sync"
                accent: osc.accent
            },
            Kit.Dropdown {
                width: panel.theme.phaseDropdownWidth
                controller: panel.controller
                theme: panel.theme
                menu: menu
                parameterId: osc.prefix + ".phase"
                accent: osc.accent
            }
        ]
        PrismSegmented {
            width: osc.bodyWidth
            height: panel.theme.segmentedHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: osc.prefix + ".wave"
            accent: osc.accent
            icons: ["saw", "pulse", "triangle", "sine"]
        }
        PrismWaveView {
            y: osc.waveY
            width: osc.bodyWidth
            height: osc.waveHeight
            controller: panel.controller
            theme: panel.theme
            prefix: osc.prefix
            accent: osc.accent
        }
        Row {
            y: osc.tuneRowY
            Repeater {
                model: ["octave", "semi", "fine", "pw", "level"]
                delegate: Kit.Knob {
                    required property string modelData
                    width: osc.knobWidth
                    diameter: panel.theme.sectionKnobDiameter
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: osc.prefix + "." + modelData
                    accent: osc.accent
                }
            }
        }
        Row {
            y: osc.voicesRowY
            Repeater {
                model: osc.number === 1 ? ["unison", "detune", "width"]
                                        : ["unison", "detune", "width", "fm"]
                delegate: Kit.Knob {
                    required property string modelData
                    width: osc.knobWidth
                    diameter: panel.theme.sectionKnobDiameter
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: osc.prefix + "." + modelData
                    accent: osc.accent
                }
            }
        }
    }

    component Envelope: PrismSection {
        id: envelope
        required property string prefix
        // The envelope picture, then a row of up to knobColumns knobs.
        readonly property real pictureHeight: 80
        readonly property real knobRowY: 88
        readonly property int knobColumns: 6
        theme: panel.theme
        PrismEnvelopeView {
            width: envelope.bodyWidth
            height: envelope.pictureHeight
            controller: panel.controller
            theme: panel.theme
            prefix: envelope.prefix
            accent: envelope.accent
        }
        Row {
            y: envelope.knobRowY
            Repeater {
                model: envelope.prefix === "prism.amp"
                    ? ["attack", "decay", "sustain", "release", "curve", "velocity"]
                    : ["attack", "decay", "sustain", "release", "curve"]
                delegate: Kit.Knob {
                    required property string modelData
                    width: envelope.bodyWidth / envelope.knobColumns
                    diameter: panel.theme.compactKnobDiameter
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: envelope.prefix + "." + modelData
                    accent: envelope.accent
                }
            }
        }
    }

    component Lfo: PrismSection {
        id: lfo
        required property string prefix
        required property var depths
        // The mode choice at the right of the shape choice; the LFO picture
        // under them, then a row of knobColumns knobs.
        readonly property real modeWidth: 84
        readonly property real pictureY: 30
        readonly property real pictureHeight: 46
        readonly property real knobRowY: 84
        readonly property int knobColumns: 4
        theme: panel.theme
        readonly property bool synced: panel.controller.revision >= 0
            && panel.controller.choice(prefix + ".sync") === 1
        trailing: [
            Kit.Switch {
                controller: panel.controller
                theme: panel.theme
                parameterId: lfo.prefix + ".sync"
                accent: lfo.accent
            },
            Kit.Switch {
                label: "Retrig"
                controller: panel.controller
                theme: panel.theme
                parameterId: lfo.prefix + ".retrigger"
                accent: lfo.accent
            }
        ]
        PrismSegmented {
            width: lfo.bodyWidth - lfo.modeWidth - panel.theme.spaceMd
            height: panel.theme.segmentedHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: lfo.prefix + ".shape"
            accent: lfo.accent
            icons: ["sine", "triangle", "saw", "saw_down", "square", "sample_hold"]
        }
        PrismSegmented {
            x: lfo.bodyWidth - lfo.modeWidth
            width: lfo.modeWidth
            height: panel.theme.segmentedHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: lfo.prefix + ".mode"
            accent: lfo.accent
        }
        PrismLfoView {
            y: lfo.pictureY
            width: lfo.bodyWidth
            height: lfo.pictureHeight
            controller: panel.controller
            theme: panel.theme
            prefix: lfo.prefix
            accent: lfo.accent
        }
        Row {
            y: lfo.knobRowY
            Kit.Knob {
                width: lfo.bodyWidth / lfo.knobColumns
                diameter: panel.theme.sectionKnobDiameter
                controller: panel.controller
                theme: panel.theme
                parameterId: lfo.prefix + (lfo.synced ? ".division" : ".rate")
                label: "Rate"
                accent: lfo.accent
            }
            Kit.Knob {
                width: lfo.bodyWidth / lfo.knobColumns
                diameter: panel.theme.sectionKnobDiameter
                controller: panel.controller
                theme: panel.theme
                parameterId: lfo.prefix + ".fade"
                accent: lfo.accent
            }
            Repeater {
                model: lfo.depths
                delegate: Kit.Knob {
                    required property var modelData
                    width: lfo.bodyWidth / lfo.knobColumns
                    diameter: panel.theme.sectionKnobDiameter
                    label: modelData[1]
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: lfo.prefix + "." + modelData[0]
                    accent: lfo.accent
                }
            }
        }
    }

    // ---- Row 1: sources and filter ---------------------------------------

    Oscillator {
        id: osc1
        x: panel.margin
        y: panel.row1Y
        width: panel.columnsWidth * panel.oscShare
        height: panel.row1Height
        prefix: "prism.osc1"
        number: 1
    }
    Oscillator {
        id: osc2
        x: osc1.x + osc1.width + panel.gap
        y: panel.row1Y
        width: osc1.width
        height: panel.row1Height
        prefix: "prism.osc2"
        number: 2
    }
    PrismSection {
        id: mixer
        x: osc2.x + osc2.width + panel.gap
        y: panel.row1Y
        width: panel.columnsWidth * panel.mixerShare
        height: panel.row1Height
        theme: panel.theme
        title: "Mixer"
        accent: panel.theme.mixer
        // Sub wave and octave side by side (shares of the body), then the
        // noise colour and the level knobs, each under its caption.
        readonly property real subWaveShare: 0.52
        readonly property real subOctaveX: 0.56
        readonly property real subOctaveShare: 0.44
        readonly property real noiseY: 58
        readonly property real levelsY: 118
        readonly property real levelKnobsY: 136
        readonly property int levelColumns: 3
        Caption { text: "SUB" }
        PrismSegmented {
            y: panel.theme.captionRowHeight
            width: mixer.bodyWidth * mixer.subWaveShare
            height: panel.theme.segmentedCompactHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "prism.sub.wave"
            accent: mixer.accent
            icons: ["sine", "square"]
        }
        PrismSegmented {
            x: mixer.bodyWidth * mixer.subOctaveX
            y: panel.theme.captionRowHeight
            width: mixer.bodyWidth * mixer.subOctaveShare
            height: panel.theme.segmentedCompactHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "prism.sub.octave"
            accent: mixer.accent
            labels: [Format.number(-1, 0), Format.number(-2, 0)]
        }
        Caption {
            y: mixer.noiseY
            text: "NOISE"
        }
        PrismSegmented {
            y: mixer.noiseY + panel.theme.captionRowHeight
            width: mixer.bodyWidth
            height: panel.theme.segmentedCompactHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "prism.noise.colour"
            accent: mixer.accent
        }
        Caption {
            y: mixer.levelsY
            text: "LEVELS"
        }
        Row {
            y: mixer.levelKnobsY
            Repeater {
                model: [["prism.sub.level", "Sub"], ["prism.noise.level", "Noise"],
                        ["prism.ring.level", "Ring"]]
                delegate: Kit.Knob {
                    required property var modelData
                    width: mixer.bodyWidth / mixer.levelColumns
                    diameter: panel.theme.levelKnobDiameter
                    label: modelData[1]
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: modelData[0]
                    accent: mixer.accent
                }
            }
        }
    }
    PrismSection {
        id: filter
        x: mixer.x + mixer.width + panel.gap
        y: panel.row1Y
        width: panel.margin + panel.inner - x
        height: panel.row1Height
        theme: panel.theme
        title: "Filter"
        accent: panel.theme.filter
        // The response picture, then the two main knobs and four smaller
        // ones (shares of the body), the smaller dropped to align their dials.
        readonly property real pictureHeight: 112
        readonly property real knobRowY: 122
        readonly property real mainKnobShare: 0.2
        readonly property real smallKnobShare: 0.15
        readonly property real smallKnobHeight: 76
        readonly property real smallKnobDrop: 14
        trailing: [
            Kit.Dropdown {
                width: panel.theme.modeDropdownWidth
                controller: panel.controller
                theme: panel.theme
                menu: menu
                parameterId: "prism.filter.mode"
                accent: filter.accent
            }
        ]
        PrismFilterView {
            width: filter.bodyWidth
            height: filter.pictureHeight
            controller: panel.controller
            theme: panel.theme
            accent: filter.accent
        }
        Row {
            y: filter.knobRowY
            width: filter.bodyWidth
            Kit.Knob {
                width: filter.bodyWidth * filter.mainKnobShare
                diameter: panel.theme.filterKnobDiameter
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.filter.cutoff"
                accent: filter.accent
            }
            Kit.Knob {
                width: filter.bodyWidth * filter.mainKnobShare
                diameter: panel.theme.filterKnobDiameter
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.filter.resonance"
                accent: filter.accent
            }
            Repeater {
                model: [["drive", "Drive"], ["env", "Env Amt"], ["keytrack", "Key Track"],
                        ["velocity", "Velocity"]]
                delegate: Item {
                    id: small
                    required property var modelData
                    width: filter.bodyWidth * filter.smallKnobShare
                    height: filter.smallKnobHeight
                    Kit.Knob {
                        y: filter.smallKnobDrop
                        width: small.width
                        diameter: panel.theme.sectionKnobDiameter
                        label: small.modelData[1]
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: "prism.filter." + small.modelData[0]
                        accent: filter.accent
                    }
                }
            }
        }
    }

    // ---- Row 2: envelopes and LFOs ----------------------------------------

    Envelope {
        id: ampEnvelope
        x: panel.margin
        y: panel.row2Y
        width: panel.columnsWidth / panel.columns
        height: panel.row2Height
        title: "Amp Env"
        accent: panel.theme.amp
        prefix: "prism.amp"
    }
    Envelope {
        id: modEnvelope
        x: ampEnvelope.x + ampEnvelope.width + panel.gap
        y: panel.row2Y
        width: ampEnvelope.width
        height: panel.row2Height
        title: "Env 2 · Filter"
        accent: panel.theme.env2
        prefix: "prism.env2"
    }
    Lfo {
        id: lfo1
        x: modEnvelope.x + modEnvelope.width + panel.gap
        y: panel.row2Y
        width: ampEnvelope.width
        height: panel.row2Height
        title: "LFO 1"
        accent: panel.theme.lfo1
        prefix: "prism.lfo1"
        depths: [["vibrato", "Vibrato"], ["wheel", "Wheel Vib"]]
    }
    Lfo {
        id: lfo2
        x: lfo1.x + lfo1.width + panel.gap
        y: panel.row2Y
        width: panel.margin + panel.inner - x
        height: panel.row2Height
        title: "LFO 2"
        accent: panel.theme.lfo2
        prefix: "prism.lfo2"
        depths: [["cutoff", "To Cutoff"], ["pw", "To Width"]]
    }

    // ---- Row 3: matrix, voicing, performance, effects ---------------------

    PrismSection {
        id: matrix
        x: panel.margin
        y: panel.row3Y
        width: panel.columnsWidth * panel.matrixShare
        height: panel.row3Height
        theme: panel.theme
        title: "Mod Matrix"
        accent: panel.theme.mod
        // Width a slot's badge, arrow and gaps take; the rest is shared by
        // the source, target and amount.
        readonly property real slotFixedWidth: 64
        readonly property real routeShare: 0.34
        readonly property real amountShare: 0.32
        Column {
            spacing: panel.theme.spaceSm
            Repeater {
                model: 4
                delegate: Row {
                    id: slot
                    required property int index
                    readonly property string prefix: "prism.mod" + (index + 1)
                    readonly property bool active: panel.controller.revision >= 0
                        && panel.controller.choice(prefix + ".source") !== 0
                        && panel.controller.choice(prefix + ".target") !== 0
                    spacing: panel.theme.spaceSm
                    Rectangle {
                        width: panel.theme.slotBadgeWidth
                        height: panel.theme.slotBadgeHeight
                        radius: panel.theme.slotBadgeRadius
                        color: slot.active ? Qt.rgba(matrix.accent.r, matrix.accent.g, matrix.accent.b,
                                                     panel.theme.slotActiveAlpha)
                                           : panel.theme.well
                        Text {
                            anchors.centerIn: parent
                            text: slot.index + 1
                            color: slot.active ? matrix.accent : panel.theme.faint
                            font.pixelSize: panel.theme.typeSmall
                            font.bold: true
                        }
                    }
                    Kit.Dropdown {
                        width: (matrix.bodyWidth - matrix.slotFixedWidth) * matrix.routeShare
                        controller: panel.controller
                        theme: panel.theme
                        menu: menu
                        parameterId: slot.prefix + ".source"
                        accent: matrix.accent
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "→"
                        color: slot.active ? matrix.accent : panel.theme.faint
                        font.pixelSize: panel.theme.typeEmphasis
                    }
                    Kit.Dropdown {
                        width: (matrix.bodyWidth - matrix.slotFixedWidth) * matrix.routeShare
                        controller: panel.controller
                        theme: panel.theme
                        menu: menu
                        parameterId: slot.prefix + ".target"
                        accent: matrix.accent
                    }
                    PrismAmountBar {
                        width: (matrix.bodyWidth - matrix.slotFixedWidth) * matrix.amountShare
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: slot.prefix + ".amount"
                        accent: matrix.accent
                        active: slot.active
                    }
                }
            }
        }
    }
    PrismSection {
        id: voice
        x: matrix.x + matrix.width + panel.gap
        y: panel.row3Y
        width: panel.columnsWidth * panel.voiceShare
        height: panel.row3Height
        theme: panel.theme
        title: "Voice"
        accent: panel.theme.voice
        // The knob row under the mode, then the glide choices side by side
        // (shares of the body).
        readonly property real knobRowY: 30
        readonly property int knobColumns: 3
        readonly property real glideY: 100
        readonly property real glideModeShare: 0.42
        readonly property real glideWhenX: 0.46
        readonly property real glideWhenShare: 0.54
        PrismSegmented {
            width: voice.bodyWidth
            height: panel.theme.segmentedHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "prism.voice.mode"
            accent: voice.accent
        }
        Row {
            y: voice.knobRowY
            Repeater {
                model: ["glide", "bend_up", "bend_down"]
                delegate: Kit.Knob {
                    required property string modelData
                    width: voice.bodyWidth / voice.knobColumns
                    diameter: panel.theme.compactKnobDiameter
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "prism.voice." + modelData
                    accent: voice.accent
                }
            }
        }
        PrismSegmented {
            y: voice.glideY
            width: voice.bodyWidth * voice.glideModeShare
            height: panel.theme.segmentedCompactHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "prism.voice.glide_mode"
            accent: voice.accent
        }
        PrismSegmented {
            x: voice.bodyWidth * voice.glideWhenX
            y: voice.glideY
            width: voice.bodyWidth * voice.glideWhenShare
            height: panel.theme.segmentedCompactHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "prism.voice.glide_when"
            accent: voice.accent
        }
    }
    PrismSection {
        id: perform
        x: voice.x + voice.width + panel.gap
        y: panel.row3Y
        width: panel.columnsWidth * panel.playShare
        height: panel.row3Height
        theme: panel.theme
        title: "Play"
        accent: panel.theme.voice
        readonly property int wheels: 3
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: (perform.bodyWidth - perform.wheels * panel.theme.wheelWidth) / (perform.wheels - 1)
            PrismWheel {
                height: perform.bodyHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.perf.bend"
                label: "BEND"
                springy: true
                accent: perform.accent
            }
            PrismWheel {
                height: perform.bodyHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.perf.wheel"
                label: "MOD"
                accent: perform.accent
            }
            PrismWheel {
                height: perform.bodyHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.perf.pressure"
                label: "PRESS"
                accent: perform.accent
            }
        }
    }
    PrismSection {
        id: effects
        x: perform.x + perform.width + panel.gap
        y: panel.row3Y
        width: panel.margin + panel.inner - x
        height: panel.row3Height
        theme: panel.theme
        title: "Effects · Output"
        accent: panel.theme.fx
        // The knob row under the chorus choice.
        readonly property real knobRowY: 48
        readonly property int knobColumns: 3
        Caption { text: "CHORUS" }
        PrismSegmented {
            y: panel.theme.captionRowHeight
            width: effects.bodyWidth
            height: panel.theme.segmentedHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "prism.fx.chorus"
            accent: effects.accent
        }
        Row {
            y: effects.knobRowY
            Kit.Knob {
                width: effects.bodyWidth / effects.knobColumns
                diameter: panel.theme.effectKnobDiameter
                label: "Mix"
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.fx.chorus_mix"
                accent: effects.accent
            }
            Kit.Knob {
                width: effects.bodyWidth / effects.knobColumns
                diameter: panel.theme.effectKnobDiameter
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.fx.drive"
                accent: effects.accent
            }
            Kit.Knob {
                width: effects.bodyWidth / effects.knobColumns
                diameter: panel.theme.effectKnobDiameter
                label: "Output"
                controller: panel.controller
                theme: panel.theme
                parameterId: "prism.out.level"
                accent: panel.theme.text
            }
        }
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
