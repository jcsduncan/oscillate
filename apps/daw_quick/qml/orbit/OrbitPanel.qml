pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Orbit device panel: a night-sky modulation card. Seven mode tabs pick
// chorus, ensemble, dimension, flanger, phaser, vibrato or tremolo, each in
// its own luminous colour. The orrery shows both channels' LFOs as bodies
// orbiting the signal, the stereo offset as the arc between them; the
// sweep scope shows what the LFO moves and how far. Below, Motion holds the
// LFO, Voice only the controls the mode uses, Output the tone and levels.
// Every control reads and writes through `controller`
// (NativeSynthDeviceController for the Orbit engine); no project or audio
// state lives in QML.
Rectangle {
    id: panel
    required property var controller
    property OrbitTheme theme: OrbitTheme {
        accent: modeColors[panel.mode]
    }

    readonly property real margin: theme.spaceXl
    readonly property int mode: controller.revision >= 0 ? controller.choice("orbit.mode") : 0
    readonly property bool synced: controller.revision >= 0 && controller.choice("orbit.sync") === 1
    readonly property int shape: controller.revision >= 0 ? controller.choice("orbit.shape") : 0
    readonly property real depth: controller.revision >= 0 ? controller.value("orbit.depth") : 0
    readonly property real stereo: controller.revision >= 0 ? controller.value("orbit.stereo") : 0
    readonly property real rate: controller.revision >= 0 ? controller.value("orbit.rate") : 1
    readonly property real time: controller.revision >= 0 ? controller.value("orbit.time") : 10
    readonly property real centre: controller.revision >= 0 ? controller.value("orbit.centre") : 800
    readonly property int voices: controller.revision >= 0 ? controller.choice("orbit.voices") + 1 : 2

    // Which controls each mode uses.
    readonly property bool usesTime: mode === 0 || mode === 2 || mode === 3 || mode === 5
    readonly property bool usesFeedback: mode === 0 || mode === 3 || mode === 4
    readonly property bool usesVoices: mode === 0
    readonly property bool usesPhaser: mode === 4
    readonly property bool usesStereo: mode !== 1 && mode !== 2

    // Live engine state (ADR-0037), polled while the panel is visible: fast
    // while the effect publishes, slowly while it does not.
    property var live: ({ live: false })
    readonly property bool isLive: live.live === true
    // Preview: the panel runs its own LFO phase at the patch's rate (synced
    // divisions at 120 BPM) so the card moves while nothing plays.
    property real previewPhase: 0.08
    property bool animate: true
    // Live: each body's half of the orbit follows its LFO's direction.
    property real previousLeft: 0
    property real previousRight: 0
    property real liveAngleLeft: 0
    property real liveAngleRight: 0

    objectName: "orbitPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.bodyTop }
        GradientStop { position: panel.theme.bodyStop; color: panel.theme.body }
        GradientStop { position: 1; color: panel.theme.bodyBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Orbit modulation effect"

    onLiveChanged: {
        if (live.live !== true)
            return
        liveAngleLeft = angleFor(live.lfo_left, previousLeft)
        liveAngleRight = angleFor(live.lfo_right, previousRight)
        previousLeft = live.lfo_left
        previousRight = live.lfo_right
    }

    // A bipolar LFO value as an angle on the orbit: cos(angle) is the value,
    // rising values ride the far half, falling ones the near half.
    function angleFor(value, previous) {
        const v = Math.max(-1, Math.min(1, value))
        const a = Math.acos(v)
        return v >= previous ? -a : a
    }
    function previewAngle(offset) {
        return 2 * Math.PI * (previewPhase + offset) - Math.PI / 2
    }
    // Cycles per second the preview runs at.
    function previewRate() {
        if (synced) {
            const lengths = [0.125, 1 / 6, 0.25, 0.375, 1 / 3, 0.5, 0.75, 2 / 3, 1, 1.5, 4 / 3, 2,
                             3, 4, 8, 16, 32]
            const division = controller.choice("orbit.division")
            return 2 / lengths[Math.max(0, Math.min(lengths.length - 1, division))]
        }
        return rate
    }
    function wave(phase) {
        const p = phase - Math.floor(phase)
        if (shape === 1)
            return 4 * Math.abs(((p - 0.25) - Math.floor(p - 0.25)) - 0.5) - 1
        return Math.sin(2 * Math.PI * p)
    }

    // What the LFO sweeps, from the engine's constants (orbit.cpp): delays
    // swing +-30% of Time (chorus voices spread -17%..+11% on top), the
    // dimension +-25%, the flanger from Time down by 95% at full depth, the
    // ensemble 5 ms +- 1.8 ms; the phaser +-3 octaves; the tremolo's gain
    // falls by depth.
    function sweepRange() {
        const d = depth
        switch (mode) {
        case 1:
            return { lo: 5 - 1.8 * d, hi: 5 + 1.8 * d, unit: "ms", log: false }
        case 2:
            return { lo: time * (1 - 0.25 * d), hi: time * (1 + 0.25 * d), unit: "ms", log: false }
        case 3:
            return { lo: time * (1 - 0.95 * d), hi: time, unit: "ms", log: false }
        case 4:
            return { lo: Math.max(20, centre * Math.pow(2, -3 * d)),
                     hi: Math.min(21600, centre * Math.pow(2, 3 * d)), unit: "Hz", log: true }
        case 6:
            return { lo: 1 - d, hi: 1, unit: "%", log: false }
        default: {
            const spread = mode === 0 && voices > 1
            return { lo: time * (spread ? 0.83 : 1) * (1 - 0.3 * d),
                     hi: time * (spread ? 1.11 : 1) * (1 + 0.3 * d), unit: "ms", log: false }
        }
        }
    }
    // A channel's sweep position at LFO phase `phase` (the first voice), as
    // the engine computes it: the flanger and tremolo use the unipolar
    // 0.5 + 0.5 shape(phase - 1/4); the dimension's right channel mirrors
    // the left; the ensemble reports its first tap on both.
    function sweepAt(phase, right) {
        const d = depth
        const value = wave(phase)
        const rising = 0.5 + 0.5 * wave(phase - 0.25)
        switch (mode) {
        case 1:
            return 5 + 1.5 * d * value
        case 2:
            return time * (1 + (right ? -0.25 : 0.25) * d * value)
        case 3:
            return time * (1 - 0.95 * d * rising)
        case 4:
            return Math.max(20, Math.min(21600, centre * Math.pow(2, 3 * d * value)))
        case 6:
            return 1 - d * rising
        default:
            return time * (1 + 0.3 * d * value)
        }
    }
    // The phase offset of the right channel's LFO.
    function rightOffset() {
        return usesStereo ? stereo / 360 : 0
    }
    function sweepLeft() {
        return isLive ? live.sweep_left : sweepAt(previewPhase, false)
    }
    function sweepRight() {
        return isLive ? live.sweep_right : sweepAt(previewPhase + rightOffset(), true)
    }
    function formatSweep(value) {
        if (mode === 4)
            return value >= 1000 ? (value / 1000).toFixed(2) + " kHz" : Math.round(value) + " Hz"
        if (mode === 6)
            return Math.round(value * 100) + "%"
        return value.toFixed(value < 10 ? 2 : 1) + " ms"
    }
    function sweepCaption() {
        if (mode === 4)
            return "NOTCH CENTRE"
        if (mode === 6)
            return "GAIN"
        if (mode === 3)
            return "FLANGE DELAY"
        return "DELAY"
    }
    function timeLabel() {
        return mode === 3 ? "Max Delay" : "Delay"
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
        return id === "" || controller.revision < 0 ? "" : controller.display(id)
    }

    // A glass section with a title.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        radius: panel.theme.plateRadius
        gradient: Gradient {
            GradientStop { position: 0; color: panel.theme.plateGlass }
            GradientStop { position: 1; color: panel.theme.plate }
        }
        border.color: panel.theme.plateEdge
        Rectangle {
            x: panel.theme.plateDotX
            y: panel.theme.plateDotY
            width: panel.theme.plateDotSize
            height: panel.theme.plateDotSize
            radius: panel.theme.plateDotSize / 2
            color: panel.theme.accent
        }
        Text {
            id: plateTitle
            x: panel.theme.plateTitleX
            y: panel.theme.plateTitleY
            text: plate.title
            color: panel.theme.ink
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
            font.letterSpacing: panel.theme.plateNoteSpacing
        }
    }

    component Cell: Item {
        id: cell
        required property string parameterId
        property int diameter: panel.theme.cellDiameter
        property bool inactive: false
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

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        // The wordmark with its own small orbit.
        Item {
            id: mark
            width: panel.theme.markSize
            height: panel.theme.markSize
            Rectangle {
                anchors.centerIn: parent
                width: panel.theme.markRingWidth
                height: panel.theme.markRingHeight
                radius: panel.theme.markRingHeight / 2
                color: "transparent"
                border.color: Qt.rgba(panel.theme.accent.r, panel.theme.accent.g, panel.theme.accent.b,
                                      panel.theme.markRingAlpha)
                border.width: panel.theme.markRingStroke
                rotation: panel.theme.markRingTilt
            }
            Rectangle {
                anchors.centerIn: parent
                width: panel.theme.markCoreSize
                height: panel.theme.markCoreSize
                radius: panel.theme.markCoreSize / 2
                gradient: Gradient {
                    GradientStop { position: 0; color: panel.theme.glint }
                    GradientStop { position: 1; color: panel.theme.accent }
                }
            }
            Rectangle {
                x: panel.theme.markMoonX
                y: panel.theme.markMoonY
                width: panel.theme.markMoonSize
                height: panel.theme.markMoonSize
                radius: panel.theme.markMoonSize / 2
                color: panel.theme.companion
            }
        }
        Text {
            id: wordmark
            x: mark.width + panel.theme.spaceMd
            y: 0
            text: "ORBIT"
            color: panel.theme.ink
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            x: wordmark.x + panel.theme.taglineIndent
            y: wordmark.y + wordmark.height + panel.theme.taglineGap
            text: "MODULATION · " + panel.theme.modeTaglines[panel.mode]
            color: panel.theme.accent
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }

        // Preset browser and readout.
        Row {
            id: browser
            x: panel.theme.browserX
            y: panel.theme.browserY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "orbitPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "orbitPresetDisplay"
                width: panel.theme.presetDisplayWidth
                height: panel.theme.presetDisplayHeight
                radius: panel.theme.presetDisplayRadius
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
                    x: panel.theme.presetTextX
                    y: panel.theme.presetCaptionY
                    text: panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.presetCategorySpacing
                }
                Text {
                    objectName: "orbitPresetName"
                    x: panel.theme.presetTextX
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
                    objectName: "orbitReadout"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.presetReadoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "orbitReadoutValue"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetValueY
                    width: display.width - x - panel.theme.presetReadoutRight
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
                objectName: "orbitPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "orbitStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- Mode tabs ------------------------------------------------------------------

    Row {
        id: tabs
        objectName: "orbitTabs:orbit.mode"
        x: panel.margin
        y: panel.theme.tabsY
        spacing: panel.theme.spaceSm
        activeFocusOnTab: panel.controller.editable
        Accessible.role: Accessible.PageTabList
        Accessible.name: "Mode"
        Accessible.description: panel.theme.modeNames[panel.mode]
        readonly property real tabWidth: (panel.width - 2 * panel.margin - 6 * spacing) / 7
        function choose(index) {
            if (!panel.controller.editable)
                return
            panel.controller.set_choice("orbit.mode", Math.max(0, Math.min(6, index)))
            panel.theme.lastTouched = "orbit.mode"
        }
        Keys.onLeftPressed: choose(panel.mode - 1)
        Keys.onRightPressed: choose(panel.mode + 1)
        Keys.onDeletePressed: {
            panel.controller.reset_to_default("orbit.mode")
            panel.theme.lastTouched = "orbit.mode"
        }
        Repeater {
            model: 7
            delegate: Rectangle {
                id: tab
                required property int index
                readonly property bool on: index === panel.mode
                readonly property color tone: panel.theme.modeColors[index]
                objectName: "orbitMode:" + index
                width: tabs.tabWidth
                height: panel.theme.tabHeight
                radius: panel.theme.radiusXl
                color: on ? Qt.rgba(tone.r, tone.g, tone.b, panel.theme.tabOnAlpha) : tabArea.containsMouse ? panel.theme.buttonHi : panel.theme.button
                border.color: on ? tone : panel.theme.wellEdge
                border.width: on ? panel.theme.tabOnStroke : panel.theme.hairline
                Accessible.role: Accessible.PageTab
                Accessible.name: panel.theme.modeNames[index]
                Accessible.checked: on
                // A tiny orbit glyph.
                Item {
                    x: panel.theme.tabGlyphX
                    anchors.verticalCenter: parent.verticalCenter
                    width: panel.theme.tabGlyphWidth
                    height: panel.theme.tabGlyphHeight
                    Rectangle {
                        anchors.fill: parent
                        radius: panel.theme.tabGlyphHeight / 2
                        color: "transparent"
                        border.color: tab.on ? tab.tone : panel.theme.inkFaint
                        border.width: panel.theme.hairline
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: panel.theme.tabGlyphCore
                        height: panel.theme.tabGlyphCore
                        radius: panel.theme.tabGlyphCore / 2
                        color: tab.on ? panel.theme.glint : panel.theme.inkFaint
                    }
                    Rectangle {
                        x: panel.theme.tabGlyphMoonX
                        y: panel.theme.tabGlyphMoonY
                        width: panel.theme.tabGlyphMoonSize
                        height: panel.theme.tabGlyphMoonSize
                        radius: panel.theme.tabGlyphMoonSize / 2
                        color: tab.on ? tab.tone : panel.theme.ledOff
                    }
                }
                Text {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: panel.theme.tabTextOffset
                    text: panel.theme.modeNames[tab.index]
                    color: tab.on ? tab.tone : panel.theme.inkDim
                    font.pixelSize: panel.theme.typeBody
                    font.bold: true
                    font.letterSpacing: panel.theme.tabSpacing
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

    // ---- Hero: orrery and sweep -------------------------------------------------------

    Rectangle {
        id: sky
        x: panel.margin
        y: panel.theme.heroY
        width: panel.theme.skyWidth
        height: panel.theme.heroHeight
        radius: panel.theme.skyRadius
        color: panel.theme.sky
        border.color: panel.theme.plateEdge
        clip: true

        OrbitOrrery {
            anchors.fill: parent
            anchors.margins: panel.theme.hairline
            theme: panel.theme
            depth: panel.depth
            stereoDegrees: panel.usesStereo ? panel.stereo : 0
            live: panel.isLive
            direction: 1
            angleLeft: panel.isLive ? panel.liveAngleLeft : panel.previewAngle(0)
            angleRight: panel.isLive ? panel.liveAngleRight
                : panel.previewAngle(panel.usesStereo ? panel.stereo / 360 : (panel.mode === 1 ? 2 / 3 : 0.5))
        }

        // Readouts over the sky.
        Column {
            x: panel.theme.skyInsetX
            y: panel.theme.skyInsetY
            spacing: panel.theme.spaceXxs
            Caption {
                text: panel.sweepCaption() + " · L / R"
            }
            Row {
                spacing: panel.theme.sweepReadoutGap
                Text {
                    objectName: "orbitSweepLeft"
                    text: panel.formatSweep(panel.sweepLeft())
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.readoutSize
                    font.bold: true
                }
                Text {
                    objectName: "orbitSweepRight"
                    text: panel.formatSweep(panel.sweepRight())
                    color: panel.theme.companion
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.readoutSize
                    font.bold: true
                }
            }
        }
        Column {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.skyInsetX
            y: panel.theme.skyInsetY
            spacing: panel.theme.spaceXxs
            Caption {
                anchors.right: parent.right
                text: panel.synced ? "SYNCED RATE" : "RATE"
            }
            Text {
                anchors.right: parent.right
                text: panel.synced ? panel.controller.display("orbit.division")
                                   : panel.controller.revision >= 0 ? panel.controller.display("orbit.rate") : ""
                color: panel.theme.ink
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.readoutSize
                font.bold: true
            }
        }
        Row {
            x: panel.theme.skyInsetX
            anchors.bottom: parent.bottom
            anchors.bottomMargin: panel.theme.skyInsetY
            spacing: panel.theme.liveLampGap
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.liveLampSize
                height: panel.theme.liveLampSize
                radius: panel.theme.liveLampSize / 2
                color: panel.isLive ? panel.theme.live : panel.theme.inkFaint
            }
            Caption {
                objectName: "orbitLiveCaption"
                text: panel.isLive ? "LIVE" : "PREVIEW · NOT PLAYING"
                color: panel.isLive ? panel.theme.live : panel.theme.inkFaint
            }
        }
        Caption {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.skyInsetX
            anchors.bottom: parent.bottom
            anchors.bottomMargin: panel.theme.skyInsetY
            text: panel.mode === 1 ? "THREE TAPS · 120° APART"
                : panel.mode === 2 ? "L AND R SWEEP IN ANTI-PHASE"
                : "DEPTH " + Math.round(panel.depth * 100) + "% · STEREO " + Math.round(panel.stereo) + "°"
        }
    }

    Plate {
        id: sweepPlate
        x: sky.x + sky.width + panel.theme.spaceLg
        y: sky.y
        width: panel.width - x - panel.margin
        height: sky.height
        title: "SWEEP"
        note: panel.mode === 4 ? "LOG FREQUENCY" : panel.mode === 6 ? "LEVEL" : "DELAY TIME"
        OrbitSweep {
            x: panel.theme.sweepInset
            y: panel.theme.plateBodyY
            width: parent.width - 2 * panel.theme.sweepInset
            height: parent.height - panel.theme.plateBodyY - panel.theme.sweepInset
            theme: panel.theme
            range: panel.controller.revision >= 0 ? panel.sweepRange() : ({ lo: 0, hi: 1, unit: "ms", log: false })
            leftValue: panel.sweepLeft()
            rightValue: panel.sweepRight()
            shape: panel.shape
            stereoDegrees: panel.usesStereo ? panel.stereo : panel.mode === 1 ? 240 : 180
            phase: panel.previewPhase
            live: panel.isLive
            lfoLeft: panel.isLive ? panel.live.lfo_left : 0
            lfoRight: panel.isLive ? panel.live.lfo_right : 0
        }
        Caption {
            x: panel.theme.sweepCaptionX
            y: panel.theme.sweepCaptionY
            text: "LFO · " + (panel.shape === 1 ? "TRIANGLE" : "SINE") + " · TWO CYCLES"
        }
    }

    // ---- Controls -------------------------------------------------------------------

    Plate {
        id: motion
        objectName: "orbitPlate:motion"
        x: panel.margin
        y: panel.theme.controlsY
        width: panel.theme.motionWidth
        height: panel.theme.controlsHeight
        title: "MOTION"
        note: "LFO"
        Row {
            x: panel.theme.plateContentX
            y: panel.theme.plateKnobsY
            spacing: panel.theme.spaceMd
            Cell {
                parameterId: "orbit.rate"
                inactive: panel.synced
            }
            Cell {
                parameterId: "orbit.depth"
            }
            Cell {
                parameterId: "orbit.stereo"
                inactive: !panel.usesStereo
            }
        }
        Column {
            x: panel.theme.motionChoicesX
            y: panel.theme.motionChoicesY
            spacing: panel.theme.spaceSm
            Caption { text: "SYNC" }
            Kit.Segment {
                width: panel.theme.motionChoiceWidth
                controller: panel.controller
                theme: panel.theme
                parameterId: "orbit.sync"
            }
            Caption { text: "DIVISION" }
            Item {
                width: panel.theme.motionChoiceWidth
                height: panel.theme.dropdownHeight
                opacity: panel.synced ? 1 : panel.theme.unsyncedOpacity
                Kit.Dropdown {
                    anchors.fill: parent
                    controller: panel.controller
                    theme: panel.theme
                    menu: menu
                    parameterId: "orbit.division"
                }
            }
            Caption { text: "SHAPE" }
            Kit.Segment {
                width: panel.theme.motionChoiceWidth
                controller: panel.controller
                theme: panel.theme
                parameterId: "orbit.shape"
            }
        }
        Caption {
            x: panel.theme.plateContentX
            y: panel.theme.motionFootY
            text: panel.synced ? "RATE FOLLOWS THE TEMPO" : "FREE RUNNING"
        }
    }

    Plate {
        id: voicePlate
        objectName: "orbitPlate:voice"
        x: motion.x + motion.width + panel.theme.spaceLg
        y: motion.y
        width: panel.theme.voiceWidth
        height: motion.height
        title: "VOICE"
        note: panel.theme.modeNames[panel.mode]
        Row {
            id: voiceKnobs
            x: panel.theme.plateContentX
            y: panel.theme.plateKnobsY
            spacing: panel.theme.spaceMd
            Cell {
                objectName: "orbitCell:orbit.time"
                parameterId: "orbit.time"
                visible: panel.usesTime
                label: panel.timeLabel()
            }
            Cell {
                objectName: "orbitCell:orbit.feedback"
                parameterId: "orbit.feedback"
                visible: panel.usesFeedback
            }
            Cell {
                objectName: "orbitCell:orbit.centre"
                parameterId: "orbit.centre"
                visible: panel.usesPhaser
            }
        }
        Column {
            x: panel.theme.plateContentX
            y: panel.theme.plateLowerY
            spacing: panel.theme.spaceSm
            Caption {
                text: "VOICES"
                visible: panel.usesVoices
            }
            Kit.Segment {
                width: panel.theme.voiceChoiceWidth
                visible: panel.usesVoices
                controller: panel.controller
                theme: panel.theme
                parameterId: "orbit.voices"
            }
            Caption {
                text: "STAGES"
                visible: panel.usesPhaser
            }
            Kit.Segment {
                width: panel.theme.voiceChoiceWidth
                visible: panel.usesPhaser
                controller: panel.controller
                theme: panel.theme
                parameterId: "orbit.stages"
            }
        }
        // Modes with nothing to set here say what they are instead.
        Text {
            objectName: "orbitVoiceNote"
            x: panel.theme.voiceNoteX
            y: panel.theme.voiceNoteY
            width: parent.width - 2 * panel.theme.voiceNoteX
            visible: !panel.usesTime && !panel.usesFeedback && !panel.usesPhaser
            wrapMode: Text.WordWrap
            text: panel.mode === 1
                ? "Three taps on one line, 120° apart, swept slow and shimmered fast: the string machine's ensemble. Depth sets how far they drift."
                : "Level, not time: the LFO rides the gain. At a 180° stereo offset the channels trade places and it pans."
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeLabel
            lineHeight: panel.theme.voiceNoteLineHeight
        }
    }

    Plate {
        id: outputPlate
        objectName: "orbitPlate:output"
        x: voicePlate.x + voicePlate.width + panel.theme.spaceLg
        y: motion.y
        width: panel.width - x - panel.margin
        height: motion.height
        title: "OUTPUT"
        Row {
            x: panel.theme.outputKnobsX
            y: panel.theme.plateKnobsY
            spacing: panel.theme.spaceXs
            Cell {
                parameterId: "orbit.tone"
                diameter: panel.theme.outputCellDiameter
            }
            Cell {
                parameterId: "orbit.mix"
                diameter: panel.theme.outputCellDiameter
            }
            Cell {
                parameterId: "orbit.output"
                diameter: panel.theme.outputCellDiameter
            }
        }
        Caption {
            x: panel.theme.plateContentX
            y: panel.theme.plateLowerY
            width: parent.width - 2 * panel.theme.plateContentX
            wrapMode: Text.WordWrap
            text: "TONE DARKENS ONLY THE WET PATH, LIKE A BUCKET-BRIGADE LINE"
        }
    }

    Timer {
        objectName: "orbitPreviewClock"
        interval: 33
        repeat: true
        running: panel.visible && panel.animate && !panel.isLive
        onTriggered: panel.previewPhase = (panel.previewPhase
            + Math.min(panel.previewRate(), 3) * interval / 1000) % 1
    }
    Timer {
        objectName: "orbitTelemetryPoll"
        interval: panel.isLive ? 33 : 250
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
