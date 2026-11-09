pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit
import "ChopGate.js" as Gate

// The Chop device panel (ADR-0075): mode, steps and division along the top with
// what was touched last; the pattern presets and Re-seed under them; then
// one pattern of the bass with the gate over it (the playhead and the bass
// from the telemetry, layout in chop.hpp); the lanes to draw (level,
// length, ratchet, repeat, chance), each stroke one undo step; and the
// knobs. Everything reads and writes through `controller`
// (NativeSynthDeviceController for Chop).
Rectangle {
    id: panel
    required property var controller
    property ChopTheme theme: ChopTheme {}
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })

    readonly property real margin: theme.panelMargin
    readonly property real gap: theme.spaceLg
    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true && live.phase !== undefined && live.phase >= 0
    // The pattern's steps, read from chop.steps' choice labels ("16", "32").
    readonly property int stepCount: Number(controller.parameter("chop.steps").choices[
        revision >= 0 ? controller.choice("chop.steps") : 0])
    readonly property int playingStep: playing && live.step !== undefined ? live.step : -1
    readonly property var played: playing && live.played !== undefined ? live.played : []
    readonly property var laneTints: ({ level: theme.blade, length: theme.blade, ratchet: theme.amber,
                                        repeat: theme.violet, chance: theme.teal })
    readonly property real tallLane: theme.laneTallHeight
    readonly property real lane: theme.laneHeight
    readonly property real laneGap: theme.spaceSm
    readonly property real knobSpacing: theme.knobSpacing
    // chop.mode's choices (chop_table::modes): Gate, Repeat, Gate + repeat.
    readonly property int modeGate: 0
    readonly property int modeRepeat: 1
    readonly property int mode: revision >= 0 ? controller.choice("chop.mode") : modeGate
    // Why knobs go quiet in a mode: Repeat has no gate, Gate no repeats.
    readonly property string gateOffReason: mode === modeRepeat ? "The gate is off in Repeat mode" : ""
    readonly property string repeatOffReason: mode === modeGate ? "Repeats are off in Gate mode" : ""

    objectName: "chopPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    color: theme.plate
    border.color: theme.plateEdge
    Accessible.role: Accessible.Pane
    Accessible.name: "Chop gate and stutter"

    function reseed() {
        const info = controller.parameter("chop.seed")
        theme.lastTouched = "chop.seed"
        controller.set_value("chop.seed",
                             Gate.reseed(Math.round(controller.value("chop.seed")), info.minimum, info.maximum))
    }

    component PanelKnob: Kit.Knob {
        controller: panel.controller
        theme: panel.theme
        readout: true
        accent: panel.theme.blade
    }
    component Chip: Rectangle {
        id: chip
        property string label: ""
        property bool on: false
        signal activated()
        width: chipText.implicitWidth + panel.theme.chipPad
        height: panel.theme.chipHeight
        radius: height / 2
        color: panel.theme.plateHi
        border.color: on ? panel.theme.blade : panel.theme.plateEdge
        border.width: on ? panel.theme.strokeBold : panel.theme.hairline
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: label
        Keys.onReturnPressed: activated()
        Keys.onSpacePressed: activated()
        Text {
            id: chipText
            anchors.centerIn: parent
            text: chip.label
            color: chip.on ? panel.theme.ink : panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeLabel
            font.bold: true
        }
        MouseArea {
            anchors.fill: parent
            enabled: panel.controller.editable
            onClicked: chip.activated()
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
        // The blade: a coral tile with a notch cut from its right side, then
        // the wordmark.
        brand: Component {
            Row {
                spacing: panel.theme.wordmarkGap
                Rectangle {
                    id: glyph
                    // The notch: where it starts across and down the tile, and its size.
                    readonly property real notchX: 0.55
                    readonly property real notchY: 0.35
                    readonly property real notchWidth: 0.45
                    readonly property real notchHeight: 0.3
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.theme.glyphSize
                    height: panel.theme.glyphSize
                    radius: panel.theme.radiusLg
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: panel.theme.blade }
                        GradientStop { position: 1; color: panel.theme.blade2 }
                    }
                    Rectangle {
                        x: parent.width * glyph.notchX
                        y: parent.height * glyph.notchY
                        width: parent.width * glyph.notchWidth
                        height: parent.height * glyph.notchHeight
                        color: panel.theme.plate
                    }
                }
                Text {
                    objectName: "chopWordmark"
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Chop"
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: Font.ExtraBold
                }
            }
        }

        // Mode, steps and division: the header's second row.
        Kit.Segment {
            width: panel.theme.modeSegmentWidth
            controller: panel.controller
            theme: panel.theme
            parameterId: "chop.mode"
            labels: ["Gate", "Repeat", "Gate + repeat"]
        }
        Kit.Segment {
            width: panel.theme.stepsSegmentWidth
            controller: panel.controller
            theme: panel.theme
            parameterId: "chop.steps"
            labels: ["16 steps", "32"]
        }
        Kit.Segment {
            width: panel.theme.divisionSegmentWidth
            controller: panel.controller
            theme: panel.theme
            parameterId: "chop.division"
        }
    }

    // ---- Pattern presets ---------------------------------------------------------------

    Flow {
        id: patterns
        objectName: "chopPatterns"
        x: panel.margin
        y: header.y + header.height + panel.gap
        width: panel.width - 2 * panel.margin
        spacing: panel.theme.spaceSm
        Repeater {
            model: panel.controller.presets
            delegate: Chip {
                required property var modelData
                objectName: "chopPreset:" + modelData.name
                label: modelData.name
                on: panel.controller.preset_index === modelData.index && !panel.controller.preset_modified
                onActivated: panel.controller.apply_preset(modelData.index)
            }
        }
        Chip {
            objectName: "chopReseed"
            label: "⚄ Re-seed chance"
            onActivated: panel.reseed()
        }
    }

    // ---- The pattern -------------------------------------------------------------------

    // The pattern along its beats; LIVE while the bass plays through it,
    // a PREVIEW of the gate over a flat bar before.
    Kit.PlotFrame {
        id: heroFrame
        x: panel.margin
        y: patterns.y + patterns.height + panel.gap
        width: panel.width - 2 * panel.margin
        height: panel.theme.heroHeight
        theme: panel.theme
        live: panel.playing
        xUnit: "beat"
        xTicks: {
            const ticks = []
            for (let beat = 0; beat * Gate.beatSteps < panel.stepCount; ++beat)
                ticks.push({ at: beat * Gate.beatSteps / panel.stepCount, label: Format.number(beat + 1, 0) })
            return ticks
        }
        ChopHero {
            id: hero
            anchors.fill: parent
            controller: panel.controller
            theme: panel.theme
            live: panel.live
            stepCount: panel.stepCount
        }
    }

    Column {
        id: lanes
        x: panel.margin
        y: heroFrame.y + heroFrame.height + panel.gap
        width: panel.width - 2 * panel.margin
        spacing: panel.laneGap
        Repeater {
            model: Gate.lanes
            delegate: ChopLane {
                required property var modelData
                width: lanes.width
                height: modelData.tall === true ? panel.tallLane : panel.lane
                controller: panel.controller
                theme: panel.theme
                field: modelData.field
                title: modelData.title
                caption: modelData.caption
                tint: panel.laneTints[modelData.field]
                stepCount: panel.stepCount
                playingStep: panel.playingStep
                played: panel.played
            }
        }
    }

    // ---- Knobs -------------------------------------------------------------------------

    Kit.Card {
        id: knobs
        x: panel.margin
        y: lanes.y + lanes.height + panel.gap
        width: panel.width - 2 * panel.margin
        height: panel.height - y - panel.margin
        theme: panel.theme
        title: "EDGES · GROOVE · REPEAT · OUTPUT"
        note: panel.gateOffReason !== "" ? panel.gateOffReason : panel.repeatOffReason
        noteColor: panel.theme.inkDim
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.knobRowY
            spacing: panel.knobSpacing
            PanelKnob { parameterId: "chop.smooth" }
            PanelKnob {
                parameterId: "chop.shape"
                inactive: panel.gateOffReason !== ""
                inactiveReason: panel.gateOffReason
                valueText: panel.revision >= 0 ? Gate.shapeText(panel.controller.value("chop.shape")) : ""
            }
            PanelKnob {
                parameterId: "chop.swing"
                valueText: panel.revision >= 0 ? Gate.swingText(panel.controller.value("chop.swing")) : ""
            }
            PanelKnob {
                parameterId: "chop.rotate"
                valueText: panel.revision >= 0
                    ? Format.quantity(Math.round(panel.controller.value("chop.rotate")) % panel.stepCount, "steps", 0) : ""
            }
            PanelKnob {
                parameterId: "chop.repeat_pitch"
                accent: panel.theme.violet
                inactive: panel.repeatOffReason !== ""
                inactiveReason: panel.repeatOffReason
            }
            PanelKnob {
                parameterId: "chop.repeat_decay"
                accent: panel.theme.violet
                inactive: panel.repeatOffReason !== ""
                inactiveReason: panel.repeatOffReason
            }
            PanelKnob {
                parameterId: "chop.depth"
                inactive: panel.gateOffReason !== ""
                inactiveReason: panel.gateOffReason
            }
            PanelKnob { parameterId: "chop.mix" }
            PanelKnob { parameterId: "chop.seed"; accent: panel.theme.teal }
        }
    }

    Timer {
        objectName: "chopTelemetryPoll"
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
