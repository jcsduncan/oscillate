pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Expanse device panel, "night sky": a midnight case under aurora greens
// and violets. The space leads: its six kinds on the left, the decay
// landscape (energy over time and frequency, computed from the settings as
// the reverb designs them) at the centre, Size, Decay, Pre-Delay and Freeze
// on the right; below, damping, diffusion, motion and shimmer, then levels,
// tone, the live sky light and output. Every control reads and writes
// through `controller` (NativeSynthDeviceController for Expanse); no project
// or audio state lives in QML.
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037, layout in expanse.hpp), polled while visible:
    // fast while the reverb publishes, slowly while it does not.
    property var live: ({ playing: false, input: 0, early: 0, late: 0 })
    property ExpanseTheme theme: ExpanseTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap

    objectName: "expansePanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.skyTop }
        GradientStop { position: panel.theme.skyMidStop; color: panel.theme.sky }
        GradientStop { position: 1; color: panel.theme.skyBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Expanse reverb"

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
    function decode(state) {
        const frame = state.frame
        if (frame === undefined || frame.length < 3)
            return { playing: false, input: 0, early: 0, late: 0 }
        return { playing: true, input: frame[0], early: frame[1], late: frame[2] }
    }
    function db(level) {
        return level > 0 ? 20 * Math.log10(level) : -120
    }

    // The night: a deterministic scatter of stars and two faint aurora
    // ribbons across the top of the case.
    Canvas {
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = panel.theme
            let state = 7 >>> 0
            const next = () => {
                state = (Math.imul(state, 1664525) + 1013904223) >>> 0
                return (state >>> 8) / 16777216
            }
            for (let i = 0; i < 170; ++i) {
                const x = next() * width
                const y = next() * height
                const r = t.starMinRadius + next() * t.starRadiusRange
                ctx.fillStyle = Qt.rgba(t.star.r, t.star.g, t.star.b, t.starMinAlpha + next() * t.starAlphaRange)
                ctx.beginPath()
                ctx.arc(x, y, r, 0, Math.PI * 2)
                ctx.fill()
            }
            for (const [base, colour, alpha] of [[t.ribbonY, t.aurora, t.ribbonAlpha], [t.ribbonVioletY, t.auroraViolet, t.ribbonVioletAlpha]]) {
                const g = ctx.createLinearGradient(0, 0, width, 0)
                g.addColorStop(0, Qt.rgba(colour.r, colour.g, colour.b, 0))
                g.addColorStop(t.ribbonPeakStop, Qt.rgba(colour.r, colour.g, colour.b, alpha))
                g.addColorStop(1, Qt.rgba(colour.r, colour.g, colour.b, 0))
                ctx.strokeStyle = g
                ctx.lineWidth = t.ribbonWidth
                ctx.beginPath()
                for (let x = 0; x <= width; x += t.ribbonStep) {
                    const y = base + t.ribbonSwell * Math.sin(x / t.ribbonSwellLength + base)
                        + t.ribbonRipple * Math.sin(x / t.ribbonRippleLength)
                    if (x === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
        }
    }

    // A glass section with a fine starlit edge, titled in widely spaced caps.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        radius: panel.theme.radiusXl
        color: panel.theme.glassFill
        border.color: panel.theme.plateEdge
        Rectangle {
            x: panel.theme.hairline
            y: panel.theme.hairline
            width: parent.width - 2 * panel.theme.hairline
            height: panel.theme.hairline
            radius: panel.theme.hairline
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.sheenClear }
                GradientStop { position: 0.5; color: panel.theme.sheen }
                GradientStop { position: 1; color: panel.theme.sheenClear }
            }
        }
        Text {
            id: plateTitle
            x: panel.theme.cardTitleX
            y: panel.theme.cardTitleY
            text: plate.title
            color: panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.cardTitleSize
            font.bold: true
            font.letterSpacing: panel.theme.plateTitleSpacing
        }
        Text {
            x: plateTitle.x + plateTitle.implicitWidth + panel.theme.cardNoteGap
            y: panel.theme.plateNoteY
            visible: plate.note !== ""
            text: plate.note
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.letterSpacing: panel.theme.noteSpacing
        }
    }

    component Cell: Item {
        id: cell
        required property string parameterId
        property int diameter: panel.theme.knobDiameter
        property string label: ""
        property bool readout: false
        width: panel.theme.cellWidth
        height: diameter + (readout ? panel.theme.cellReadoutHeightPad : panel.theme.cellHeightPad)
        Kit.Knob {
            id: cellKnob
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: cell.parameterId
            diameter: cell.diameter
            readout: cell.readout
            label: cell.label !== "" ? cell.label : cellKnob.info.name !== undefined ? cellKnob.info.name : ""
        }
    }

    // A plate holding a centred row of knobs.
    component KnobPlate: Plate {
        id: knobPlate
        required property var parameters
        property var labels: []
        Row {
            id: knobRow
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.plateHeaderHeight + (knobPlate.height - panel.theme.plateHeaderHeight - knobRow.height) / 2
            Repeater {
                model: knobPlate.parameters
                delegate: Cell {
                    required property string modelData
                    required property int index
                    parameterId: modelData
                    label: knobPlate.labels.length > index ? knobPlate.labels[index] : ""
                }
            }
        }
    }

    // ---- Header ---------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: panel.theme.spaceXxs
            text: "EXPANSE"
            color: panel.theme.ink
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Rectangle {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.wordmarkRuleGap
            width: wordmark.implicitWidth - panel.theme.wordmarkRuleInset
            height: panel.theme.wordmarkRuleHeight
            radius: panel.theme.wordmarkRuleHeight / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.aurora }
                GradientStop { position: panel.theme.wordmarkRuleMidStop; color: panel.theme.auroraTeal }
                GradientStop { position: 1; color: panel.theme.wordmarkRuleFade }
            }
        }
        Text {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "ALGORITHMIC REVERB"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.subtitleSpacing
        }

        // Preset browser and readout.
        Row {
            id: browser
            x: panel.theme.presetX
            y: panel.theme.presetY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "expansePresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "expansePresetDisplay"
                width: panel.theme.presetWidth
                height: panel.theme.presetHeight
                radius: panel.theme.radiusXl
                color: panel.theme.presetFill
                border.color: activeFocus ? panel.theme.aurora : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.focusStroke : panel.theme.hairline
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", edited" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(display, panel.controller, panel.theme.aurora)
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
                    font.letterSpacing: panel.theme.presetCaptionSpacing
                }
                Text {
                    objectName: "expansePresetName"
                    x: panel.theme.presetTextX
                    y: panel.theme.presetNameY
                    width: panel.theme.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeHeading
                    font.letterSpacing: panel.theme.presetNameSpacing
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
                    objectName: "expanseReadout"
                    x: panel.theme.readoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "expanseReadoutValue"
                    x: panel.theme.readoutX
                    y: panel.theme.readoutValueY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.aurora
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeSubtitle
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
                objectName: "expansePresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "expanseStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- The space ------------------------------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Plate {
        id: space
        title: "SPACE"
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.spaceWidth
        height: panel.rowAHeight
        Kit.Segment {
            x: panel.theme.plateInset
            y: panel.theme.spaceListY
            width: space.width - 2 * panel.theme.plateInset
            height: space.height - panel.theme.spaceListY - panel.theme.plateInset
            vertical: true
            controller: panel.controller
            theme: panel.theme
            parameterId: "expanse.mode"
            labels: ["ROOM", "HALL", "PLATE", "CHAMBER", "CATHEDRAL", "CLOUD"]
            captions: ["Close walls, quick and woody", "Concert hall, warm and wide",
                       "Steel plate, dense and bright", "Stone chamber, focused",
                       "Vast nave, slow to fade", "Endless, blooming haze"]
        }
    }

    Plate {
        id: landscapePlate
        title: "DECAY LANDSCAPE"
        note: "ENERGY OVER TIME AND FREQUENCY · STARS ARE EARLY REFLECTIONS"
        x: space.x + space.width + panel.gap
        y: panel.rowAY
        width: panel.theme.landscapeWidth
        height: panel.rowAHeight
        ExpanseLandscape {
            id: landscape
            x: panel.theme.spaceMd
            y: panel.theme.landscapeY
            width: landscapePlate.width - 2 * panel.theme.spaceMd
            height: landscapePlate.height - panel.theme.landscapeY - panel.theme.spaceSm
            controller: panel.controller
            theme: panel.theme
            live: panel.live.playing === true
            earlyDb: panel.db(panel.live.early)
            lateDb: panel.db(panel.live.late)
        }
    }

    Plate {
        id: dimensions
        title: "DIMENSIONS"
        x: landscapePlate.x + landscapePlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.dimensionsKnobY
            Cell {
                parameterId: "expanse.size"
                diameter: panel.theme.bigKnobDiameter
                width: panel.theme.bigCellWidth
                readout: true
            }
            Cell {
                parameterId: "expanse.decay"
                diameter: panel.theme.bigKnobDiameter
                width: panel.theme.bigCellWidth
                readout: true
            }
        }
        Cell {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.predelayY
            parameterId: "expanse.predelay"
            diameter: panel.theme.smallKnobDiameter
            readout: true
        }
        ExpanseToggle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: dimensions.height - height - panel.theme.toggleMargin
            width: dimensions.width - 2 * panel.theme.toggleMargin
            controller: panel.controller
            theme: panel.theme
            parameterId: "expanse.freeze"
            onCaption: "HOLDING · INPUT CLOSED"
            offCaption: "HOLD THE TAIL FOREVER"
        }
    }

    // ---- Character ------------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: theme.rowBHeight
    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: theme.rowCHeight

    KnobPlate {
        id: damping
        title: "DAMPING"
        note: "LOW · MID · HIGH DECAY"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.dampingWidth
        height: panel.rowBHeight
        parameters: ["expanse.low_decay", "expanse.low_freq", "expanse.high_freq", "expanse.high_decay"]
        labels: ["Low", "Low Freq", "High Freq", "High"]
    }
    KnobPlate {
        id: diffusion
        title: "DIFFUSION"
        x: damping.x + damping.width + panel.gap
        y: panel.rowBY
        width: panel.theme.pairPlateWidth
        height: panel.rowBHeight
        parameters: ["expanse.early_diffusion", "expanse.late_diffusion"]
        labels: ["Early", "Late"]
    }
    KnobPlate {
        id: motion
        title: "MOTION"
        x: diffusion.x + diffusion.width + panel.gap
        y: panel.rowBY
        width: panel.theme.pairPlateWidth
        height: panel.rowBHeight
        parameters: ["expanse.mod_depth", "expanse.mod_rate"]
        labels: ["Depth", "Rate"]
    }
    Plate {
        id: shimmerPlate
        title: "SHIMMER"
        note: "PITCH-SHIFTED INSIDE THE LOOP"
        x: motion.x + motion.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        Cell {
            x: panel.theme.spaceLg
            y: panel.theme.shimmerCellY
            parameterId: "expanse.shimmer"
            label: "Amount"
            diameter: panel.theme.shimmerKnobDiameter
            width: panel.theme.shimmerCellWidth
        }
        Text {
            x: panel.theme.shimmerTextX
            y: panel.theme.intervalCaptionY
            text: "INTERVAL · SEMITONES"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.intervalCaptionSpacing
        }
        Kit.Segment {
            x: panel.theme.shimmerTextX
            y: panel.theme.intervalY
            width: shimmerPlate.width - panel.theme.shimmerTextX - panel.theme.spaceXl
            height: panel.theme.intervalHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "expanse.shimmer_pitch"
            labels: ["−12", "+5", "+7", "+12", "+19", "+24"]
        }
        Text {
            x: panel.theme.shimmerTextX
            y: panel.theme.shimmerNoteY
            width: shimmerPlate.width - panel.theme.shimmerTextX - panel.theme.spaceXl
            text: "Octaves and fifths rise through the tail as it rings — the heavens of Cathedral and Hall."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }

    KnobPlate {
        id: levels
        title: "LEVELS"
        x: panel.margin
        y: panel.rowCY
        width: panel.theme.levelsWidth
        height: panel.rowCHeight
        parameters: ["expanse.early", "expanse.late", "expanse.width"]
    }
    KnobPlate {
        id: tonePlate
        title: "TONE"
        x: levels.x + levels.width + panel.gap
        y: panel.rowCY
        width: panel.theme.pairPlateWidth
        height: panel.rowCHeight
        parameters: ["expanse.low_cut", "expanse.high_cut"]
    }
    Plate {
        id: light
        title: "SKY LIGHT"
        note: panel.live.playing === true ? "LIVE" : "NOT PLAYING"
        x: tonePlate.x + tonePlate.width + panel.gap
        y: panel.rowCY
        width: panel.theme.lightWidth
        height: panel.rowCHeight
        Column {
            x: panel.theme.spaceXl
            y: panel.theme.meterColumnY
            width: light.width - 2 * panel.theme.spaceXl
            spacing: panel.theme.spaceMd
            ExpanseMeter {
                width: parent.width
                theme: panel.theme
                label: "Input"
                live: panel.live.playing === true
                level: panel.live.input
                tone: panel.theme.ice
            }
            ExpanseMeter {
                width: parent.width
                theme: panel.theme
                label: "Early"
                live: panel.live.playing === true
                level: panel.live.early
                tone: panel.theme.auroraTeal
            }
            ExpanseMeter {
                width: parent.width
                theme: panel.theme
                label: "Late"
                live: panel.live.playing === true
                level: panel.live.late
                tone: panel.theme.auroraViolet
            }
        }
    }
    KnobPlate {
        id: output
        title: "OUTPUT"
        x: light.x + light.width + panel.gap
        y: panel.rowCY
        width: panel.width - panel.margin - x
        height: panel.rowCHeight
        parameters: ["expanse.mix", "expanse.output"]
    }

    Timer {
        objectName: "expanseTelemetryPoll"
        interval: panel.live.playing === true ? 33 : 250
        repeat: true
        running: panel.visible
        onTriggered: panel.live = panel.decode(panel.controller.telemetry())
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
