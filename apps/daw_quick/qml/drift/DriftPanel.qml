pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The Drift device panel, "Nebula": a deep-space faceplate lit by soft
// aurora. The grain cloud preview leads, beside the grain controls with the
// Blur macro and the big Freeze and Sync switches; below sit the source,
// amp, filter, LFOs, shimmer reverb and effects. Every control reads and
// writes through `controller` (NativeSynthDeviceController for the Drift
// engine); no project or audio state lives in QML.
Rectangle {
    id: panel
    required property var controller
    // Live engine state (ADR-0037), polled while the panel is visible: fast
    // while the synth publishes, slowly while it does not.
    property var live: ({ live: false })
    property DriftTheme theme: DriftTheme {}

    // The filter type, as the filter view names it.
    readonly property string filterCaption: ["LOW-PASS 12", "LOW-PASS 24", "BAND-PASS 12",
        "HIGH-PASS 12"][controller.revision >= 0 ? controller.choice("drift.filter.type") : 0]
    // The layout: the panel's designed size, its margin and the gap between
    // plates, then each plate's place and the controls' places inside it.
    readonly property real designedWidth: 1280
    readonly property real designedHeight: 806
    readonly property real margin: 16
    readonly property real gap: 10
    readonly property real headerY: 10
    readonly property real headerHeight: 52
    readonly property real browserX: 230
    readonly property real browserY: 4
    readonly property real displayWidth: 520
    readonly property real displayHeight: 42
    readonly property real displayPadX: 14
    readonly property real categoryY: 5
    readonly property real presetNameY: 16
    readonly property real presetNameWidth: 220
    readonly property real dividerX: 248
    readonly property real dividerInset: 8
    readonly property real readoutX: 260
    readonly property real readoutValueY: 17
    readonly property real readoutRightPad: 26
    readonly property real statusY: 13
    // Inside a plate: controls start contentX in and contentTop down (rows
    // of keys and dropdowns a little lower at controlTop); views sit
    // viewInset in from the sides and bottom.
    readonly property real contentX: 14
    readonly property real contentTop: 30
    readonly property real controlTop: 32
    readonly property real viewInset: 12
    readonly property real heroWidth: 612
    readonly property real cloudInset: 10
    readonly property real motionWidth: 112
    readonly property real blurHeight: 92
    readonly property real blurDiameter: 58
    readonly property real switchHeight: 48
    readonly property real grainDividerX: 140
    readonly property real grainKnobsX: 152
    readonly property real pitchRowY: 106
    readonly property real pitchSlotHeight: 60
    readonly property real pitchStepperY: 8
    readonly property real stepperWidth: 84
    readonly property real keysY: 184
    readonly property real keysWidth: 384
    readonly property real keysHeight: 26
    readonly property real sourceWidth: 520
    readonly property real sourceColumnWidth: 150
    readonly property real spectrumY: 132
    readonly property real spectrumBottomInset: 10
    readonly property real sourceKnobsX: 178
    readonly property real partialsRowY: 108
    readonly property real ampWidth: 336
    readonly property real envelopeHeight: 72
    readonly property real ampKnobsY: 110
    readonly property real filterViewWidth: 184
    readonly property real filterControlsX: 206
    readonly property real filterKnobsY: 62
    readonly property real keytrackY: 126
    readonly property real lfoShapeWidth: 104
    readonly property real lfoDivisionX: 126
    readonly property real lfoDivisionWidth: 92
    readonly property real lfoCaptionY: 62
    readonly property real lfoKnobsX: 236
    readonly property real lfoKnobsY: 28
    readonly property real shimmerWidth: 506
    readonly property real decayViewWidth: 160
    readonly property real shimmerKnobsX: 182
    readonly property real effectsWidth: 466
    readonly property real chorusWidth: 160
    readonly property real delayColumnX: 186
    readonly property real delayWidth: 88
    readonly property real delayKnobsX: 280
    readonly property real compactCellWidth: 60
    readonly property real narrowCellWidth: 54
    readonly property real bendY: 40
    readonly property real bendWidth: 70
    readonly property real outputKnobsX: 86
    // Telemetry polling: fast while the synth publishes, slow while it
    // does not (ms).
    readonly property int pollLiveMs: 33
    readonly property int pollIdleMs: 250
    readonly property bool frozen: controller.revision >= 0 && controller.choice("drift.freeze") === 1
    readonly property bool synced: controller.revision >= 0 && controller.choice("drift.sync") === 1

    objectName: "driftPanel"
    implicitWidth: designedWidth
    implicitHeight: designedHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.spaceTop }
        GradientStop { position: panel.theme.spaceStop; color: panel.theme.space }
        GradientStop { position: 1; color: panel.theme.spaceBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Drift granular synthesizer"

    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        return info.name === undefined ? "" : (info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || controller.revision < 0 ? "" : controller.display(id)
    }
    // Draws grain window `index` as a small curve for the shape keys.
    function windowGlyph(ctx, index, w, h, colour) {
        const curve = controller.grain_window(index, theme.glyphSamples)
        if (curve === undefined || curve.length < 2)
            return
        ctx.strokeStyle = colour
        ctx.lineWidth = theme.glyphStroke
        ctx.beginPath()
        for (let i = 0; i < curve.length; ++i) {
            const gx = i / (curve.length - 1) * w
            const gy = h - curve[i] * (h - 1)
            if (i === 0)
                ctx.moveTo(gx, gy)
            else
                ctx.lineTo(gx, gy)
        }
        ctx.stroke()
    }

    // Aurora: soft coloured light drifting behind the glass.
    Canvas {
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const blob = (x, y, r, c, a) => {
                const g = ctx.createRadialGradient(x, y, 0, x, y, r)
                g.addColorStop(0, Qt.rgba(c.r, c.g, c.b, a))
                g.addColorStop(1, Qt.rgba(c.r, c.g, c.b, 0))
                ctx.fillStyle = g
                ctx.fillRect(x - r, y - r, 2 * r, 2 * r)
            }
            for (const light of panel.theme.auroraLights)
                blob(width * light.x, height * light.y, light.radius, light.colour, light.alpha)
        }
    }

    // A glass section with a title.
    component Plate: Rectangle {
        id: plate
        required property string title
        property string note: ""
        property color titleColor: panel.theme.ink
        radius: panel.theme.plateRadius
        color: Qt.rgba(panel.theme.plate.r, panel.theme.plate.g, panel.theme.plate.b, panel.theme.plateAlpha)
        border.color: panel.theme.plateEdge
        Text {
            id: plateTitle
            x: panel.theme.plateTitleX
            y: panel.theme.plateTitleY
            text: plate.title
            color: plate.titleColor
            font.pixelSize: panel.theme.plateTitleSize
            font.weight: Font.DemiBold
            font.letterSpacing: panel.theme.plateTitleSpacing
        }
        Text {
            x: plateTitle.x + plateTitle.implicitWidth + panel.theme.plateNoteGap
            y: panel.theme.plateNoteY
            visible: plate.note !== ""
            text: plate.note
            color: panel.theme.inkFaint
            font.pixelSize: panel.theme.captionSize
            font.bold: true
            font.letterSpacing: panel.theme.captionSpacing
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
        font.pixelSize: panel.theme.captionSize
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }

    // ---- Header ---------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin
        y: panel.headerY
        width: panel.width - 2 * panel.margin
        height: panel.headerHeight

        Text {
            id: wordmark
            y: 0
            text: "DRIFT"
            color: panel.theme.ink
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Rectangle {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height
            width: wordmark.implicitWidth - panel.theme.wordmarkRuleTrim
            height: panel.theme.strokeBold
            radius: height / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.accent }
                GradientStop { position: 0.6; color: panel.theme.accent2 }
                GradientStop { position: 1; color: panel.theme.magenta }
            }
        }
        Text {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.taglineGap
            text: "GRANULAR TEXTURES · NEBULA"
            color: panel.theme.inkDim
            font.pixelSize: panel.theme.captionSize
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }

        // Preset browser and readout.
        Row {
            id: browser
            x: panel.browserX
            y: panel.browserY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "driftPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "driftPresetDisplay"
                width: panel.displayWidth
                height: panel.displayHeight
                radius: panel.theme.radiusXl
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.litEdgeWidth : panel.theme.hairline
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
                    x: panel.displayPadX
                    y: panel.categoryY
                    text: panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.captionSize
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                }
                Text {
                    objectName: "driftPresetName"
                    x: panel.displayPadX
                    y: panel.presetNameY
                    width: panel.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.pixelSize: panel.theme.presetNameSize
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Rectangle {
                    x: panel.dividerX
                    y: panel.dividerInset
                    width: panel.theme.hairline
                    height: parent.height - 2 * panel.dividerInset
                    color: panel.theme.wellEdge
                }
                Text {
                    objectName: "driftReadout"
                    x: panel.readoutX
                    y: panel.categoryY
                    width: display.width - x - panel.readoutRightPad
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.captionSize
                    font.bold: true
                    font.letterSpacing: panel.theme.readoutLabelSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "driftReadoutValue"
                    x: panel.readoutX
                    y: panel.readoutValueY
                    width: display.width - x - panel.readoutRightPad
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.readoutValueSize
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.chevronInset
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.accent
                    font.pixelSize: panel.theme.chevronSize
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
                objectName: "driftPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "driftStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.statusY
        }
    }

    // ---- Cloud and grains -----------------------------------------------------------------

    readonly property real rowAY: 72
    readonly property real rowAHeight: 300

    Plate {
        id: hero
        title: "CLOUD"
        note: panel.frozen ? "FROZEN" : panel.synced ? "SYNCED" : ""
        x: panel.margin
        y: panel.rowAY
        width: panel.heroWidth
        height: panel.rowAHeight
        DriftCloudView {
            x: panel.cloudInset
            y: panel.contentTop
            width: hero.width - 2 * panel.cloudInset
            height: hero.height - panel.contentTop - panel.cloudInset
            controller: panel.controller
            theme: panel.theme
            live: panel.live.live === true
            liveGrains: panel.live.live === true ? panel.live.grains : []
        }
    }

    Plate {
        id: grains
        title: "GRAINS"
        x: hero.x + hero.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight

        // The macro and the big switches.
        Column {
            id: motion
            x: panel.contentX
            y: panel.contentTop
            spacing: panel.theme.motionSpacing
            Item {
                width: panel.motionWidth
                height: panel.blurHeight
                Cell {
                    anchors.horizontalCenter: parent.horizontalCenter
                    parameterId: "drift.blur"
                    diameter: panel.blurDiameter
                    label: "Blur"
                }
            }
            DriftToggle {
                width: panel.motionWidth
                height: panel.switchHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.freeze"
                title: "Freeze"
                caption: panel.frozen ? "HOLDING" : "OFF"
                tone: panel.theme.frost
            }
            DriftToggle {
                width: panel.motionWidth
                height: panel.switchHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.sync"
                title: "Sync"
                caption: panel.synced ? "TEMPO GRID" : "FREE"
                tone: panel.theme.accent2
            }
            Kit.Dropdown {
                width: panel.motionWidth
                height: panel.theme.dropdownHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.sync.division"
                menu: menu
                accent: panel.theme.accent2
                opacity: panel.synced ? 1 : panel.theme.unsyncedOpacity
            }
        }
        Rectangle {
            x: panel.grainDividerX
            y: panel.contentTop
            width: panel.theme.hairline
            height: grains.height - panel.contentTop - panel.viewInset
            color: panel.theme.plateEdge
        }
        // Timing and placement.
        Row {
            x: panel.grainKnobsX
            y: panel.contentTop
            spacing: panel.theme.spaceXxs
            Cell { parameterId: "drift.grain.size" }
            Cell { parameterId: "drift.grain.density" }
            Cell {
                parameterId: "drift.grain.chaos"
                inactive: panel.synced
            }
            Cell { parameterId: "drift.grain.position" }
            Cell { parameterId: "drift.grain.scan" }
            Cell { parameterId: "drift.grain.spray" }
        }
        // Pitch and colour.
        Row {
            x: panel.grainKnobsX
            y: panel.pitchRowY
            spacing: panel.theme.spaceXxs
            Cell { parameterId: "drift.grain.jitter" }
            Cell {
                parameterId: "drift.grain.interval_amount"
                inactive: panel.controller.revision >= 0 && panel.controller.choice("drift.grain.intervals") === 0
            }
            Cell { parameterId: "drift.grain.reverse" }
            Cell { parameterId: "drift.grain.spread" }
            Item {
                width: panel.motionWidth
                height: panel.pitchSlotHeight
                Kit.Stepper {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: panel.pitchStepperY
                    width: panel.stepperWidth
                    height: panel.theme.stepperHeight
                    controller: panel.controller
                    theme: panel.theme
                    parameterId: "drift.grain.pitch"
                    label: "Pitch"
                }
            }
        }
        Column {
            x: panel.grainKnobsX
            y: panel.keysY
            spacing: panel.theme.spaceSm
            Kit.Segment {
                width: panel.keysWidth
                height: panel.keysHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.grain.intervals"
                labels: ["OFF", "OCTAVES", "FIFTHS", "PENTA"]
            }
            Caption { text: "INTERVALS · HARMONY SETS HOW OFTEN" }
            Item { width: 1; height: panel.theme.spaceXxs }
            Kit.Segment {
                width: panel.keysWidth
                height: panel.keysHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.grain.shape"
                labels: ["HANN", "TUKEY", "DECAY", "SWELL"]
                glyph: panel.windowGlyph
            }
            Caption { text: "GRAIN SHAPE" }
        }
    }

    // ---- Source, amp and filter -----------------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: 194

    Plate {
        id: source
        title: "SOURCE"
        note: "WHAT EACH NOTE WRITES"
        x: panel.margin
        y: panel.rowBY
        width: panel.sourceWidth
        height: panel.rowBHeight
        Column {
            x: panel.contentX
            y: panel.controlTop
            spacing: panel.theme.spaceSm
            Kit.Dropdown {
                width: panel.sourceColumnWidth
                height: panel.theme.dropdownHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.wt.table"
                menu: menu
            }
            Caption { text: "WAVETABLE" }
            Item { width: 1; height: panel.theme.spaceXxs }
            Kit.Stepper {
                width: panel.stepperWidth
                height: panel.theme.stepperHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.source.octave"
                label: "Octave"
            }
        }
        DriftSpectrumView {
            x: panel.contentX
            y: panel.spectrumY
            width: panel.sourceColumnWidth
            height: source.height - panel.spectrumY - panel.spectrumBottomInset
            controller: panel.controller
            theme: panel.theme
        }
        Row {
            x: panel.sourceKnobsX
            y: panel.contentTop
            Cell {
                parameterId: "drift.wt.level"
                label: "Wavetable"
            }
            Cell {
                parameterId: "drift.wt.position"
                label: "Position"
            }
            Cell { parameterId: "drift.wt.drift" }
            Cell {
                parameterId: "drift.air.level"
                label: "Air"
            }
            Cell {
                parameterId: "drift.air.width"
                label: "Air Width"
                inactive: panel.controller.revision >= 0 && panel.controller.value("drift.air.level") <= 0
            }
        }
        Row {
            x: panel.sourceKnobsX
            y: panel.partialsRowY
            Cell {
                parameterId: "drift.partials.level"
                label: "Partials"
            }
            Cell { parameterId: "drift.partials.tilt" }
            Cell { parameterId: "drift.partials.odd" }
            Cell { parameterId: "drift.partials.stretch" }
        }
    }

    Plate {
        id: amp
        title: "AMP"
        x: source.x + source.width + panel.gap
        y: panel.rowBY
        width: panel.ampWidth
        height: panel.rowBHeight
        // The amp envelope as the kernel runs it: attack, decay, a short
        // hold at sustain, then release, with the release point marked.
        Kit.EnvelopeView {
            id: ampEnvelope
            readonly property var sketch: panel.controller.revision >= 0
                ? panel.controller.amp_envelope(panel.theme.envelopeSamples) : ({})
            objectName: "driftEnvelopeView"
            x: panel.viewInset
            y: panel.contentTop
            width: amp.width - 2 * panel.viewInset
            height: panel.envelopeHeight
            controller: panel.controller
            theme: panel.theme
            levels: sketch.points !== undefined ? sketch.points : []
            markers: sketch.seconds !== undefined ? [{ x: sketch.release / sketch.seconds }] : []
            Accessible.name: "Amp envelope"
            Text {
                objectName: "driftEnvelopeLength"
                anchors.right: parent.right
                anchors.rightMargin: panel.theme.envelopeLabelInset
                y: panel.theme.envelopeLabelTop
                text: ampEnvelope.sketch.seconds !== undefined ? ampEnvelope.sketch.seconds.toFixed(2) + " s" : ""
                color: panel.theme.envelopeLabelInk
                font.family: panel.theme.envelopeLabelFont
                font.pixelSize: panel.theme.envelopeLabelSize
            }
        }
        Row {
            x: (amp.width - width) / 2
            y: panel.ampKnobsY
            Cell { parameterId: "drift.amp.attack" }
            Cell { parameterId: "drift.amp.decay" }
            Cell { parameterId: "drift.amp.sustain" }
            Cell { parameterId: "drift.amp.release" }
            Cell { parameterId: "drift.amp.velocity" }
        }
    }

    Plate {
        id: filter
        title: "FILTER"
        note: ""
        x: amp.x + amp.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight
        Kit.FilterView {
            caption: panel.filterCaption
            x: panel.viewInset
            y: panel.contentTop
            width: panel.filterViewWidth
            height: filter.height - panel.contentTop - panel.viewInset
            controller: panel.controller
            theme: panel.theme
        }
        Kit.Segment {
            x: panel.filterControlsX
            y: panel.controlTop
            width: filter.width - panel.filterControlsX - panel.viewInset
            height: panel.theme.segmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "drift.filter.type"
            labels: ["LP12", "LP24", "BP", "HP"]
        }
        Row {
            x: panel.filterControlsX + (filter.width - panel.filterControlsX - panel.viewInset - width) / 2
            y: panel.filterKnobsY
            Cell { parameterId: "drift.filter.cutoff" }
            Cell { parameterId: "drift.filter.resonance" }
        }
        Row {
            x: panel.filterControlsX + (filter.width - panel.filterControlsX - panel.viewInset - width) / 2
            y: panel.keytrackY
            Cell {
                parameterId: "drift.filter.keytrack"
                label: "Key"
            }
        }
    }

    // ---- LFOs -------------------------------------------------------------------------------

    readonly property real rowCY: rowBY + rowBHeight + gap
    readonly property real rowCHeight: 100

    component LfoPlate: Plate {
        id: lfoPlate
        required property string number
        required property string firstTarget
        required property string secondTarget
        readonly property string base: "drift.lfo" + number
        readonly property bool lfoSynced: panel.controller.revision >= 0 && panel.controller.choice(base + ".sync") === 1
        y: panel.rowCY
        height: panel.rowCHeight
        Column {
            x: panel.contentX
            y: panel.controlTop
            spacing: panel.theme.spaceSm
            Kit.Dropdown {
                width: panel.lfoShapeWidth
                height: panel.theme.dropdownHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: lfoPlate.base + ".shape"
                menu: menu
            }
            Kit.Segment {
                width: panel.lfoShapeWidth
                height: panel.theme.segmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: lfoPlate.base + ".sync"
                labels: ["FREE", "SYNC"]
            }
        }
        Kit.Dropdown {
            x: panel.lfoDivisionX
            y: panel.controlTop
            width: panel.lfoDivisionWidth
            height: panel.theme.dropdownHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: lfoPlate.base + ".division"
            menu: menu
            opacity: lfoPlate.lfoSynced ? 1 : panel.theme.unsyncedOpacity
        }
        Caption {
            x: panel.lfoDivisionX
            y: panel.lfoCaptionY
            text: "DIVISION"
        }
        Row {
            x: panel.lfoKnobsX
            y: panel.lfoKnobsY
            spacing: panel.theme.spaceMd
            Cell {
                parameterId: lfoPlate.base + ".rate"
                inactive: lfoPlate.lfoSynced
            }
            Cell { parameterId: lfoPlate.base + "." + lfoPlate.firstTarget }
            Cell { parameterId: lfoPlate.base + "." + lfoPlate.secondTarget }
        }
    }
    LfoPlate {
        id: lfo1
        title: "LFO 1"
        note: "MOVES THE CLOUD"
        number: "1"
        firstTarget: "position"
        secondTarget: "density"
        x: panel.margin
        width: (panel.width - 2 * panel.margin - panel.gap) / 2
    }
    LfoPlate {
        id: lfo2
        title: "LFO 2"
        note: "BREATHES THE TONE"
        number: "2"
        firstTarget: "filter"
        secondTarget: "size"
        x: lfo1.x + lfo1.width + panel.gap
        width: lfo1.width
    }

    // ---- Shimmer, effects and output --------------------------------------------------------

    readonly property real rowDY: rowCY + rowCHeight + gap
    readonly property real rowDHeight: 112

    Plate {
        id: shimmer
        title: "SHIMMER"
        note: "OCTAVE-UP REVERB"
        x: panel.margin
        y: panel.rowDY
        width: panel.shimmerWidth
        height: panel.rowDHeight
        DriftDecayView {
            x: panel.viewInset
            y: panel.contentTop
            width: panel.decayViewWidth
            height: shimmer.height - panel.contentTop - panel.viewInset
            controller: panel.controller
            theme: panel.theme
        }
        Row {
            x: panel.shimmerKnobsX
            y: panel.controlTop
            Cell {
                parameterId: "drift.verb.mix"
                label: "Mix"
            }
            Cell { parameterId: "drift.verb.size" }
            Cell { parameterId: "drift.verb.decay" }
            Cell { parameterId: "drift.verb.damping" }
            Cell {
                parameterId: "drift.verb.shimmer"
                label: "Shimmer"
            }
        }
    }

    Plate {
        id: effects
        title: "EFFECTS"
        x: shimmer.x + shimmer.width + panel.gap
        y: panel.rowDY
        width: panel.effectsWidth
        height: panel.rowDHeight
        readonly property bool delaySynced: panel.controller.revision >= 0 && panel.controller.choice("drift.fx.delay_sync") === 1
        Column {
            x: panel.contentX
            y: panel.controlTop
            spacing: panel.theme.spaceSm
            Kit.Segment {
                width: panel.chorusWidth
                height: panel.theme.segmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.fx.chorus"
                labels: ["OFF", "I", "II", "I+II"]
            }
            Caption { text: "CHORUS" }
        }
        Column {
            x: panel.delayColumnX
            y: panel.controlTop
            spacing: panel.theme.spaceSm
            Kit.Segment {
                width: panel.delayWidth
                height: panel.theme.segmentHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.fx.delay_sync"
                labels: ["FREE", "SYNC"]
            }
            Kit.Dropdown {
                width: panel.delayWidth
                height: panel.theme.dropdownHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "drift.fx.delay_division"
                menu: menu
                opacity: effects.delaySynced ? 1 : panel.theme.unsyncedOpacity
            }
        }
        Row {
            x: panel.delayKnobsX
            y: panel.controlTop
            Cell {
                parameterId: "drift.fx.delay"
                label: "Delay"
                width: panel.compactCellWidth
            }
            Cell {
                parameterId: "drift.fx.delay_time"
                inactive: effects.delaySynced
                width: panel.compactCellWidth
            }
            Cell {
                parameterId: "drift.fx.delay_feedback"
                label: "Repeats"
                width: panel.compactCellWidth
            }
        }
    }

    Plate {
        id: output
        title: "PLAY · OUT"
        x: effects.x + effects.width + panel.gap
        y: panel.rowDY
        width: panel.width - panel.margin - x
        height: panel.rowDHeight
        Kit.Stepper {
            x: panel.viewInset
            y: panel.bendY
            width: panel.bendWidth
            height: panel.theme.stepperHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "drift.bend"
            label: "Bend"
        }
        Row {
            x: panel.outputKnobsX
            y: panel.controlTop
            Cell {
                parameterId: "drift.perf.bend"
                label: "P. Bend"
                width: panel.narrowCellWidth
            }
            Cell {
                parameterId: "drift.perf.wheel"
                label: "Wheel"
                width: panel.narrowCellWidth
            }
            Cell {
                parameterId: "drift.out.level"
                label: "Output"
                width: panel.compactCellWidth
            }
        }
    }

    Timer {
        objectName: "driftTelemetryPoll"
        interval: panel.live.live === true ? panel.pollLiveMs : panel.pollIdleMs
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
