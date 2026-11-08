pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts
import Oscillate.Format
import "../devicekit" as Kit

// The Signal device panel: bench test gear in warm grey with an orange
// LCD. The display leads: what is being generated, in large digits (the
// tone's frequency and note, the sweep's live frequency, or the noise's
// colour), its level marked peak or RMS, and a 20 Hz to 20 kHz strip
// showing the tone, the sweep's progress or the noise's slope; beside it
// the power switch, Replace or Add, and the output routing drawn as two
// speakers. Below: the five waves as pictured tiles, the tone controls
// and the timing control the wave uses. Every control reads and writes
// through `controller` (NativeSynthDeviceController for Signal); the live
// frequency and progress come only from the telemetry (layout in
// signal.hpp).
Rectangle {
    id: panel
    required property var controller
    // Live state (ADR-0037), polled while visible.
    property var live: panel.idle()
    property SignalTheme theme: SignalTheme {}

    readonly property real margin: theme.spaceXl
    readonly property real gap: theme.plateGap
    readonly property int revision: controller.revision

    // signal.hpp: the frequency now (Hz; 0 for noise and impulses) and the
    // sweep's progress (0..1).
    readonly property int frameSize: 2
    readonly property real sweepLowHz: 20
    readonly property real sweepHighHz: 20000

    readonly property bool playing: live.playing === true
    readonly property bool on: revision >= 0 && controller.choice("signal.on") === 1
    readonly property int wave: revision >= 0 ? controller.choice("signal.wave") : 0
    readonly property int output: revision >= 0 ? controller.choice("signal.output") : 0
    readonly property bool add: revision >= 0 && controller.choice("signal.mode") === 1
    readonly property real frequency: revision >= 0 ? controller.value("signal.frequency") : 1000
    readonly property real level: revision >= 0 ? controller.value("signal.level") : -18
    readonly property real sweepSeconds: revision >= 0 ? controller.value("signal.sweep_seconds") : 10
    readonly property real impulseRate: revision >= 0 ? controller.value("signal.impulse_rate") : 1
    readonly property bool tonal: wave === 0
    readonly property bool noise: wave === 2 || wave === 3
    readonly property var waveNames: ["SINE", "LOG SWEEP", "PINK NOISE", "WHITE NOISE", "IMPULSES"]
    readonly property var outputNames: ["BOTH", "LEFT", "RIGHT", "ALTERNATE", "RIGHT INVERTED"]
    // What the big digits show: the live sweep when one is running.
    readonly property bool sweeping: wave === 1 && on && playing && live.hz > 0

    objectName: "signalPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.caseTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.caseColor }
        GradientStop { position: 1; color: panel.theme.caseBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Signal"

    function idle() {
        return { playing: false, hz: 0, progress: 0 }
    }
    function decode(state) {
        const f = state.frame
        if (f === undefined || f.length < frameSize)
            return idle()
        return { playing: true, hz: Math.max(0, f[0]), progress: Math.max(0, Math.min(1, f[1])) }
    }

    // ---- Formatting ------------------------------------------------------------------

    // The level holds one decimal, so the figure does not jump in width.
    readonly property int levelDecimals: 1
    function signed(value) {
        return Format.signed_number(value, levelDecimals)
    }
    // A generator's frequency to four significant figures: 40.00 Hz,
    // 997.0 Hz, 1.000 kHz, 15.00 kHz.
    readonly property int frequencyFigures: 4
    function hzDigits(hz) {
        const v = hz >= 1000 ? hz / 1000 : hz
        const whole = v >= 100 ? 3 : v >= 10 ? 2 : 1
        return Format.number(v, frequencyFigures - whole)
    }
    function hzUnit(hz) {
        return hz >= 1000 ? "kHz" : "Hz"
    }
    function hzText(hz) {
        return hzDigits(hz) + " " + hzUnit(hz)
    }
    // The nearest equal-tempered note against A4 = 440 Hz, with cents.
    function noteOf(hz) {
        if (!(hz > 0))
            return ""
        const midi = Contracts.midi_of_hz(hz)
        const nearest = Math.round(midi)
        const cents = Math.round((midi - nearest) * 100)
        const name = Contracts.note_name(nearest, Contracts.sharp, Contracts.scientific_middle_c)
        return name + (cents === 0 ? "" : " " + Format.cents(cents))
    }
    function secondsText(s) {
        return Format.seconds(s)
    }
    function periodText(rate) {
        return Format.seconds(1 / rate)
    }
    // The level's meaning for the wave: peak for tones and impulses, RMS
    // for noise.
    readonly property string levelKind: noise ? "RMS" : "PEAK"

    function readoutName(id) {
        if (id === "")
            return "TOUCH A CONTROL"
        const info = controller.parameter(id)
        if (info.name === undefined)
            return ""
        return (info.group === info.name ? info.name : info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue(id) {
        return id === "" || revision < 0 ? "" : controller.display(id)
    }

    // The display's main line and the line under it.
    readonly property string bigText: !on ? "OFF"
        : wave === 0 ? hzDigits(frequency)
        : wave === 1 ? (sweeping ? hzDigits(live.hz) : "20 → 20k")
        : wave === 2 ? "PINK"
        : wave === 3 ? "WHITE"
        : Format.number(impulseRate)
    readonly property string bigUnit: !on ? ""
        : wave === 0 ? hzUnit(frequency)
        : wave === 1 ? (sweeping ? hzUnit(live.hz) : "Hz")
        : wave === 4 ? "Hz" : ""
    readonly property string subText: !on ? "THE INPUT PASSES UNTOUCHED"
        : wave === 0 ? noteOf(frequency)
        : wave === 1 ? (sweeping ? noteOf(live.hz) + "  ·  " + Format.fraction(live.progress) + " OF "
                                   + secondsText(sweepSeconds)
                                 : "EXPONENTIAL · " + secondsText(sweepSeconds) + " A PASS")
        : wave === 2 ? "−3 dB/oct · EQUAL ENERGY PER OCTAVE"
        : wave === 3 ? "FLAT · EQUAL ENERGY PER HERTZ"
        : "ONE IMPULSE EVERY " + periodText(impulseRate)

    // A plate: a fine bevel along the top, the title in spaced caps.
    component Caption: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeMicro
        font.bold: true
        font.letterSpacing: panel.theme.captionSpacing
    }
    component Note: Text {
        color: panel.theme.inkFaint
        font.pixelSize: panel.theme.typeCaption
        wrapMode: Text.WordWrap
    }
    // A screw head in each corner of the case, as on rack gear.
    component Screw: Rectangle {
        width: panel.theme.screwSize
        height: panel.theme.screwSize
        radius: width / 2
        color: panel.theme.plateHi
        border.color: panel.theme.caseBottom
        Rectangle {
            anchors.centerIn: parent
            width: panel.theme.screwSlotLength
            height: panel.theme.screwSlotWidth
            rotation: panel.theme.screwSlotAngle
            color: panel.theme.caseBottom
        }
    }
    readonly property real screwFar: theme.screwInset + theme.screwSize
    Screw { x: panel.theme.screwInset; y: panel.theme.screwInset }
    Screw { x: panel.width - panel.screwFar; y: panel.theme.screwInset }
    Screw { x: panel.theme.screwInset; y: panel.height - panel.screwFar }
    Screw { x: panel.width - panel.screwFar; y: panel.height - panel.screwFar }

    // ---- Header ----------------------------------------------------------------------

    Item {
        id: header
        x: panel.margin + panel.theme.headerInset
        y: panel.theme.headerY
        width: panel.width - 2 * panel.margin - 2 * panel.theme.headerInset
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: panel.theme.wordmarkY
            text: "SIGNAL"
            color: panel.theme.glacier
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Rectangle {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.underlineGap
            width: wordmark.implicitWidth - panel.theme.underlineTrim
            height: panel.theme.strokeBold
            radius: height / 2
            color: panel.theme.accent
        }
        Text {
            x: wordmark.x + panel.theme.wordmarkInset
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "TEST GENERATOR"
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
                objectName: "signalPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "signalPresetDisplay"
                width: panel.theme.presetDisplayWidth
                height: panel.theme.presetDisplayHeight
                radius: panel.theme.radiusLg
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
                    y: panel.theme.presetCategoryY
                    text: panel.controller.preset_category.toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.captionSpacing
                }
                Text {
                    objectName: "signalPresetName"
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
                    objectName: "signalReadout"
                    x: panel.theme.readoutX
                    y: panel.theme.presetCategoryY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutName(panel.theme.lastTouched)
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.tagSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "signalReadoutValue"
                    x: panel.theme.readoutX
                    y: panel.theme.readoutValueY
                    width: display.width - x - panel.theme.readoutRight
                    text: panel.readoutValue(panel.theme.lastTouched)
                    color: panel.theme.accent
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
                objectName: "signalPresetNext"
                direction: 1
            }
        }

        Kit.StatusBadge {
            id: status
            objectName: "signalStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- The display -----------------------------------------------------------------

    readonly property real rowAY: theme.rowAY
    readonly property real rowAHeight: theme.rowAHeight

    Kit.Card {
        theme: panel.theme
        id: displayPlate
        title: "DISPLAY"
        note: !panel.on ? "OUTPUT OFF" : panel.add ? "ADDED TO THE INPUT" : "REPLACING THE INPUT"
        noteColor: panel.on ? panel.theme.accent : panel.theme.inkFaint
        x: panel.margin
        y: panel.rowAY
        width: panel.theme.displayWidth
        height: panel.rowAHeight

        Rectangle {
            id: lcd
            objectName: "signalLcd"
            x: panel.theme.lcdInset
            y: panel.theme.lcdY
            width: displayPlate.width - 2 * panel.theme.lcdInset
            height: displayPlate.height - panel.theme.lcdY - panel.theme.lcdBottom
            radius: panel.theme.lcdRadius
            color: panel.theme.lcd
            border.color: panel.theme.lcdEdge
            border.width: panel.theme.strokeBold
            // Glass: a faint sheen across the top.
            Rectangle {
                x: lcd.border.width
                y: lcd.border.width
                width: parent.width - 2 * lcd.border.width
                height: parent.height * panel.theme.glassShare
                radius: panel.theme.radiusMd
                gradient: Gradient {
                    GradientStop { position: 0; color: panel.theme.glassSheenTop }
                    GradientStop { position: 1; color: panel.theme.glassSheenBottom }
                }
            }

            SignalGlyph {
                x: panel.theme.lcdTextX
                y: panel.theme.lcdGlyphY
                width: panel.theme.lcdGlyphWidth
                height: panel.theme.lcdGlyphHeight
                wave: panel.wave
                tone: panel.on ? panel.theme.lcdInk : panel.theme.lcdDim
                lineWidth: panel.theme.lcdGlyphStroke
            }
            Text {
                objectName: "signalWaveName"
                x: panel.theme.waveNameX
                y: panel.theme.waveNameY
                text: panel.waveNames[panel.wave]
                color: panel.on ? panel.theme.lcdInk : panel.theme.lcdDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeTitle
                font.bold: true
                font.letterSpacing: panel.theme.waveNameSpacing
            }
            Text {
                objectName: "signalOutputName"
                anchors.right: parent.right
                anchors.rightMargin: panel.theme.lcdTextX
                y: panel.theme.outputNameY
                text: panel.on ? (panel.add ? "ADD" : "REPLACE") + " · " + panel.outputNames[panel.output] : ""
                color: panel.theme.lcdDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeLabel
                font.letterSpacing: panel.theme.lcdLabelSpacing
            }

            // The big digits, with the ghost of every segment behind them.
            Text {
                objectName: "signalBig"
                x: panel.theme.lcdTextX
                y: panel.theme.bigY
                text: panel.bigText
                color: panel.on ? panel.theme.lcdInk : panel.theme.lcdDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.bigSize
                font.letterSpacing: panel.theme.bigSpacing
                style: Text.Raised
                styleColor: panel.on ? Qt.rgba(panel.theme.lcdGlow.r, panel.theme.lcdGlow.g, panel.theme.lcdGlow.b,
                                               panel.theme.bigGlowAlpha)
                                     : "transparent"
                Text {
                    objectName: "signalBigUnit"
                    anchors.left: parent.right
                    anchors.leftMargin: panel.theme.bigUnitGap
                    anchors.baseline: parent.baseline
                    text: panel.bigUnit
                    color: panel.theme.lcdDim
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.bigUnitSize
                }
            }
            Text {
                objectName: "signalSub"
                x: panel.theme.subX
                y: panel.theme.subY
                text: panel.subText
                color: panel.on ? panel.theme.lcdInk : panel.theme.lcdDim
                opacity: panel.theme.subOpacity
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeHeading
                font.letterSpacing: panel.theme.tagSpacing
            }
            // The level, top right of the digits.
            Column {
                anchors.right: parent.right
                anchors.rightMargin: panel.theme.levelRight
                y: panel.theme.levelY
                spacing: panel.theme.spaceXxs
                Text {
                    objectName: "signalLevel"
                    anchors.right: parent.right
                    text: panel.signed(panel.level)
                    color: panel.on ? panel.theme.lcdInk : panel.theme.lcdDim
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.levelSize
                }
                Text {
                    objectName: "signalLevelKind"
                    anchors.right: parent.right
                    text: "dBFS " + panel.levelKind
                    color: panel.theme.lcdDim
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeEmphasis
                    font.letterSpacing: panel.theme.lcdLabelSpacing
                }
            }

            SignalAxis {
                objectName: "signalAxis"
                x: panel.theme.lcdTextX
                y: panel.theme.axisY
                width: lcd.width - 2 * panel.theme.lcdTextX
                height: lcd.height - panel.theme.axisY - panel.theme.axisBottom
                theme: panel.theme
                wave: panel.wave
                on: panel.on
                live: panel.sweeping
                hz: panel.wave === 0 ? panel.frequency : panel.sweeping ? panel.live.hz : 0
                progress: panel.live.progress
            }
        }
    }

    // ---- Output ----------------------------------------------------------------------

    Kit.Card {
        theme: panel.theme
        id: outputPlate
        title: "OUTPUT"
        x: displayPlate.x + displayPlate.width + panel.gap
        y: panel.rowAY
        width: panel.width - panel.margin - x
        height: panel.rowAHeight

        // The power switch: a lit rocker with its legend.
        Rectangle {
            id: power
            objectName: "signalToggle:signal.on"
            x: panel.theme.cardPad
            y: panel.theme.powerY
            width: outputPlate.width - 2 * panel.theme.cardPad
            height: panel.theme.powerHeight
            radius: panel.theme.radiusXl
            activeFocusOnTab: panel.controller.editable
            opacity: panel.controller.editable ? 1 : panel.theme.disabledOpacity
            color: panel.on ? Qt.rgba(panel.theme.power.r, panel.theme.power.g, panel.theme.power.b,
                                      panel.theme.powerFillAlpha)
                : powerArea.pressed ? panel.theme.button : panel.theme.buttonHi
            border.color: activeFocus ? panel.theme.accent
                : panel.on ? Qt.rgba(panel.theme.power.r, panel.theme.power.g, panel.theme.power.b,
                                     panel.theme.powerEdgeAlpha)
                : panel.theme.seam
            border.width: activeFocus ? panel.theme.focusStroke : panel.theme.hairline
            Accessible.role: Accessible.CheckBox
            Kit.HoverTip { theme: panel.theme }
            Accessible.name: "On"
            Accessible.checked: panel.on
            function flip() {
                panel.theme.lastTouched = "signal.on"
                panel.controller.set_choice("signal.on", panel.on ? 0 : 1)
            }
            Keys.onSpacePressed: flip()
            Keys.onReturnPressed: flip()
            Keys.onDeletePressed: {
                panel.controller.reset_to_default("signal.on")
                panel.theme.lastTouched = "signal.on"
            }
            // The power symbol in a lit ring.
            Canvas {
                id: powerGlyph
                // The symbol's drawing in its box: the ring's centre height
                // and radius, the gap at its top (rad either side), and the
                // stem's ends.
                readonly property real ringCentreY: 16
                readonly property real ringRadius: 10
                readonly property real ringGap: 0.6
                readonly property real stemTop: 3
                readonly property real stemBottom: 14
                x: panel.theme.cardPad
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.powerGlyphSize
                height: panel.theme.powerGlyphSize
                renderStrategy: Canvas.Cooperative
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const tone = panel.on ? panel.theme.power : panel.theme.inkFaint
                    ctx.strokeStyle = tone
                    const cx = width / 2
                    ctx.lineWidth = panel.theme.powerGlyphStroke
                    ctx.lineCap = "round"
                    ctx.beginPath()
                    ctx.arc(cx, powerGlyph.ringCentreY, powerGlyph.ringRadius,
                            -Math.PI / 2 + powerGlyph.ringGap, Math.PI * 1.5 - powerGlyph.ringGap)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(cx, powerGlyph.stemTop)
                    ctx.lineTo(cx, powerGlyph.stemBottom)
                    ctx.stroke()
                }
                Connections {
                    target: panel
                    function onOnChanged() { powerGlyph.requestPaint() }
                }
            }
            Column {
                x: panel.theme.powerLegendX
                anchors.verticalCenter: parent.verticalCenter
                spacing: panel.theme.spaceXxs
                Text {
                    text: panel.on ? "ON" : "OFF"
                    color: panel.on ? panel.theme.ink : panel.theme.inkDim
                    font.pixelSize: panel.theme.typeSubtitle
                    font.bold: true
                    font.letterSpacing: panel.theme.powerLegendSpacing
                }
                Text {
                    text: panel.on ? "GENERATING · 10 ms FADE" : "BIT-EXACT PASS-THROUGH"
                    color: panel.on ? panel.theme.power : panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.letterSpacing: panel.theme.powerNoteSpacing
                }
            }
            MouseArea {
                id: powerArea
                anchors.fill: parent
                enabled: panel.controller.editable
                onClicked: {
                    power.forceActiveFocus()
                    power.flip()
                }
            }
        }

        Caption {
            x: panel.theme.cardPad
            y: panel.theme.modeCaptionY
            text: "MODE"
        }
        Kit.Segment {
            x: panel.theme.cardPad
            y: panel.theme.modeY
            width: outputPlate.width - 2 * panel.theme.cardPad
            height: panel.theme.modeHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "signal.mode"
            labels: ["REPLACE", "ADD"]
        }

        Caption {
            x: panel.theme.cardPad
            y: panel.theme.routingCaptionY
            text: "ROUTING"
        }
        // The two speakers, each with what it plays.
        Item {
            id: diagram
            objectName: "signalRouting"
            x: panel.theme.cardPad
            y: panel.theme.routingY
            width: outputPlate.width - 2 * panel.theme.cardPad
            height: panel.theme.routingHeight
            readonly property bool leftPlays: panel.output !== 2
            readonly property bool rightPlays: panel.output !== 1
            component Speaker: Rectangle {
                id: speaker
                required property string name
                required property color tone
                required property bool plays
                property bool inverted: false
                property string tag: ""
                width: (diagram.width - panel.theme.speakerGap) / 2
                height: diagram.height
                radius: panel.theme.radiusLg
                color: panel.theme.well
                border.color: plays && panel.on ? Qt.rgba(tone.r, tone.g, tone.b, panel.theme.speakerEdgeAlpha)
                                                : panel.theme.wellEdge
                Text {
                    x: panel.theme.speakerTextX
                    y: panel.theme.speakerNameY
                    text: speaker.name
                    color: speaker.plays && panel.on ? speaker.tone : panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeEmphasis
                    font.bold: true
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.speakerTextX
                    y: panel.theme.speakerTagY
                    text: speaker.tag
                    color: speaker.inverted ? panel.theme.invert : panel.theme.inkDim
                    font.pixelSize: panel.theme.typeCaption
                    font.bold: true
                    font.letterSpacing: panel.theme.speakerTagSpacing
                }
                SignalGlyph {
                    x: panel.theme.speakerGlyphX
                    y: panel.theme.speakerGlyphY
                    width: parent.width - 2 * panel.theme.speakerGlyphX
                    height: panel.theme.speakerGlyphHeight
                    wave: panel.wave
                    inverted: speaker.inverted
                    tone: speaker.plays && panel.on ? speaker.tone : panel.theme.wellEdge
                    lineWidth: panel.theme.strokeBold
                }
                // Silent: a flat line.
                Rectangle {
                    visible: !speaker.plays
                    x: panel.theme.speakerGlyphX
                    y: panel.theme.speakerSilentY
                    width: parent.width - 2 * panel.theme.speakerGlyphX
                    height: panel.theme.hairline
                    color: panel.theme.inkFaint
                }
            }
            Speaker {
                objectName: "signalSpeakerL"
                x: 0
                name: "L"
                tone: panel.theme.left
                plays: diagram.leftPlays
                tag: panel.output === 3 ? "SECONDS 1, 3…" : diagram.leftPlays ? "" : "SILENT"
            }
            // Between them: the alternation, when there is one.
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                text: panel.output === 3 ? "⇄\n1 s" : panel.output === 4 ? "Ø" : panel.output === 0 ? "=" : ""
                horizontalAlignment: Text.AlignHCenter
                color: panel.output === 4 ? panel.theme.invert : panel.theme.inkDim
                font.pixelSize: panel.output === 3 ? panel.theme.typeSubtitle : panel.theme.routingGlyphSize
                font.bold: true
            }
            Speaker {
                objectName: "signalSpeakerR"
                x: diagram.width - width
                name: "R"
                tone: panel.theme.right
                plays: diagram.rightPlays
                inverted: panel.output === 4
                tag: panel.output === 4 ? "INVERTED" : panel.output === 3 ? "SECONDS 2, 4…"
                    : diagram.rightPlays ? "" : "SILENT"
            }
        }
        Kit.Segment {
            x: panel.theme.cardPad
            y: panel.theme.outputSegmentY
            width: outputPlate.width - 2 * panel.theme.cardPad
            height: panel.theme.outputSegmentHeight
            controller: panel.controller
            theme: panel.theme
            parameterId: "signal.output"
            labels: ["BOTH", "L", "R", "ALT", "R Ø"]
        }
        Note {
            x: panel.theme.cardPad
            y: panel.theme.outputNoteY
            width: outputPlate.width - 2 * panel.theme.cardPad
            text: ["The same signal on both sides.",
                   "The left side only: check which speaker is which.",
                   "The right side only: check which speaker is which.",
                   "Left, then right, a second each: walk the channels.",
                   "The right side upside down: in a mono sum it cancels, if polarity is right."][panel.output]
        }
    }

    // ---- Row B: waves, tone, timing --------------------------------------------------

    readonly property real rowBY: rowAY + rowAHeight + gap
    readonly property real rowBHeight: panel.height - rowBY - margin

    Kit.Card {
        theme: panel.theme
        id: wavePlate
        title: "WAVE"
        x: panel.margin
        y: panel.rowBY
        width: panel.theme.waveWidth
        height: panel.rowBHeight

        // The five waves as pictured tiles over one choice.
        Item {
            id: waves
            objectName: "signalSegment:signal.wave"
            x: panel.theme.cardPad
            y: panel.theme.wavesY
            width: wavePlate.width - 2 * panel.theme.cardPad
            height: panel.theme.wavesHeight
            activeFocusOnTab: panel.controller.editable
            opacity: panel.controller.editable ? 1 : panel.theme.disabledOpacity
            Accessible.role: Accessible.ComboBox
            Accessible.name: "Wave"
            Accessible.description: panel.waveNames[panel.wave]
            function pick(index) {
                panel.theme.lastTouched = "signal.wave"
                panel.controller.set_choice("signal.wave", index)
            }
            function stepBy(direction) {
                panel.controller.step("signal.wave", direction, false)
                panel.theme.lastTouched = "signal.wave"
            }
            Keys.onLeftPressed: stepBy(-1)
            Keys.onRightPressed: stepBy(1)
            Keys.onUpPressed: stepBy(-1)
            Keys.onDownPressed: stepBy(1)
            Keys.onDeletePressed: {
                panel.controller.reset_to_default("signal.wave")
                panel.theme.lastTouched = "signal.wave"
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -panel.theme.segmentFocusMargin
                radius: panel.theme.radiusXl
                color: "transparent"
                border.color: waves.activeFocus ? panel.theme.accent : "transparent"
            }
            Row {
                id: tiles
                readonly property int count: 5
                spacing: panel.theme.spaceMd
                Repeater {
                    model: tiles.count
                    delegate: Rectangle {
                        id: tile
                        required property int index
                        readonly property bool chosen: index === panel.wave
                        objectName: "signalWave:" + index
                        width: (waves.width - (tiles.count - 1) * tiles.spacing) / tiles.count
                        height: waves.height
                        radius: panel.theme.radiusLg
                        color: chosen ? Qt.rgba(panel.theme.accent.r, panel.theme.accent.g, panel.theme.accent.b,
                                                panel.theme.tileFillAlpha)
                            : tileArea.pressed ? panel.theme.button : panel.theme.buttonHi
                        border.color: chosen ? Qt.rgba(panel.theme.accent.r, panel.theme.accent.g, panel.theme.accent.b,
                                                       panel.theme.tileEdgeAlpha)
                            : tileArea.containsMouse ? panel.theme.wellEdge : panel.theme.seam
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: panel.waveNames[index]
                        Accessible.checked: chosen
                        Rectangle {
                            x: panel.theme.tileScreenInset
                            y: panel.theme.tileScreenY
                            width: parent.width - 2 * panel.theme.tileScreenInset
                            height: panel.theme.tileScreenHeight
                            radius: panel.theme.radiusMd
                            color: panel.theme.lcd
                            border.color: tile.chosen ? panel.theme.lcdEdge : panel.theme.well
                            SignalGlyph {
                                anchors.fill: parent
                                anchors.margins: panel.theme.tileGlyphInset
                                wave: tile.index
                                tone: tile.chosen ? panel.theme.lcdInk : panel.theme.lcdDim
                                lineWidth: panel.theme.tileGlyphStroke
                            }
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: panel.theme.tileLabelY
                            width: parent.width - panel.theme.tileLabelInset
                            horizontalAlignment: Text.AlignHCenter
                            text: ["SINE", "SWEEP", "PINK", "WHITE", "IMPULSE"][tile.index]
                            color: tile.chosen ? panel.theme.ink : panel.theme.inkDim
                            font.pixelSize: panel.theme.typeSmall
                            font.bold: tile.chosen
                            font.letterSpacing: panel.theme.tileLabelSpacing
                        }
                        Rectangle {
                            visible: tile.chosen
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: parent.height - panel.theme.tileRuleBottom
                            width: panel.theme.tileRuleWidth
                            height: panel.theme.tileRuleHeight
                            radius: height / 2
                            color: panel.theme.accent
                        }
                        MouseArea {
                            id: tileArea
                            anchors.fill: parent
                            enabled: panel.controller.editable
                            hoverEnabled: true
                            onClicked: {
                                waves.forceActiveFocus()
                                waves.pick(tile.index)
                            }
                        }
                    }
                }
            }
        }
        Note {
            x: panel.theme.cardPad
            y: panel.theme.waveNoteY
            width: wavePlate.width - 2 * panel.theme.cardPad
            text: ["A pure tone: set levels, find resonances, check a meter against its reference.",
                   "A log sweep from 20 Hz to 20 kHz, equal time per octave: excite a room or a chain, then read it on Lens.",
                   "Pink noise: equal energy in every octave, so a flat system reads flat on an octave analyser. For balancing speakers and mixes.",
                   "White noise: equal energy per hertz, bright to the ear. For checking filters and high-frequency response.",
                   "Single-sample clicks at the rate: hear a reverb's tail, time a delay, or find latency."][panel.wave]
        }
    }

    Kit.Card {
        theme: panel.theme
        id: tonePlate
        title: "TONE"
        x: wavePlate.x + wavePlate.width + panel.gap
        y: panel.rowBY
        width: panel.theme.toneWidth
        height: panel.rowBHeight

        Kit.Knob {
            x: panel.theme.toneKnobX
            y: panel.theme.cardBodyY
            controller: panel.controller
            theme: panel.theme
            parameterId: "signal.frequency"
            diameter: panel.theme.bigDialDiameter
            readout: true
            inactive: !panel.tonal
            valueText: panel.hzText(panel.frequency)
        }
        Text {
            objectName: "signalNote"
            x: panel.theme.toneKnobX
            y: panel.theme.dialNoteY
            width: panel.theme.dialNoteWidth
            horizontalAlignment: Text.AlignHCenter
            text: panel.tonal ? panel.noteOf(panel.frequency) : "SINE ONLY"
            color: panel.tonal ? panel.theme.accent : panel.theme.inkFaint
            font.family: panel.theme.mono
            font.pixelSize: panel.tonal ? panel.theme.typeEmphasis : panel.theme.typeCaption
            font.letterSpacing: panel.tonal ? 0 : panel.theme.tagSpacing
        }
        Kit.Knob {
            x: panel.theme.levelKnobX
            y: panel.theme.cardBodyY
            controller: panel.controller
            theme: panel.theme
            parameterId: "signal.level"
            diameter: panel.theme.bigDialDiameter
            readout: true
            valueText: panel.signed(panel.level) + " dBFS"
        }
        Text {
            objectName: "signalLevelNote"
            x: panel.theme.levelKnobX
            y: panel.theme.dialNoteY
            width: panel.theme.dialNoteWidth
            horizontalAlignment: Text.AlignHCenter
            text: panel.levelKind
            color: panel.theme.accent
            font.pixelSize: panel.theme.typeBody
            font.bold: true
            font.letterSpacing: panel.theme.levelNoteSpacing
        }
        Note {
            x: panel.theme.cardPad
            y: panel.theme.plateNoteY
            width: tonePlate.width - 2 * panel.theme.cardPad
            text: panel.noise ? "Noise is set by its RMS level: −20 dBFS RMS is the usual alignment level."
                : "Tones and impulses are set by their peak: a sine at −18 dBFS peak reads −21 dBFS RMS."
        }
    }

    Kit.Card {
        theme: panel.theme
        id: timingPlate
        title: "TIMING"
        note: panel.wave === 1 ? "SWEEP" : panel.wave === 4 ? "IMPULSES" : "NONE FOR THIS WAVE"
        x: tonePlate.x + tonePlate.width + panel.gap
        y: panel.rowBY
        width: panel.width - panel.margin - x
        height: panel.rowBHeight

        Kit.Knob {
            visible: panel.wave === 1
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.cardBodyY
            controller: panel.controller
            theme: panel.theme
            parameterId: "signal.sweep_seconds"
            diameter: panel.theme.bigDialDiameter
            readout: true
            valueText: panel.secondsText(panel.sweepSeconds)
        }
        Kit.Knob {
            visible: panel.wave === 4
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.cardBodyY
            controller: panel.controller
            theme: panel.theme
            parameterId: "signal.impulse_rate"
            diameter: panel.theme.bigDialDiameter
            readout: true
            valueText: Format.hertz(panel.impulseRate)
        }
        // The sweep's progress, as a bar.
        Rectangle {
            visible: panel.wave === 1
            x: panel.theme.timingPad
            y: panel.theme.progressY
            width: timingPlate.width - 2 * panel.theme.timingPad
            height: panel.theme.progressHeight
            radius: height / 2
            color: panel.theme.well
            border.color: panel.theme.wellEdge
            Rectangle {
                objectName: "signalProgress"
                x: parent.border.width
                y: parent.border.width
                height: parent.height - 2 * parent.border.width
                radius: height / 2
                width: (parent.width - 2 * parent.border.width) * (panel.sweeping ? panel.live.progress : 0)
                color: panel.theme.accent
            }
        }
        Text {
            visible: panel.wave === 4
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.dialNoteY
            text: "EVERY " + panel.periodText(panel.impulseRate)
            color: panel.theme.accent
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeEmphasis
        }
        Note {
            visible: panel.wave !== 1 && panel.wave !== 4
            x: panel.theme.timingPad
            y: panel.theme.timingIdleNoteY
            width: timingPlate.width - 2 * panel.theme.timingPad
            horizontalAlignment: Text.AlignHCenter
            text: "Tones and noise run continuously. Pick the sweep or impulses for a time control."
        }
        Note {
            x: panel.theme.timingPad
            y: panel.theme.plateNoteY
            width: timingPlate.width - 2 * panel.theme.timingPad
            horizontalAlignment: Text.AlignHCenter
            visible: panel.wave === 1 || panel.wave === 4
            text: panel.wave === 1 ? "One pass, then it starts again from 20 Hz."
                : "Each impulse is one sample at the level."
        }
    }

    Timer {
        objectName: "signalTelemetryPoll"
        interval: panel.playing ? 33 : 250
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
