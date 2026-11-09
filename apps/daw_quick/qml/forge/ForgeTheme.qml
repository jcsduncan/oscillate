import QtQuick
import "../devicekit"

// Forge design tokens: blackened, brushed steel lit by the forge, copper
// fittings, and heat running from dull copper through ember and amber to
// white-hot as the drive rises.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "forge"
    accent: amber
    tipRadius: 3
    menuHighlightAlpha: 0.2
    knobDiameter: 40
    knobDialPad: 14
    knobWidthPad: 24
    knobHeightPad: 34
    knobLongName: "group"
    knobCaptionGap: 2
    knobLabelSize: 9
    knobLabelBold: true
    knobLabelSpacing: 1.4
    segmentHeight: 24
    segmentSpacing: 3
    segmentFocusRadius: 4
    segmentRadius: 3
    segmentRadiusVertical: 3
    segmentFillAcross: false
    segmentLetterSpacingVertical: 1.2
    segmentTextXVertical: 14
    segmentSizeVertical: 11
    segmentLetterSpacing: 0.8
    segmentTextRightVertical: 8
    segmentCaptionGap: 0
    segmentCaptionSpacing: 0.6
    segmentGlyphMode: "column"
    segmentGlyphIdleInk: inkFaint
    arrowWidth: 26
    arrowRadius: 5
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 10
    arrowInk: copper

    // Blackened steel: the case, its plates and the wells cut into them.
    property color steelTop: "#1b1c1f"
    steel: "#111214"
    property color steelBottom: "#08090a"
    property color brush: "#2c2e33"
    seam: "#26272b"
    plate: "#0f1012"
    plateEdge: "#2b2a29"
    well: "#050506"
    wellEdge: "#2a2622"
    grid: "#1f2023"
    gridStrong: "#2d2b29"

    // Copper fittings.
    property color copper: "#b5683c"
    property color copperHi: "#e39a66"
    property color copperLo: "#5e321b"

    // Heat, coolest to hottest.
    property color heatCold: "#8a4426"
    property color ember: "#ff6a1a"
    property color amber: "#ffad3b"
    property color molten: "#ffd685"
    property color whiteHot: "#fff4dc"

    ink: "#ece4d6"
    inkDim: "#a0978a"
    inkFaint: "#847c72"

    knobTop: "#2e3035"
    knobBottom: "#0b0c0e"
    property color knurl: "#3c3e44"
    button: "#111214"
    buttonHi: "#1c1d21"

    warning: "#ff9d2e"
    live: "#ffad3b"

    mono: "DejaVu Sans Mono"
    sans: "DejaVu Sans"

    // The control last touched, read back in the display.
    lastTouched: ""

    // ---- Panel geometry ----------------------------------------------------------
    property int panelWidth: 1120
    property int panelHeight: 566
    property real caseRadius: 10
    // Where steel sits in the case gradient.
    property real caseMidStop: 0.4
    // Between the plates.
    property real cardGap: 10
    // Tracking for small bold capitals (plate notes aside).
    property real labelSpacing: 1.3
    property real capsTightSpacing: 1.2
    // Lines of wrapped notes.
    property real noteLineHeight: 1.2
    // The well the preset display and the forge screen are cut into.
    property real wellRadius: 5
    property real focusStroke: 1.5
    // The heat colour drawn for the drive (the wordmark, odd harmonics):
    // heat(heatGlowBase + heatGlowRange * drive).
    property real heatGlowBase: 0.35
    property real heatGlowRange: 0.5
    // A heated knob's arc: heat(knobHeatBase + knobHeatRange * position).
    property real knobHeatBase: 0.15
    property real knobHeatRange: 0.85

    // Brushed steel: grain lines, their spacing and side inset, and their
    // alpha (a base plus a random spread); the forge's warmth rising from
    // the bottom, from this share of the height down.
    property real grainStep: 2
    property real grainInset: 4
    property real grainAlpha: 0.03
    property real grainAlphaSpread: 0.05
    property real warmthTop: 0.55
    property real warmthAlpha: 0.09
    property real warmthHeatAlpha: 0.08

    // A plate: its fill, copper rivets in the corners, its title and note.
    property real plateAlpha: 0.9
    property real rivetInset: 5
    property real rivetSize: 4
    property real rivetStroke: 0.6
    property real plateTitleX: 16
    property real plateTitleY: 9
    property real plateTitleSpacing: 3
    property real plateNoteGap: 10
    property real plateNoteY: 10
    property real plateNoteSpacing: 1.1
    // Inside a plate: the inset and the top of its body.
    property real plateInset: 12
    property real plateBodyY: 30
    // A knob row sits this far below a plate's centre (clear of the title).
    property real plateBodyOffset: 10

    // A knob cell: at least this wide, else the dial plus the pad.
    property real cellMinWidth: 80
    property real cellWidthPad: 30
    property real cellHeightPad: 36

    // The header (Kit.DeviceHeader): the heat-lit wordmark over its rule and
    // subtitle, the preset browser and, once a control is touched, its readout.
    property real headerY: 10
    headerHeight: 52
    headerBrowserX: 232
    wordmarkSize: 30
    wordmarkWeight: Font.Bold
    wordmarkSpacing: 10
    property color wordmarkShadow: "#000000"
    property real ruleGap: 1
    // The rule runs this much shorter than the wordmark.
    property real ruleTrim: 12
    // Where the rule turns from amber to ember.
    property real ruleEmberStop: 0.6
    subtitleGap: 7
    subtitleSpacing: 1.8
    subtitleInk: inkDim
    presetDisplayWidth: 300
    presetDisplayHeight: 42
    presetDisplayRadius: wellRadius
    presetFocusStroke: focusStroke
    presetTextX: 14
    presetTagY: 5
    presetTagSpacing: capsTightSpacing
    presetNameY: 16
    presetNameSize: typeTitle
    presetChevronMargin: 10
    presetChevronInk: copper
    readoutWidth: 230
    readoutRadius: wellRadius
    readoutTextX: 14
    readoutNameY: 5
    readoutValueY: 17
    readoutValueInk: amber

    // The top row: the curve selector, the forge, the levels.
    property real rowAY: 72
    property real rowAHeight: 312
    property real curveCardWidth: 210
    property real levelsCardWidth: 196
    property real curveViewY: 28
    // The selector's curve glyphs.
    property real glyphAxisAlpha: 0.25
    property real glyphStroke: 1.6
    // The glyph stays this far inside its box.
    property real glyphInset: 1.5

    // The levels plate: the meter pair and the auto-gain makeup.
    property real meterRowX: 26
    property real meterRowY: 34
    property real meterRowGap: 34
    property real levelMeterHeight: 168
    property real autoGainY: 214
    property real makeupY: 226
    property int makeupSize: 20
    property real autoGainSwitchY: 262

    // The bottom row: heat, tone, output.
    property real heatCardWidth: 470
    property real toneCardWidth: 170
    property real heatKnobsX: 18
    property real heatKnobGap: 18
    property int driveDiameter: 66
    property int heatSmallDiameter: 44
    // Bias and Colour sit this far below Drive's top.
    property real heatSmallDrop: 12
    property real heatNoteX: 330
    property real heatNoteY: 46
    property int cellDiameter: 50

    // A level meter: its size, track and bar, the 0 dBFS line, the scale.
    property real meterWidth: 34
    property real meterHeight: 160
    property real meterTrackWidth: 12
    // Under the track: the label and the value.
    property real meterFootRoom: 30
    property real meterBarInset: 2
    property real meterBarRadius: 1
    // Where the bar turns amber, then ember, from the top.
    property real meterAmberStop: 0.2
    property real meterEmberStop: 0.6
    property real meterZeroAlpha: 0.45
    property real meterScaleGap: 3
    // The scale's numbers sit this far above their marks.
    property real meterScaleLift: 5
    property int scaleTextSize: typeMicro
    property real meterValueY: 15

    // The forge screen: forge-light rising from below (each tint scales the
    // heat colour's green and blue; each alpha is a base plus the heat's and
    // the output's share), the grid and unity, the molten fill and the
    // layered curve, the input sparks and the side readouts.
    property real forgeLightGreen: 0.7
    property real forgeLightBlue: 0.4
    property real forgeLightAlpha: 0.16
    property real forgeLightHeatAlpha: 0.22
    property real forgeLightBreathAlpha: 0.2
    property real forgeLightMidStop: 0.55
    property real forgeLightMidGreen: 0.5
    property real forgeLightMidBlue: 0.3
    property real forgeLightMidAlpha: 0.05
    property real forgeLightMidHeatAlpha: 0.06
    property var unityDash: [3, 4]
    property real unityAlpha: 0.7
    property real moltenFillAlpha: 0.20
    property real moltenFillMidAlpha: 0.02
    // The curve's strokes, widest glow to core: [width, alpha], each alpha
    // scaled by curveGlowBase + curveGlowHeat * drive.
    property var curveLayers: [[16, 0.05], [9, 0.12], [4.5, 0.35], [2.2, 1.0]]
    property real curveGlowBase: 0.6
    property real curveGlowHeat: 0.4
    property real curveCoreAlpha: 0.35
    property real curveCoreHeatAlpha: 0.5
    property real curveCoreStroke: 0.9
    property var sparkDash: [2, 3]
    property real sparkLineAlpha: 0.25
    property real sparkRadius: 12
    property color sparkCore: Qt.rgba(1, 0.97, 0.88, 0.95)
    property real sparkMidStop: 0.3
    property real sparkMidAlpha: 0.6
    property var sineDash: [2, 3]
    property real sineAlpha: 0.55
    // The wave's strokes, glow to core: [width, alpha].
    property var waveLayers: [[6, 0.12], [3, 0.35], [1.6, 1.0]]
    property real harmonicHaloAlpha: 0.18
    property real harmonicFootAlpha: 0.35

    // Heat for a 0..1 amount: copper, ember, amber, molten, white-hot.
    function heat(amount) {
        const t = Math.max(0, Math.min(1, amount))
        const stops = [heatCold, ember, amber, molten, whiteHot]
        const at = t * (stops.length - 1)
        const i = Math.min(stops.length - 2, Math.floor(at))
        const f = at - i
        const a = stops[i]
        const b = stops[i + 1]
        return Qt.rgba(a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, 1)
    }

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const t = knob.theme
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        // Recessed track cut into the plate.
        ctx.lineCap = "round"
        ctx.lineWidth = 3
        ctx.strokeStyle = t.well
        ctx.beginPath()
        ctx.arc(c, c, radius + 2, start, start + sweep)
        ctx.stroke()
        // The value arc, with a soft glow beneath it.
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            const a = knob.arc
            ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, 0.22)
            ctx.lineWidth = 7
            ctx.beginPath()
            ctx.arc(c, c, radius + 2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
            ctx.strokeStyle = a
            ctx.lineWidth = 2.4
            ctx.beginPath()
            ctx.arc(c, c, radius + 2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Copper collar.
        const collar = ctx.createLinearGradient(0, c - radius, 0, c + radius)
        collar.addColorStop(0, t.copperHi)
        collar.addColorStop(0.5, t.copper)
        collar.addColorStop(1, t.copperLo)
        ctx.fillStyle = collar
        ctx.beginPath()
        ctx.arc(c, c, radius - 1, 0, Math.PI * 2)
        ctx.fill()
        // Knurled steel cap: fine radial ridges round its rim.
        const cap = radius - 3.5
        const fill = ctx.createRadialGradient(c - cap * 0.35, c - cap * 0.45, cap * 0.1, c, c, cap)
        fill.addColorStop(0, t.knobTop)
        fill.addColorStop(1, t.knobBottom)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, cap, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = 0.7
        ctx.strokeStyle = t.knurl
        const ridges = Math.round(knob.diameter * 0.9)
        for (let i = 0; i < ridges; ++i) {
            const angle = i / ridges * Math.PI * 2
            ctx.beginPath()
            ctx.moveTo(c + Math.cos(angle) * (cap - 3), c + Math.sin(angle) * (cap - 3))
            ctx.lineTo(c + Math.cos(angle) * cap, c + Math.sin(angle) * cap)
            ctx.stroke()
        }
        ctx.lineWidth = knob.activeFocus ? 1.6 : 0.8
        ctx.strokeStyle = knob.activeFocus ? t.amber : "#000000"
        ctx.beginPath()
        ctx.arc(c, c, cap, 0, Math.PI * 2)
        ctx.stroke()
        // White-hot pointer.
        ctx.lineCap = "round"
        ctx.strokeStyle = t.whiteHot
        ctx.lineWidth = 2.2
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * cap * 0.25, c + Math.sin(at) * cap * 0.25)
        ctx.lineTo(c + Math.cos(at) * (cap - 4), c + Math.sin(at) * (cap - 4))
        ctx.stroke()
    }

    // Segment plates: an ember wash and molten lettering when chosen.
    function segmentFill(tile, stop) {
        return stop === 0 && !tile.pressed ? buttonHi : button
    }
    function segmentEdge(tile) {
        return tile.on ? Qt.rgba(ember.r, ember.g, ember.b, 0.7) : seam
    }
    function segmentLabelInk(tile) {
        return inkDim
    }
    function segmentBold(tile) {
        return true
    }
    function segmentCaptionInk(tile) {
        return inkFaint
    }
}
