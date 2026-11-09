import QtQuick
import "../devicekit"

// Pulse design tokens: a warm grey drum-machine faceplate with black
// legends, a dark instrument strip, and pads in the classic red, orange,
// yellow and cream sequence.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "pulse"
    accent: red
    tipRadius: 3
    tipFill: strip
    tipEdge: stripLine
    tipInk: stripText
    tipFont: ""
    tipBold: true
    menuFill: strip
    menuEdge: stripLine
    menuHeaderInk: stripDim
    menuInk: stripText
    knobAccent: cream
    knobDiameter: 40
    knobDialPad: 12
    knobWidthPad: 18
    knobMinWidth: 58
    knobHeightPad: 30
    knobDisabledOpacity: 0.5
    knobLongName: "label"
    knobCaptionInset: 4
    knobLabelFont: ""
    knobLabelSize: 10
    knobLabelBold: true
    knobLabelSpacing: 0.8
    knobLabelInk: ink
    knobValueFont: ""
    knobValueBold: true
    knobValueInk: red
    // Channel faders: a cream cap in a black slot between printed scales,
    // lit from the cap down the slot.
    faderAccent: cream
    faderFinePixels: 1600
    faderTrackFill: knobSkirt
    faderTrackEdge: seam
    faderTrackFocusEdge: led
    faderLit: "end"
    faderLitOpacity: 0.85
    faderLitInset: 2
    faderCapHeight: 16
    faderGripInk: knobSkirt
    faderTicks: 5
    faderTickMajor: 7
    faderTickMinor: 5
    faderTickGap: 1
    faderTickAnchor: "edges"
    faderTickCentred: false
    faderTickInk: inkDim
    // The cap's sheen and rim, from the fader's own colour.
    property real faderCapLighten: 1.08
    property real faderCapDarken: 1.12
    property real faderCapRimDarken: 1.7
    function faderCapFill(fader, stop) {
        return stop === 0 ? Qt.lighter(fader.accent, faderCapLighten) : Qt.darker(fader.accent, faderCapDarken)
    }
    function faderCapEdge(fader) {
        return fader.activeFocus ? led : Qt.darker(fader.accent, faderCapRimDarken)
    }
    arrowWidth: 30
    arrowHeight: presetDisplayHeight
    arrowRadius: 4
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 11
    arrowInk: ink
    arrowNoun: "kit"

    property color body: "#d6d0c2"
    property color bodyLight: "#e4dfd3"
    property color bodyDark: "#bdb6a6"
    seam: "#a39b8a"
    ink: "#26231f"
    inkDim: "#464139"
    inkFaint: "#5d574e"
    inkGround: body

    property color strip: "#1d1c1a"
    property color stripLine: "#33312d"
    property color stripText: "#efe9dc"
    property color stripDim: "#8f887b"

    property color lcd: "#131417"
    property color lcdGrid: "#26282c"
    property color lcdText: "#f3e7cf"

    property color red: "#d9412f"
    property color orange: "#ee872b"
    property color yellow: "#f0c83c"
    property color cream: "#efe8d6"
    property color led: "#ff3b26"
    ledOff: "#5a2a22"

    property color knobSkirt: "#1b1a18"
    property color knobBody: "#2c2a27"
    pointer: "#f4efe4"

    warning: "#e0901f"
    live: "#4f9a3c"

    // Pads 1..12 take three of each colour in turn.
    property var padColours: [red, orange, yellow, cream]
    property int padsPerColour: 3
    function padColor(index) {
        return padColours[Math.floor(index / padsPerColour)]
    }

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        // Printed scale ticks on the faceplate.
        ctx.strokeStyle = knob.theme.inkDim
        ctx.lineCap = "round"
        for (let i = 0; i <= 10; ++i) {
            const a = start + sweep * i / 10
            const inner = radius + 1.5
            const outer = radius + (i % 5 === 0 ? 5.5 : 3.5)
            ctx.lineWidth = i % 5 === 0 ? 1.4 : 0.9
            ctx.beginPath()
            ctx.moveTo(c + Math.cos(a) * inner, c + Math.sin(a) * inner)
            ctx.lineTo(c + Math.cos(a) * outer, c + Math.sin(a) * outer)
            ctx.stroke()
        }
        // Skirt with a value arc printed on it.
        ctx.fillStyle = knob.theme.knobSkirt
        ctx.beginPath()
        ctx.arc(c, c, radius, 0, Math.PI * 2)
        ctx.fill()
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            ctx.strokeStyle = knob.accent
            ctx.lineWidth = 2.4
            ctx.beginPath()
            ctx.arc(c, c, radius - 2.5, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Coloured cap.
        const cap = radius * 0.62
        const fill = ctx.createRadialGradient(c - cap * 0.35, c - cap * 0.45, 1, c, c, cap)
        fill.addColorStop(0, Qt.lighter(knob.accent, 1.25))
        fill.addColorStop(1, Qt.darker(knob.accent, 1.18))
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, cap, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? 2 : 1
        ctx.strokeStyle = knob.activeFocus ? knob.theme.led : Qt.darker(knob.accent, 1.6)
        ctx.stroke()
        // Pointer across skirt and cap.
        ctx.strokeStyle = knob.theme.pointer
        ctx.lineWidth = 2.4
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * cap * 0.15, c + Math.sin(at) * cap * 0.15)
        ctx.lineTo(c + Math.cos(at) * (radius - 1.5), c + Math.sin(at) * (radius - 1.5))
        ctx.stroke()
        ctx.strokeStyle = knob.theme.knobSkirt
        ctx.lineWidth = 1.2
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * cap * 0.15, c + Math.sin(at) * cap * 0.15)
        ctx.lineTo(c + Math.cos(at) * cap * 0.92, c + Math.sin(at) * cap * 0.92)
        ctx.stroke()
    }

    // Header: moulded kit arrows and a printed status pill.
    function arrowFill(arrow, stop) {
        return stop === 0 ? bodyLight : bodyDark
    }
    function arrowEdge(arrow) {
        return arrow.activeFocus ? red : seam
    }
    function statusFill(tone) {
        return strip
    }
    function statusInk(tone) {
        return stripText
    }

    // The pad envelope on the display strip's LCD: a crisp grid, the -60 dB
    // point dashed under a glowing fill, captions above and below.
    envelopeRadius: 4
    envelopeWell: lcd
    envelopeEdge: stripLine
    envelopeMargin: 1
    envelopePlotSide: 8
    envelopePlotTop: 22
    // The scale sits under the LCD (Kit.PlotFrame), so the plot needs no
    // room for labels at its foot.
    envelopePlotBottom: envelopePlotSide
    envelopeLineWidth: 1.8
    envelopeLineJoin: "round"
    envelopeFillOverLine: false
    envelopeFillAlpha: 0.45
    envelopeGradient: true
    envelopeFillEndAlpha: 0.03
    envelopeGridRows: 4
    envelopeGridColumns: 6
    envelopeGridInk: lcdGrid
    envelopeGridSnap: true
    envelopeMarkerInk: Qt.rgba(lcdText.r, lcdText.g, lcdText.b, 0.35)
    envelopeMarkerSnap: true
    envelopeMarkersOver: false
    envelopeLabelInk: stripDim
    envelopeLabelSize: 9
    envelopeLabelSpacing: 1.4
    envelopeLabelInset: 9
    envelopeLabelTop: 5
    plotTickInk: envelopeLabelInk
    plotTickSize: envelopeLabelSize
    plotChipFill: lcd

    // ---- Faceplate layout ------------------------------------------------------
    property int panelWidth: 1240
    property int panelHeight: 700
    property real panelRadius: 10
    // Where the faceplate's middle tone sits in its top-to-bottom gradient.
    property real caseGradientMid: 0.6
    property real sectionGap: 10

    // Engraved section plates: a pale wash, a dark rim, a lit top lip.
    property color plateWash: Qt.rgba(1, 1, 1, 0.16)
    property color plateRim: Qt.rgba(0, 0, 0, 0.14)
    property color plateLip: Qt.rgba(1, 1, 1, 0.45)
    property real plateTitleTop: 7
    property real plateTitleSpacing: 2.2
    property real plateBodyTop: 28
    property color dividerInk: Qt.rgba(0, 0, 0, 0.12)

    // Header (Kit.DeviceHeader): four slanted colour stripes, the italic
    // wordmark and its tagline; the kit's LCD between moulded arrows, and
    // the touched control's readout on a second LCD.
    property real headerTop: 10
    headerHeight: 46
    property real stripesTop: 6
    property real stripeGap: 3
    property int stripeWidth: 8
    property int stripeHeight: 30
    property real stripeRadius: 1
    property color stripeEdge: Qt.rgba(0, 0, 0, 0.18)
    // Horizontal shear of each stripe per pixel of height.
    property real stripeSlant: -0.22
    property real wordmarkGap: 14
    wordmarkSize: 30
    wordmarkWeight: Font.Black
    wordmarkSpacing: 3
    wordmarkInk: ink
    property real taglineIndent: 2
    property real taglineRise: 4
    subtitleSize: typeCaption
    subtitleSpacing: 2.4
    subtitleInk: red
    // Letter spacing of small printed legends (GR, strip names).
    property real legendSpacing: 1

    presetDisplayWidth: 330
    presetDisplayHeight: 36
    presetDisplayRadius: radiusMd
    presetFill: lcd
    presetEdge: strip
    presetFocusStroke: strokeBold
    presetTextX: 10
    presetTagY: 3
    presetTagInk: orange
    presetTagSpacing: 1.2
    presetNameY: 14
    presetNameInk: lcdText
    presetChevronMargin: 9
    presetChevronInk: stripDim
    readoutWidth: 220
    readoutFill: lcd
    readoutEdge: strip
    readoutRadius: radiusMd
    readoutTextX: 10
    readoutNameY: 3
    readoutNameInk: stripDim
    readoutValueY: 15
    readoutValueInk: orange

    // Selected pad editor.
    property int editorTop: 66
    property int editorWidth: 752
    property int editorHeight: 250
    property int badgeWidth: 304
    property int badgeHeight: 46
    property real badgeInset: 10
    property int badgeSwatch: 22
    property real badgeSwatchRimDarken: 1.5
    property real badgeNameX: 42
    property real badgeNameTop: 5
    property real badgeNameSpacing: 1.5
    property real badgeCaptionX: 43
    property real badgeCaptionTuck: 1
    property real badgeCaptionSpacing: 1.4
    property real padKnobsTop: 84
    property int padKnobColumns: 4
    property int padKnobCellWidth: 78
    property int padKnobCellHeight: 76
    property int padKnobDiameter: 44
    property real editorDisplayX: 328
    property int envelopeHeight: 104
    property real shapeTop: 140

    // Kit bus.
    property real busInset: 10
    property real busKnobsTop: 34
    property int busColumns: 2
    property real busRowGap: 18
    property int busCellWidth: 170
    property int busCellHeight: 86
    property int busKnobDiameter: 50
    property real busDividerX: 362
    property real busDividerTop: 30
    property real busDividerTrim: 44
    property real glueMeterX: 376
    property real glueMeterTrim: 48

    // Mixer channels.
    property int mixerHeight: 164
    property real channelInset: 3
    property real channelTop: 24
    property real channelTrim: 28
    property color channelSelectedWash: Qt.rgba(1, 1, 1, 0.32)
    property color channelSelectedRim: Qt.rgba(0, 0, 0, 0.16)
    property real channelRuleTop: 30
    property real channelRuleTrim: 40
    property color channelRuleInk: Qt.rgba(0, 0, 0, 0.08)
    property real channelPanTop: 34
    property int channelPanDiameter: 26
    property int channelPanReadoutWidth: 46
    property int channelLevelReadoutWidth: 50
    property real channelReadoutGap: 3
    property real channelFaderTop: 30
    property real channelFaderTrim: 52

    // Instrument strip above the pads.
    property int stripHeight: 44
    property real stripRuleInset: 8
    property real stripNameTop: 7
    property real stripKeyTop: 24
    property real padsBottomMargin: 14

    // Pads: select LED and halo, drop shadow, sounding ring, the tinted face.
    property int padWidth: 92
    property int padHeight: 104
    property int padLedSize: 10
    property int padLedHaloSize: 20
    property color padLedHalo: Qt.rgba(1, 0.25, 0.15, 0.22)
    property real padLedRimDarken: 1.6
    property real padShadowX: 1
    property real padShadowDrop: 4
    property color padShadow: Qt.rgba(0, 0, 0, 0.22)
    property real padRingOutset: 3
    property real padFaceInset: 4
    property real padFaceTop: 18
    property real padFaceTrim: 24
    property real padPressTravel: 2
    property real padRadius: 5
    property real padSheen: 1.12
    property real padGradientMid: 0.85
    property real padShade: 1.15
    property real padRimDarken: 1.45
    property real padLipInset: 3
    property real padLipTop: 2
    property real padLipHeight: 3
    property color padLip: Qt.rgba(1, 1, 1, 0.28)
    property color padFlash: "#ffffff"
    property real padLiveOpacity: 0.55
    property real padFlashOpacity: 0.75
    property int padFlashMs: 260
    property color padNumberInk: Qt.rgba(0, 0, 0, 0.45)

    // Glue gain-reduction meter.
    property int glueMeterWidth: 34
    property int glueMeterHeight: 150
    property real glueWellTop: 16
    property int glueWellWidth: 10
    property real glueSegmentInset: 2
    property real glueSegmentPad: 3
    property real glueSegmentGap: 2
    property real glueSegmentRadius: 1
    // The top segments light in the LED colour, the rest in red.
    property int glueHotSegments: 4
    property real glueSegmentOffOpacity: 0.6
    property real glueMarkerOverhang: 5
    property real glueScaleGap: 3
    property real glueScaleRise: 5

    // Pitch-drop / noise-band display, on the envelope's LCD plot insets.
    property real shapeFillAlpha: 0.4
    property real shapeFillEndAlpha: 0.02
    property real shapeAxisInset: 10
}
