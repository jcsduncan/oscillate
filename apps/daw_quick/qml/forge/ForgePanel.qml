pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Forge device panel: blackened, brushed steel lit from below by the
// forge. The transfer curve leads, drawn as molten metal that glows hotter
// as the drive rises, with the input's peak riding it live; to its left the
// eight curves, to its right the input and output meters and the auto-gain
// makeup; below, the heat, tone and output controls. Every control reads and
// writes through `controller` (NativeSynthDeviceController for Forge); no
// project or audio state lives in QML.
Rectangle {
    id: panel
    required property var controller
    // Live effect state (ADR-0037), polled while the panel is visible.
    property var live: ({ live: false })
    property ForgeTheme theme: ForgeTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.cardGap
    // The engine's transfer curve: as drawn, and finer for the analysis.
    readonly property int curveSamples: 129
    readonly property int analysisSamples: 513
    // The selector glyph's sample count.
    readonly property int glyphSamples: 32
    // Pixel centres for the steel grain.
    readonly property real pixelCentre: 0.5
    readonly property bool isLive: live.live === true
    readonly property real heat: controller.revision >= 0 ? controller.normalised("forge.drive") : 0
    readonly property var curvePoints: isLive && live.curve !== undefined ? live.curve
        : controller.revision >= 0 ? controller.forge_curve(panel.curveSamples) : []
    readonly property real makeupDb: isLive ? live.makeup_db
        : controller.revision >= 0 ? controller.forge_makeup_db() : 0
    readonly property bool autoGain: controller.revision >= 0 && controller.choice("forge.auto_gain") === 1

    objectName: "forgePanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.steelTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.steel }
        GradientStop { position: 1; color: panel.theme.steelBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Forge saturator"

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
    function modelName() {
        const info = controller.parameter("forge.model")
        const index = controller.revision >= 0 ? controller.choice("forge.model") : 0
        return info.choices !== undefined && info.choices[index] !== undefined ? info.choices[index] : ""
    }

    // Each curve's shape as a glyph, for the selector (a sketch only: the
    // view itself draws the engine's own curve).
    function glyph(ctx, index, w, h, colour) {
        const shapes = [
            u => Math.tanh(u),
            u => u >= 0 ? Math.tanh(u) : Math.tanh(0.5 * u) / 0.5 * 0.55,
            u => u / Math.sqrt(1 + u * u),
            u => Math.sign(u) * (1 - Math.exp(-Math.abs(u))),
            u => Math.max(-1, Math.min(1, u)),
            u => { const t = (u + 1) / 4; const s = t - Math.floor(t + 0.5); return 4 * Math.abs(s) - 1 },
            u => Math.sin(u),
            u => Math.abs(Math.tanh(u)) * 2 - 1
        ]
        const drive = [2.5, 2.5, 2.8, 2.6, 1.6, 3.2, 3.6, 2.2][index]
        ctx.strokeStyle = Qt.rgba(colour.r, colour.g, colour.b, theme.glyphAxisAlpha)
        ctx.lineWidth = theme.hairline
        ctx.beginPath()
        ctx.moveTo(0, h / 2)
        ctx.lineTo(w, h / 2)
        ctx.stroke()
        ctx.strokeStyle = colour
        ctx.lineWidth = theme.glyphStroke
        ctx.lineJoin = "round"
        ctx.beginPath()
        for (let i = 0; i <= glyphSamples; ++i) {
            const x = -1 + 2 * i / glyphSamples
            const y = Math.max(-1, Math.min(1, shapes[index](drive * x)))
            const px = i / glyphSamples * w
            const py = h / 2 - y * (h / 2 - theme.glyphInset)
            if (i === 0)
                ctx.moveTo(px, py)
            else
                ctx.lineTo(px, py)
        }
        ctx.stroke()
    }

    // Brushed steel: fine horizontal grain, and the forge's warmth rising
    // from the bottom edge.
    Canvas {
        id: steelGrain
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = panel.theme
            const b = t.brush
            const step = t.grainStep
            let seed = 7
            for (let y = step; y < height - step; y += step) {
                seed = (seed * 1103515245 + 12345) % 2147483648
                const alpha = t.grainAlpha + t.grainAlphaSpread * (seed / 2147483648)
                ctx.strokeStyle = Qt.rgba(b.r, b.g, b.b, alpha)
                ctx.beginPath()
                ctx.moveTo(t.grainInset, y + panel.pixelCentre)
                ctx.lineTo(width - t.grainInset, y + panel.pixelCentre)
                ctx.stroke()
            }
            const e = t.ember
            const warm = ctx.createLinearGradient(0, height, 0, height * t.warmthTop)
            warm.addColorStop(0, Qt.rgba(e.r, e.g, e.b, t.warmthAlpha + t.warmthHeatAlpha * panel.heat))
            warm.addColorStop(1, Qt.rgba(e.r, e.g, e.b, 0))
            ctx.fillStyle = warm
            ctx.fillRect(0, height * t.warmthTop, width, height * (1 - t.warmthTop))
        }
        Connections {
            target: panel
            function onHeatChanged() { steelGrain.requestPaint() }
        }
    }

    // A steel plate, riveted in copper, titled in amber.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        radius: panel.theme.radiusLg
        color: Qt.rgba(panel.theme.plate.r, panel.theme.plate.g, panel.theme.plate.b, panel.theme.plateAlpha)
        border.color: panel.theme.plateEdge
        Repeater {
            model: 4
            delegate: Rectangle {
                required property int index
                x: index % 2 === 0 ? panel.theme.rivetInset
                    : plate.width - panel.theme.rivetInset - panel.theme.rivetSize
                y: index < 2 ? panel.theme.rivetInset
                    : plate.height - panel.theme.rivetInset - panel.theme.rivetSize
                width: panel.theme.rivetSize
                height: panel.theme.rivetSize
                radius: width / 2
                color: panel.theme.copperLo
                border.color: panel.theme.copper
                border.width: panel.theme.rivetStroke
            }
        }
        Text {
            id: plateTitle
            x: panel.theme.plateTitleX
            y: panel.theme.plateTitleY
            text: plate.title
            color: panel.theme.amber
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeCaption
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
        property int diameter: panel.theme.knobDiameter
        property bool heated: false
        property string label: ""
        width: Math.max(panel.theme.cellMinWidth, diameter + panel.theme.cellWidthPad)
        height: diameter + panel.theme.cellHeightPad
        Kit.Knob {
            id: cellKnob
            anchors.horizontalCenter: parent.horizontalCenter
            controller: panel.controller
            theme: panel.theme
            parameterId: cell.parameterId
            diameter: cell.diameter
            arc: cell.heated
                ? panel.theme.heat(panel.theme.knobHeatBase + panel.theme.knobHeatRange * cellKnob.position)
                : panel.theme.amber
            label: cell.label !== "" ? cell.label : cellKnob.info.name !== undefined ? cellKnob.info.name : ""
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
            y: 0
            text: "FORGE"
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.bold: true
            font.letterSpacing: panel.theme.wordmarkSpacing
            color: panel.theme.heat(panel.theme.heatGlowBase + panel.theme.heatGlowRange * panel.heat)
            style: Text.Raised
            styleColor: panel.theme.wordmarkShadow
        }
        Rectangle {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.ruleGap
            width: wordmark.implicitWidth - panel.theme.ruleTrim
            height: panel.theme.strokeBold
            radius: height / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.amber }
                GradientStop { position: panel.theme.ruleEmberStop; color: panel.theme.ember }
                GradientStop { position: 1; color: Qt.rgba(panel.theme.ember.r, panel.theme.ember.g, panel.theme.ember.b, 0) }
            }
        }
        Text {
            x: wordmark.x + panel.theme.spaceXxs
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "ANTIALIASED SATURATOR · 16× OVERSAMPLED"
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
                objectName: "forgePresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "forgePresetDisplay"
                width: panel.theme.presetWidth
                height: panel.theme.presetHeight
                radius: panel.theme.wellRadius
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.amber : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.focusStroke : panel.theme.hairline
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", edited" : "")
                function openPresets() {
                    if (panel.controller.editable)
                        menu.openPresets(display, panel.controller, panel.theme.amber)
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
                    font.letterSpacing: panel.theme.capsTightSpacing
                }
                Text {
                    objectName: "forgePresetName"
                    x: panel.theme.presetTextX
                    y: panel.theme.presetNameY
                    width: panel.theme.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeTitle
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
                    objectName: "forgeReadout"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.presetReadoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.presetReadoutSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "forgeReadoutValue"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetValueY
                    width: display.width - x - panel.theme.presetReadoutRight
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.amber
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
                    color: panel.theme.copper
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
                objectName: "forgePresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "forgeStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- The forge ------------------------------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Plate {
        id: curves
        title: "CURVE"
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.curveCardWidth
        height: panel.rowAHeight
        Kit.Segment {
            x: panel.theme.plateInset
            y: panel.theme.plateBodyY
            width: curves.width - 2 * panel.theme.plateInset
            height: curves.height - panel.theme.plateBodyY - panel.theme.plateInset
            vertical: true
            controller: panel.controller
            theme: panel.theme
            parameterId: "forge.model"
            glyph: panel.glyph
            captions: ["Soft tanh, warm", "Asymmetric, even harmonics", "Firm knee",
                       "Exponential, gritty", "Hard ceiling", "Triangle folder",
                       "Sine folder, glassy", "Full wave, octave up"]
        }
    }

    Plate {
        id: forge
        title: "THE FORGE"
        note: panel.isLive ? "" : "PLAY THROUGH IT TO SEE THE INPUT"
        x: curves.x + curves.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x - panel.gap - panel.theme.levelsCardWidth
        height: panel.rowAHeight
        ForgeCurveView {
            x: panel.theme.plateInset
            y: panel.theme.curveViewY
            width: forge.width - 2 * panel.theme.plateInset
            height: forge.height - panel.theme.curveViewY - panel.theme.plateInset
            theme: panel.theme
            points: panel.curvePoints
            analysis: panel.controller.revision >= 0 ? panel.controller.forge_curve(panel.analysisSamples) : []
            heat: panel.heat
            live: panel.isLive
            inputPeak: panel.isLive ? panel.live.input_rms * Math.SQRT2 : 0
            outputLevel: panel.isLive ? panel.live.output_rms : 0
            modelName: panel.controller.revision >= 0 ? panel.modelName() : ""
        }
    }

    Plate {
        id: levels
        title: "LEVELS"
        x: forge.x + forge.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight
        Row {
            x: panel.theme.meterRowX
            y: panel.theme.meterRowY
            spacing: panel.theme.meterRowGap
            ForgeMeter {
                theme: panel.theme
                label: "In"
                height: panel.theme.levelMeterHeight
                live: panel.isLive
                level: panel.isLive ? panel.live.input_rms : 0
            }
            ForgeMeter {
                theme: panel.theme
                label: "Out"
                height: panel.theme.levelMeterHeight
                live: panel.isLive
                level: panel.isLive ? panel.live.output_rms : 0
            }
        }
        Text {
            x: panel.theme.spaceXl
            y: panel.theme.autoGainY
            text: "AUTO GAIN"
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.labelSpacing
        }
        Text {
            objectName: "forgeMakeup"
            x: panel.theme.spaceXl
            y: panel.theme.makeupY
            width: levels.width - 2 * panel.theme.spaceXl
            text: panel.autoGain ? (panel.makeupDb >= 0 ? "+" : "−") + Math.abs(panel.makeupDb).toFixed(1) + " dB"
                                 : "OFF"
            color: panel.autoGain ? panel.theme.molten : panel.theme.inkFaint
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.makeupSize
            font.bold: true
        }
        Kit.Segment {
            x: panel.theme.spaceXl
            y: panel.theme.autoGainSwitchY
            width: levels.width - 2 * panel.theme.spaceXl
            height: panel.theme.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "forge.auto_gain"
        }
    }

    // ---- Controls -------------------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: implicitHeight - rowBY - margin

    Plate {
        id: heatPlate
        title: "HEAT"
        note: "DRIVE INTO THE CURVE"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.heatCardWidth
        height: panel.rowBHeight
        Row {
            x: panel.theme.heatKnobsX
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: panel.theme.plateBodyOffset
            spacing: panel.theme.heatKnobGap
            Cell { parameterId: "forge.drive"; diameter: panel.theme.driveDiameter; heated: true }
            Cell { parameterId: "forge.bias"; diameter: panel.theme.heatSmallDiameter; y: panel.theme.heatSmallDrop }
            Cell { parameterId: "forge.colour"; diameter: panel.theme.heatSmallDiameter; y: panel.theme.heatSmallDrop }
        }
        Text {
            x: panel.theme.heatNoteX
            y: panel.theme.heatNoteY
            width: heatPlate.width - x - panel.theme.spaceXl
            text: "Bias tips the curve for even harmonics. Colour leans the drive into the highs or lows and puts the balance back afterwards."
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.typeCaption
            wrapMode: Text.WordWrap
            lineHeight: panel.theme.noteLineHeight
        }
    }
    Plate {
        id: tonePlate
        title: "TONE"
        x: heatPlate.x + heatPlate.width + panel.gap
        y: panel.rowBY
        width: panel.theme.toneCardWidth
        height: panel.rowBHeight
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: panel.theme.plateBodyOffset
            Cell { parameterId: "forge.tone"; diameter: panel.theme.cellDiameter }
        }
    }
    Plate {
        id: outputPlate
        title: "OUTPUT"
        x: tonePlate.x + tonePlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: panel.theme.plateBodyOffset
            spacing: panel.theme.space2xl
            Cell { parameterId: "forge.mix"; diameter: panel.theme.cellDiameter }
            Cell { parameterId: "forge.output"; diameter: panel.theme.cellDiameter }
        }
    }

    Timer {
        objectName: "forgeTelemetryPoll"
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
