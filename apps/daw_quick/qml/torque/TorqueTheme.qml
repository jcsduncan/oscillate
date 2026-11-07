import QtQuick
import "../devicekit"

// Torque design tokens: a matte black modern faceplate whose neon accent
// follows the model (acid green, Moog amber, Reese violet, Sub red).
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "torque"
    tipRadius: 3
    knobDiameter: 34
    knobDialPad: 12
    knobWidthPad: 18
    knobMinWidth: 56
    knobHeightPad: 28
    knobLongName: "group"
    knobLabelFont: ""
    knobLabelSize: 9
    knobLabelBold: true
    knobLabelSpacing: 0.9
    segmentWidth: 120
    segmentHeight: 22
    segmentSpacing: 2
    segmentFocusMargin: 2
    segmentFocusRadius: 4
    segmentRadius: 3
    segmentRadiusVertical: 3
    segmentFillAcross: false
    segmentRule: "none"
    segmentLed: true
    segmentLedX: 4
    dropdownHeight: 22
    dropdownTextX: 7
    dropdownTextRight: 22
    dropdownChevronMargin: 6
    arrowWidth: 26
    arrowHeight: 40
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 10
    arrowInk: accent

    property color bodyTop: "#17181b"
    property color body: "#111214"
    property color bodyBottom: "#0b0c0d"
    seam: "#26282c"
    plate: "#16171a"
    plateEdge: "#24262a"
    well: "#0a0b0c"
    wellEdge: "#1f2124"
    grid: "#1c1e21"
    ink: "#e9ebee"
    inkDim: "#8b9097"
    inkFaint: "#7b828b"

    readonly property var modelColors: ["#a6ff3d", "#ffb02e", "#a97bff", "#ff3d55"]
    readonly property var modelNames: ["ACID", "MOOG", "REESE", "SUB"]
    readonly property var modelTaglines: ["303 DIODE LADDER", "3-OSC TRANSISTOR LADDER",
                                          "DETUNED STACK · COMB", "808 SINE · PITCH DROP"]
    // The current model's accent; the panel binds it.
    accent: modelColors[0]

    knobRing: "#050506"
    knobTop: "#2a2c30"
    knobBottom: "#141518"
    pointer: "#f4f5f7"
    button: "#1d1f22"
    buttonHi: "#26292d"
    ledOff: "#2a2c30"

    warning: "#e0901f"
    live: "#3fb58f"

    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // Neon glow: the soft wide stroke under a lit arc or trace.
    property real glowAlpha: 0.22
    // A tinted fill behind a chosen model tab.
    property real tabWashAlpha: 0.14
    // The edge of a focused or chosen well (tabs, preset display).
    property real focusStroke: 1.5
    // A synced-away control (division while free) is dimmed.
    property real unsyncedOpacity: 0.45

    // The faceplate.
    property int panelWidth: 1280
    property int panelHeight: 770
    property real panelRadius: 10
    // Between plates.
    property real plateGap: 10
    property real plateTitleY: 9
    property real plateTitleSpacing: 2.2
    property real plateNoteY: 10
    // Small bold captions under controls.
    property real captionSpacing: 1.2
    // Where a plate's controls start: the first row, the control columns,
    // the left inset of model rows and captions.
    property real plateContentY: 30
    property real plateControlY: 32
    property real plateContentX: 14
    // A caption at a plate's foot sits this far above its bottom.
    property real plateFootRise: 18

    // Knob cells: a knob with its caption below.
    property int cellDiameter: 30
    property int cellWidthPad: 22
    property int cellMinWidth: 62
    property int cellHeightPad: 30

    // Header: wordmark, model tabs, preset display.
    property real headerY: 10
    property real headerHeight: 50
    property int wordmarkSize: 30
    property real wordmarkSpacing: 6
    property real wordmarkRuleInset: 6
    property real wordmarkRuleHeight: 3
    property real taglineSpacing: 1.6
    property real modelTabsX: 196
    property int modelTabWidth: 92
    property int modelTabHeight: 40
    property real modelLedX: 10
    property int modelLedSize: 7
    property real modelTabTextOffset: 6
    property real modelTabLetterSpacing: 1.6
    property real browserGap: 18
    property int presetDisplayWidth: 420
    property int presetDisplayHeight: 40
    property real presetNameY: 15
    property int presetNameWidth: 190
    property real presetDividerX: 212
    property real presetDividerInset: 7
    property real readoutX: 222
    property real readoutValueY: 16
    property real readoutLetterSpacing: 1
    property real presetChevronMargin: 9
    property real statusY: 12

    // Row A: the scope plate and the model's main controls.
    property real rowAY: 72
    property real rowAHeight: 240
    property int scopePlateWidth: 500
    property real scopeInset: 10
    property real scopeTop: 28
    property int filterViewWidth: 300
    property int mainKnobDiameter: 46
    property int mainCellWidth: 92
    property real mainDividerY: 118
    property real mainCaptionY: 126
    property real modelRowY: 148
    property real moogRowY: 146
    property real modelRowSpacing: 18
    property real moogRowSpacing: 10
    property real reeseRowSpacing: 14
    property real oscColumnSpacing: 5
    // Above a segment column so it centres against the knob cells.
    property real segmentColumnPad: 10
    property int largeSegmentHeight: 26
    property int compactSegmentHeight: 24
    property int stepperHeight: 22
    property int tallStepperHeight: 24
    property int acidWaveWidth: 150
    property int oscControlWidth: 92
    property int reeseVoicesWidth: 76
    property int reeseFilterWidth: 132
    property int subLayerWidth: 190

    // Rows B and C: envelopes, voice, LFO, effects, output.
    property real rowBHeight: 112
    property real rowCHeight: 112
    property int envelopesWidth: 470
    property real envKnobY: 40
    property real envDividerX: 204
    property real envDividerTop: 34
    property real ampRowX: 208
    property real ampCaptionX: 214
    property int voicePlateWidth: 300
    property real voiceGridY: 34
    property real voiceGridRowSpacing: 18
    property int voiceStepperWidth: 64
    property real fineKnobX: 226
    property int fineKnobDiameter: 26
    property int lfoControlWidth: 100
    property real lfoKnobsX: 120
    property real lfoKnobsY: 34
    property int effectsWidth: 900
    property int distModeWidth: 168
    property real distKnobsX: 186
    property real chorusDividerX: 378
    property real chorusX: 392
    property int chorusWidth: 150
    property real delayDividerX: 556
    property real delayX: 570
    property int delayControlWidth: 104
    property real delayKnobsX: 686
    property real outputSpacing: 14
    property real outputDividerHeight: 60
    property int outputKnobDiameter: 40

    // Row D: the sequencer.
    property int seqOnWidth: 132
    property int seqLengthWidth: 58
    property int seqRateWidth: 68
    property real seqKnobsPad: 10
    property int seqKnobDiameter: 24
    property int seqCellWidth: 66
    property real stepsX: 160
    property real stepSpacing: 5
    // The step strip while the sequencer is off.
    property real seqOffOpacity: 0.7

    // A sequencer step.
    property real stepRadius: 5
    property real stepEdgeAlpha: 0.45
    property real stepDisabledOpacity: 0.45
    property real stepOutsideOpacity: 0.32
    property real stepInset: 5
    property real stepRowGap: 5
    // The step number and gate LED, inset from the cell's top corners.
    property real stepEdgeInset: 7
    property real stepNumberY: 5
    property real stepLedY: 8
    property int stepLedSize: 8
    property real tieLedOpacity: 0.55
    property real playheadInset: 3
    property real playheadHeight: 3
    property real playheadClosedOpacity: 0.5
    property real playWashOpenAlpha: 0.14
    property real playWashClosedAlpha: 0.06
    property real noteWellY: 22
    property real noteWellHeight: 40
    property real noteNameY: 3
    property real intervalY: 23
    property real intervalRestOpacity: 0.5
    property int octaveKeyHeight: 18
    property real octaveOnAlpha: 0.22
    property int stepToggleHeight: 20
    property real stepToggleSpacing: 0.8
    // Offsets below the octave keys: the gate toggle, then accent and slide.
    property real stepGateOffset: 23
    property real stepFlagsOffset: 46
    property real slideLinkWidth: 6

    // The oscillator scope.
    property real scopeCanvasTop: 22
    property real scopeTitleX: 10
    property real scopeTitleSpacing: 1.4
    property real scopeReadoutY: 5
    property real scopeGlowWidth: 6
    property real scopeTraceWidth: 1.8
    property real dropTraceWidth: 1.5
    property real dropLabelX: 13
    // The drop caption sits this far below the wave's share of the scope.
    property real dropLabelDrop: 11
    property real dropLabelSpacing: 1

    // The knob face's strokes.
    property real knobArcWidth: 3
    property real knobGlowWidth: 7
    property real knobFocusStroke: 1.8
    property real knobPointerWidth: 2
    // A chosen segment key's edge.
    property real segmentEdgeAlpha: 0.7

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        ctx.lineCap = "round"
        // Track.
        ctx.lineWidth = knobArcWidth
        ctx.strokeStyle = knob.theme.grid
        ctx.beginPath()
        ctx.arc(c, c, radius + 2, start, start + sweep)
        ctx.stroke()
        // Value arc with a soft glow underneath.
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            ctx.strokeStyle = Qt.rgba(knob.accent.r, knob.accent.g, knob.accent.b, glowAlpha)
            ctx.lineWidth = knobGlowWidth
            ctx.beginPath()
            ctx.arc(c, c, radius + 2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
            ctx.strokeStyle = knob.accent
            ctx.lineWidth = knobArcWidth
            ctx.beginPath()
            ctx.arc(c, c, radius + 2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Body.
        const body = radius - 2.5
        const fill = ctx.createLinearGradient(0, c - body, 0, c + body)
        fill.addColorStop(0, knob.theme.knobTop)
        fill.addColorStop(1, knob.theme.knobBottom)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, body, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? knobFocusStroke : hairline
        ctx.strokeStyle = knob.activeFocus ? knob.accent : knob.theme.knobRing
        ctx.stroke()
        // Pointer: a short bright line with a dot at the rim.
        ctx.strokeStyle = knob.theme.pointer
        ctx.lineWidth = knobPointerWidth
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * body * 0.42, c + Math.sin(at) * body * 0.42)
        ctx.lineTo(c + Math.cos(at) * (body - 2), c + Math.sin(at) * (body - 2))
        ctx.stroke()
    }

    // Segment keys.
    function segmentFill(tile, stop) {
        return stop === 0 && !tile.pressed ? buttonHi : button
    }
    function segmentEdge(tile) {
        return tile.on ? Qt.rgba(tile.accent.r, tile.accent.g, tile.accent.b, segmentEdgeAlpha) : knobRing
    }
}
