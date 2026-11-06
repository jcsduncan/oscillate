pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Dust device panel (ADR-0069): a bed of background texture under
// anything, and age for what passes through. The record turns where the
// engine's record is and sparks with its pops beside the bed meter (each
// layer's level and the duck); the shelf holds the eight layers; Age, Duck
// and Mix sit along the foot. Every control reads and writes through
// `controller` (NativeSynthDeviceController for Dust); live readings come
// only from the telemetry (layout in dust.hpp).
Rectangle {
    id: panel
    required property var controller
    // The tracks keying Dust (its sidechain, or the notes that duck the bed).
    property bool keyed: false
    property string keyName: "the sidechain"
    // Live state (ADR-0037), polled while visible.
    property var live: ({ live: false })
    property DustTheme theme: DustTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.sectionGap
    readonly property real headerHeight: theme.headerHeight
    readonly property real topHeight: theme.topRowHeight
    readonly property real shelfHeight: theme.shelfHeight
    readonly property real footHeight: theme.footRowHeight
    // The peak meters' reading with nothing playing (below any floor).
    readonly property real silentPeakDb: -200
    // Telemetry polls: fast while playing, slow while idle (milliseconds).
    readonly property int livePollInterval: 33
    readonly property int idlePollInterval: 250
    readonly property int revision: controller.revision
    // Live and decoded as Dust's frame (the controller's dust branch).
    readonly property bool playing: live.live === true && live.output_peak !== undefined
    // Duck sources (dust's table): the sidechain's audio, or the keying
    // tracks' notes.
    readonly property int sourceSidechain: 0
    readonly property int sourceKeyNotes: 1
    readonly property int duckSource: revision >= 0 ? controller.choice("dust.duck.source") : 0
    readonly property string seedId: "dust.mix.seed"
    // The layers in the engine's order (dust::Layer, the telemetry's order),
    // each with its own knobs and choice after Level, Tone and Width.
    readonly property var layers: [
        { key: "vinyl", title: "Vinyl", knobs: ["crackle", "dust", "wear", "rumble"], choice: "speed" },
        { key: "tape", title: "Tape", knobs: ["hiss", "dropouts"], choice: "" },
        { key: "hum", title: "Hum", knobs: ["buzz"], choice: "mains" },
        { key: "radio", title: "Radio", knobs: ["tuning"], choice: "gate" },
        // Short names where the table's would not fit under a card knob.
        { key: "rain", title: "Rain", knobs: ["intensity", "distance"], choice: "surface",
          labels: { intensity: "Density" } },
        { key: "wind", title: "Wind", knobs: ["strength", "gusts"], choice: "" },
        { key: "room", title: "Room", knobs: ["size"], choice: "" },
        { key: "machine", title: "Machine", knobs: ["speed"], choice: "type",
          choiceLabels: ["Fan", "Fridge", "Server"] }
    ]
    readonly property var layerKeys: layers.map(l => l.key)
    readonly property int layersOn: {
        if (revision < 0)
            return 0
        let count = 0
        for (const l of layers)
            count += controller.choice("dust." + l.key + ".on") === 1 ? 1 : 0
        return count
    }

    objectName: "dustPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: margin + headerHeight + gap + topHeight + gap + shelfHeight + gap + footHeight + margin
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.caseTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.caseColor }
        GradientStop { position: 1; color: panel.theme.caseBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Dust texture and vinyl"

    // A different record, a different storm: any seed but this one.
    function newSeed() {
        if (!controller.editable)
            return
        const info = controller.parameter(seedId)
        const low = Math.round(Number(info.minimum))
        const high = Math.round(Number(info.maximum))
        const now = Math.round(controller.value(seedId))
        let next = low + Math.floor(Math.random() * (high - low))
        if (next >= now)
            next += 1
        theme.lastTouched = seedId
        controller.set_value(seedId, Math.min(high, next))
    }
    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        return info.name === undefined ? "" : (info.group + " · " + info.name).toUpperCase()
    }

    component Note: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.noteSize
        wrapMode: Text.WordWrap
    }
    component FootKnob: Kit.Knob {
        controller: panel.controller
        theme: panel.theme
        diameter: panel.theme.footKnobDiameter
        readout: true
    }

    // ---- Header ----------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.margin - panel.theme.spaceSm
        width: panel.width - 2 * panel.margin
        height: panel.headerHeight

        Text {
            id: wordmark
            text: "DUST"
            color: panel.theme.amber
            font.family: panel.theme.display
            font.pixelSize: panel.theme.wordmarkSize
            font.bold: true
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            x: panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + 1
            text: "CRACKLE · HISS · RAIN · ROOM TONE · AGE"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }
        Row {
            x: panel.theme.presetX
            y: panel.theme.presetTop
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "dustPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "dustPresetDisplay"
                width: panel.theme.presetWidth
                height: panel.theme.presetHeight
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(display, panel.controller, panel.theme.accent)
                }
                Keys.onReturnPressed: openPresets()
                Keys.onSpacePressed: openPresets()
                Text {
                    x: panel.theme.spaceLg
                    y: panel.theme.presetCaptionTop
                    text: "PRESET · " + panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                }
                Text {
                    objectName: "dustPresetName"
                    x: panel.theme.spaceLg
                    y: panel.theme.presetNameTop
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
                    objectName: "dustReadout"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetCaptionTop
                    width: display.width - x - panel.theme.space2xl
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "dustReadoutValue"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetValueTop
                    width: display.width - x - panel.theme.space2xl
                    text: panel.theme.lastTouched === "" || panel.revision < 0 ? ""
                        : panel.controller.display(panel.theme.lastTouched)
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeEmphasis
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.presetChevronInset
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
                objectName: "dustPresetNext"
                direction: 1
            }
        }
        // Out, held peaks.
        Row {
            anchors.right: parent.right
            y: panel.theme.spaceXs
            height: panel.theme.outMeterHeight
            spacing: panel.theme.spaceXs
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "OUT"
                color: panel.theme.inkFaint
                font.pixelSize: panel.theme.typeMicro
                font.bold: true
                font.letterSpacing: panel.theme.captionSpacing
            }
            Kit.PeakMeter {
                objectName: "dustMeterOutLeft"
                theme: panel.theme
                width: panel.theme.outMeterWidth
                height: panel.theme.outMeterHeight
                live: panel.playing
                peakDb: panel.playing ? Number(panel.live.output_peak[0]) : panel.silentPeakDb
            }
            Kit.PeakMeter {
                objectName: "dustMeterOutRight"
                theme: panel.theme
                width: panel.theme.outMeterWidth
                height: panel.theme.outMeterHeight
                live: panel.playing
                peakDb: panel.playing ? Number(panel.live.output_peak[1]) : panel.silentPeakDb
            }
        }
    }

    // ---- The record and the bed --------------------------------------------------------

    Kit.Card {
        id: recordCard
        x: panel.margin
        y: header.y + header.height + panel.gap
        width: panel.theme.recordCardWidth
        height: panel.topHeight
        theme: panel.theme
        title: "THE RECORD"
        note: "WEAR · SEED " + (panel.revision >= 0 ? Math.round(panel.controller.value(panel.seedId)) : "")
        DustRecord {
            id: record
            x: panel.theme.spaceSm
            y: panel.theme.recordTop
            width: parent.width - 2 * panel.theme.spaceSm
            height: parent.height - panel.theme.recordTop - panel.theme.spaceXs
            controller: panel.controller
            theme: panel.theme
            live: panel.live
        }
    }
    Kit.Card {
        id: bedCard
        x: recordCard.x + recordCard.width + panel.gap
        y: recordCard.y
        width: panel.width - x - panel.margin
        height: panel.topHeight
        theme: panel.theme
        title: "THE BED"
        note: panel.layersOn === 0 ? "NO LAYERS ON · THE INPUT ALONE"
            : panel.layersOn + (panel.layersOn === 1 ? " LAYER" : " LAYERS") + " · DUCKED UNDER THE KEY"
        DustBedMeter {
            x: panel.theme.bedInset
            y: panel.theme.bedTop
            width: parent.width - 2 * panel.theme.bedInset
            height: parent.height - panel.theme.bedTop - panel.theme.spaceMd
            controller: panel.controller
            theme: panel.theme
            layers: panel.layers
            live: panel.live
        }
    }

    // ---- The shelf -------------------------------------------------------------------

    Row {
        id: shelf
        objectName: "dustShelf"
        x: panel.margin
        y: recordCard.y + recordCard.height + panel.gap
        width: panel.width - 2 * panel.margin
        height: panel.shelfHeight
        spacing: panel.theme.spaceMd
        Repeater {
            model: panel.layers
            delegate: DustLayerCard {
                required property var modelData
                width: (shelf.width - (panel.layers.length - 1) * shelf.spacing) / panel.layers.length
                height: shelf.height
                controller: panel.controller
                theme: panel.theme
                spec: modelData
            }
        }
    }

    // ---- Age, Duck, Mix ------------------------------------------------------------------

    Row {
        id: foot
        x: panel.margin
        y: shelf.y + shelf.height + panel.gap
        width: panel.width - 2 * panel.margin
        height: panel.height - y - panel.margin
        spacing: panel.gap

        Kit.Card {
            id: ageCard
            theme: panel.theme
            title: "AGE"
            note: "WHAT PASSES THROUGH"
            width: panel.theme.ageCardWidth
            height: foot.height
            Row {
                x: panel.theme.spaceMd
                y: panel.theme.footKnobsTop
                spacing: panel.theme.ageKnobSpacing
                Repeater {
                    model: ["dust.age.amount", "dust.age.wow", "dust.age.flutter", "dust.age.mono"]
                    delegate: FootKnob {
                        required property string modelData
                        parameterId: modelData
                        // "Mono below" does not fit a foot knob; its value says below what.
                        label: modelData === "dust.age.mono" ? "Mono" : (info.name !== undefined ? info.name : "")
                    }
                }
            }
            Note {
                x: panel.theme.spaceLg
                y: ageCard.height - panel.theme.noteRise
                width: ageCard.width - 2 * panel.theme.spaceLg
                text: "A worn record's band and drive, its pitch drifting; lows folded to mono."
            }
        }

        Kit.Card {
            id: duckCard
            theme: panel.theme
            title: "DUCK"
            note: panel.keyed ? "KEY · " + panel.keyName.toUpperCase() : "NO KEY"
            noteColor: panel.keyed ? panel.theme.teal : panel.theme.inkFaint
            width: panel.theme.duckCardWidth
            height: foot.height
            Kit.Segment {
                x: panel.theme.spaceLg
                y: panel.theme.duckSourceTop
                width: panel.theme.duckSourceWidth
                height: panel.theme.segmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "dust.duck.source"
                accent: panel.theme.teal
            }
            Note {
                objectName: "dustDuckKey"
                x: panel.theme.spaceLg
                y: panel.theme.duckKeyTop
                width: panel.theme.duckSourceWidth
                color: panel.keyed ? panel.theme.inkDim : panel.theme.warning
                text: panel.keyed
                    ? (panel.duckSource === panel.sourceKeyNotes
                       ? "Ducks on each note of " + panel.keyName + "."
                       : "Ducks under " + panel.keyName + "'s audio.")
                    : (panel.duckSource === panel.sourceKeyNotes
                       ? "No key · send a MIDI track here to duck on its notes."
                       : "No key · send the kick track here to duck under it.")
            }
            Row {
                x: panel.theme.duckKnobsX
                y: panel.theme.footKnobsTop
                FootKnob { parameterId: "dust.duck.amount"; accent: panel.theme.teal }
                FootKnob { parameterId: "dust.duck.release"; accent: panel.theme.teal }
            }
            Note {
                x: panel.theme.spaceLg
                y: duckCard.height - panel.theme.noteRise
                width: duckCard.width - 2 * panel.theme.spaceLg
                text: "Only the bed ducks, never the input: it breathes with the kick."
            }
        }

        Kit.Card {
            id: mixCard
            theme: panel.theme
            title: "MIX"
            width: foot.width - ageCard.width - duckCard.width - 2 * panel.gap
            height: foot.height
            Row {
                x: panel.theme.spaceMd
                y: panel.theme.footKnobsTop
                FootKnob { parameterId: "dust.mix.input" }
                FootKnob { parameterId: "dust.mix.bed" }
                FootKnob { parameterId: "dust.mix.output" }
            }
            Column {
                x: mixCard.width - width - panel.theme.seedInset
                y: panel.theme.seedTop
                spacing: panel.theme.spaceSm
                Text {
                    text: "SEED"
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.tagSpacing
                }
                Row {
                    spacing: panel.theme.spaceSm
                    Kit.Stepper {
                        width: panel.theme.seedStepperWidth
                        controller: panel.controller
                        theme: panel.theme
                        parameterId: panel.seedId
                        plain: true
                    }
                    Rectangle {
                        id: dice
                        objectName: "dustSeedDice"
                        width: panel.theme.diceWidth
                        height: panel.theme.diceHeight
                        radius: panel.theme.radiusMd
                        color: diceArea.pressed ? panel.theme.button : panel.theme.buttonHi
                        border.color: activeFocus || diceArea.containsMouse ? panel.theme.accent : panel.theme.wellEdge
                        activeFocusOnTab: panel.controller.editable
                        opacity: panel.controller.editable ? 1 : panel.theme.diceDisabledOpacity
                        Accessible.role: Accessible.Button
                        Accessible.name: "New seed"
                        Accessible.description: "A different record, a different storm"
                        Keys.onSpacePressed: panel.newSeed()
                        Keys.onReturnPressed: panel.newSeed()
                        Text {
                            anchors.centerIn: parent
                            text: "🎲"
                            color: panel.theme.ink
                            font.pixelSize: panel.theme.typeEmphasis
                        }
                        MouseArea {
                            id: diceArea
                            anchors.fill: parent
                            enabled: panel.controller.editable
                            hoverEnabled: true
                            onClicked: {
                                dice.forceActiveFocus()
                                panel.newSeed()
                            }
                            Kit.HoverTip {
                                theme: panel.theme
                                text: "New seed · a different record, a different storm"
                            }
                        }
                    }
                }
            }
            Note {
                x: panel.theme.spaceLg
                y: mixCard.height - panel.theme.noteRise
                width: mixCard.width - 2 * panel.theme.spaceLg
                text: "Seeded: an export sounds exactly as playback did."
            }
        }
    }

    Timer {
        objectName: "dustTelemetryPoll"
        interval: panel.playing ? panel.livePollInterval : panel.idlePollInterval
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
