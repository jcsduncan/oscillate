import QtQuick
import "../devicekit"

// Halo design tokens: a dark 80s digital faceplate in deep brown-black, cream
// legends, teal for modulators and amber for carriers, and a teal LCD.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "halo"
    accent: teal
    tipRadius: 3
    tipFill: lcd
    tipEdge: lcdDim
    tipInk: lcdText
    knobDiameter: 32
    knobDialPad: 10
    knobWidthPad: 16
    knobMinWidth: 52
    knobHeightPad: 26
    knobInactiveOpacity: 0.4
    knobLongName: "group"
    knobCaptionInset: 5
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
    segmentAccent: amber
    segmentGlyphMode: "fill"
    dropdownHeight: 22
    dropdownTextX: 7
    dropdownTextRight: 22
    dropdownChevronMargin: 6
    stepperFill: lcd
    stepperEdge: lcdEdge
    stepperInk: lcdText
    dropdownEdge: lcdEdge
    arrowHeight: 40
    arrowRadius: 4
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 10
    arrowInk: teal
    cardSheen: Qt.rgba(1, 1, 1, 0.05)
    cardTitleX: 10
    cardTitleY: 8
    cardTitleInk: ink
    cardTitleFont: ""
    cardTitleSize: 10
    cardTitleSpacing: 2.2
    cardNoteGap: 8
    cardNoteBold: true

    property color bodyTop: "#271d18"
    property color body: "#1b1411"
    property color bodyBottom: "#120d0b"
    seam: "#3b2d25"
    plate: "#211915"
    plateEdge: "#3a2c24"
    plateHi: "#33271f"
    ink: "#ecdfc8"
    inkDim: "#a8957c"
    inkFaint: "#95816a"

    property color teal: "#36c9b8"
    property color tealDim: "#1d5f58"
    property color amber: "#ffae3d"
    property color amberDim: "#6e4a1d"
    property color red: "#ff5d3d"

    property color lcd: "#0b1a17"
    property color lcdEdge: "#040b0a"
    property color lcdText: "#6ff3dc"
    property color lcdDim: "#24584f"
    property color lcdAmber: "#ffc266"

    property color membrane: "#3a2e27"
    property color membraneHi: "#4b3c32"
    property color membraneLo: "#2a201b"
    ledOff: "#3d2c1d"

    knobRing: "#0e0a09"
    knobTop: "#4a3f38"
    knobBottom: "#231c18"
    pointer: "#f6ecd9"

    warning: "#e0901f"
    live: "#3fb58f"

    mono: "DejaVu Sans Mono"

    // The control last touched, shown on the LCD. Shared by the panel's controls.
    lastTouched: ""

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        // Track and value arc around the knob.
        ctx.lineCap = "round"
        ctx.lineWidth = 2.6
        ctx.strokeStyle = knob.theme.knobRing
        ctx.beginPath()
        ctx.arc(c, c, radius + 1.5, start, start + sweep)
        ctx.stroke()
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            ctx.strokeStyle = knob.accent
            ctx.beginPath()
            ctx.arc(c, c, radius + 1.5, Math.min(from, at), Math.max(from, at))
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
        ctx.lineWidth = knob.activeFocus ? 1.6 : 1
        ctx.strokeStyle = knob.activeFocus ? knob.accent : knob.theme.knobRing
        ctx.stroke()
        // Pointer.
        ctx.strokeStyle = knob.theme.pointer
        ctx.lineWidth = 2
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * body * 0.3, c + Math.sin(at) * body * 0.3)
        ctx.lineTo(c + Math.cos(at) * (body - 1.5), c + Math.sin(at) * (body - 1.5))
        ctx.stroke()
    }

    // Segment keys: membrane switches.
    function segmentFill(tile, stop) {
        return stop === 0 && !tile.pressed ? membraneHi : membraneLo
    }
    function segmentEdge(tile) {
        return tile.on ? Qt.rgba(tile.accent.r, tile.accent.g, tile.accent.b, 0.7) : knobRing
    }

    // Waveform glyphs for the operator wave keys: sine, half, full and quarter.
    function paintWaveGlyph(ctx, index, w, h, colour) {
        ctx.strokeStyle = colour
        ctx.lineWidth = 1.3
        ctx.beginPath()
        const mid = h / 2
        const amp = h / 2 - 1
        for (let i = 0; i <= 40; ++i) {
            const p = i / 40
            const s = Math.sin(2 * Math.PI * p)
            let v = s
            if (index === 1)
                v = p < 0.5 ? s : 0
            else if (index === 2)
                v = Math.abs(s)
            else if (index === 3)
                v = (p < 0.25 || (p >= 0.5 && p < 0.75)) ? Math.abs(s) : 0
            const y = mid - v * amp
            if (i === 0)
                ctx.moveTo(p * w, y)
            else
                ctx.lineTo(p * w, y)
        }
        ctx.stroke()
    }

    // Dropdown: an LCD readout.
    function dropdownFill(dropdown) {
        return lcd
    }
    function dropdownInk(dropdown) {
        return lcdText
    }

    // Header: membrane preset arrows and an LCD status pill.
    function arrowFill(arrow, stop) {
        return stop === 0 && !arrow.pressed ? membraneHi : membraneLo
    }
    function arrowEdge(arrow) {
        return arrow.activeFocus ? teal : knobRing
    }
    function statusFill(tone) {
        return lcd
    }

    // Operator envelopes: the kernel's samples on the LCD strip, a rule at
    // zero, the release point dashed, the length in the corner.
    property int envelopeSamples: 96
    envelopeRadius: 3
    envelopeWell: lcd
    envelopeEdge: lcdEdge
    envelopePlotTop: 0
    envelopePlotBottom: 0
    envelopeHeadroom: 2
    envelopeLineWidth: 1.5
    envelopeFillOverLine: false
    envelopeFillAlpha: 0.18
    envelopeBaseline: true
    envelopeBaselineInk: lcdDim
    envelopeBaselineOffset: -0.5
    envelopeMarkerInk: lcdDim
    envelopeMarkerDash: [2, 3]
    envelopeMarkersOver: false
    envelopeLabelInk: lcdDim
    envelopeLabelFont: mono
    envelopeLabelSize: typeMicro
    envelopeLabelInset: 5
    envelopeLabelTop: 3

    // Panel layout: the faceplate, the operator row above, the algorithm
    // plate and two rows of global sections below.
    property int panelWidth: 1280
    property int panelHeight: 816
    property real panelRadius: 10
    property real sectionGap: 10
    property real lowerBottomMargin: spaceLg
    // Section cards: content inset from the card edge, the top of the first
    // body line, the knob row, the control row and the second knob row.
    property real cardPad: 10
    property real cardBodyTop: 26
    property real knobRowTop: 30
    property real controlRowTop: 32
    property real lowerKnobRowTop: 84
    // A dropdown in a card's title bar, a caption near the card's foot, and a
    // row of controls along its foot.
    property real cardDropdownY: 6
    property int cardDropdownWidth: 92
    property real cardCaptionBottom: 20
    property real cardCaptionSpacing: 1
    property real cardFootRowBottom: 32
    // Control heights: compact (operator and title-bar controls) and in a row.
    property int compactControlHeight: 20
    property int rowControlHeight: 22
    // Dimming for a stepper or dropdown that does not apply in the current mode.
    property real stepperInactiveOpacity: 0.4
    property real dropdownInactiveOpacity: 0.45
    // Global section knobs: the cell, the FM hero knobs and the play knobs.
    property int cellWidth: 70
    property int cellHeight: 62
    property int heroDiameter: 38
    property int playDiameter: 24
    property int playCellHeight: 54
    // Section widths and spacing.
    property int algorithmCardWidth: 372
    property int fmCardWidth: 150
    property real fmRowX: 5
    property int filterCardWidth: 400
    property int filterViewWidth: 176
    property real filterKnobGap: 26
    property int lfoSyncWidth: 92
    property int lfoDivisionWidth: 90
    property int voiceCardWidth: 330
    property int voiceModeWidth: 172
    property int unisonWidth: 58
    property int bendWidth: 64
    property real voiceKnobGap: 30
    property int effectsCardWidth: 356
    property int chorusWidth: 140
    property int delaySyncWidth: 86
    property real effectsKnobGap: 14

    // Header: the ring logo, the wordmark and its tagline.
    property real headerTop: 10
    property int headerHeight: 46
    property real logoTop: 7
    property int logoSize: 30
    property real logoStroke: 3
    property int logoDotSize: 12
    property real wordmarkGap: spaceLg
    property int wordmarkSize: 30
    property real wordmarkSpacing: 7
    property real taglineIndent: spaceXxs
    property real taglineLift: 5
    property real taglineSpacing: 2.2

    // The header LCD: category and preset name on the left, a divider, the
    // last touched control and its value, the algorithm and edited flag at
    // the right, faint scan lines over it all.
    property int lcdWidth: 470
    property int lcdHeight: 40
    property color lcdScanLine: Qt.rgba(1, 1, 1, 0.012)
    property int lcdScanLines: 18
    property real lcdScanHeight: 2
    property real lcdScanInset: spaceXxs
    property real lcdTextX: 10
    property real lcdTopLineY: 4
    property real lcdNameY: 15
    property int lcdNameWidth: 230
    property real lcdDividerX: 250
    property real lcdDividerInset: 6
    property real lcdReadoutX: 260
    property real lcdReadoutRight: 96
    property real lcdReadoutValueY: 17
    property real lcdTagRight: 22
    property real lcdAlgorithmY: 5
    property real lcdEditedY: 23
    property real lcdChevronRight: 7

    // The algorithm plate: the routing view and the grid of 32 membrane keys.
    property int algorithmViewHeight: 176
    property real algorithmGridTop: 210
    property int algorithmKeyHeight: 24
    property real algorithmFocusOutset: 3
    property real algorithmKeyLedX: 5
    property int algorithmKeyLedSize: 4
    property real algorithmKeyTextOffset: 3

    // The routing view: its title line, the drawing's top, and the drawing.
    property real routingTop: 24
    property real routingStroke: 1.5
    // Tints on the teal edges and the amber carrier boxes.
    property real routingEdgeAlpha: 0.85
    property real carrierFillAlpha: 0.85

    // The filter view: the response line and its fill, the dashed 0 dB rule,
    // the frequency labels and the bypass legend.
    property real responseStroke: 1.6
    property real responseFillAlpha: 0.16
    property var zeroDash: [2, 3]
    property real frequencyLabelGap: 3
    property real frequencyLabelBottom: 13
    property real bypassOffset: 14
    property real bypassSpacing: 2

    // Operator cards: sheen, role strip, title, badges, the frequency
    // readout, the controls and the envelope, top to bottom.
    property real operatorTop: 66
    property int operatorHeight: 392
    property real operatorGap: spaceMd
    property real operatorSheenStop: 0.25
    property real roleStripHeight: 3
    property real operatorTitleY: 9
    property real operatorTitleSpacing: 1.5
    property real badgeRowY: 10
    property real badgePad: 10
    property int badgeHeight: 15
    property real badgeSpacing: 0.8
    property real readoutTop: 32
    property int readoutHeight: 30
    property int readoutSize: 17
    property real readoutDetuneRight: 7
    property real operatorModeY: 68
    property int operatorModeWidth: 100
    property real operatorWaveY: 94
    property real operatorKnobsY: 122
    property int operatorKnobCellHeight: 60
    property int operatorLevelDiameter: 32
    property int operatorKnobDiameter: 28
    property real operatorEnvelopeY: 248
    property int operatorEnvelopeHeight: 40
    property real operatorRatesY: 292
    property real operatorRateRowHeight: 48
    property int operatorRateDiameter: 20
}
