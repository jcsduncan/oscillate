import QtQuick
import "../devicekit"

// Concert design tokens, "Concert black": a gloss-black lacquer case with
// brass hardware, gold leaf lettering and ivory keys.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "concert"
    accent: gold
    tipRadius: 3
    knobDiameter: 36
    knobDialPad: 12
    knobWidthPad: 22
    knobMinWidth: 62
    knobHeightPad: 32
    knobLongName: "group"
    knobCaptionGap: 2
    knobLabelFont: serif
    knobLabelSize: 9
    knobLabelSpacing: 1.1
    segmentHeight: 24
    segmentSpacing: 3
    segmentFocusRadius: 4
    segmentRadius: 3
    segmentRadiusVertical: 3
    segmentFillAcross: false
    segmentLetterSpacingVertical: 1.2
    segmentTextXVertical: 14
    segmentFont: serif
    segmentCaptionGap: 1
    arrowWidth: 26
    arrowRadius: 5
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 10
    arrowInk: brass

    // Lacquer: near-black with a cool sheen at the top edge.
    property color lacquerTop: "#1a1a1f"
    property color lacquer: "#0b0b0e"
    property color lacquerBottom: "#050507"
    property color sheen: "#3a3a44"
    seam: "#24242b"
    // Inset wells (the lid's shadow).
    well: "#030304"
    wellEdge: "#2a2620"
    plate: "#0e0e12"
    plateEdge: "#2b2720"

    // Brass hardware and gold leaf.
    property color brass: "#b8924a"
    property color brassHi: "#e3c27a"
    property color brassLo: "#6d5426"
    property color gold: "#e8c77a"
    property color goldDim: "#8f7746"

    // Ivory and ebony keys.
    property color ivory: "#efe6d2"
    property color ivoryShade: "#cbbf9f"
    property color ebony: "#121214"
    property color felt: "#7a1f24"
    property color feltLifted: "#b53a40"

    ink: "#f1ead9"
    inkDim: "#a49a85"
    inkFaint: "#847b69"

    knobTop: "#26262c"
    knobBottom: "#08080a"
    // The edge round the knob's lacquered cap.
    property color capEdge: "#000000"
    pointer: "#f6e3ae"
    button: "#111115"
    buttonHi: "#1b1b21"
    ledOff: "#2a2620"

    warning: "#e0901f"
    live: "#e8c77a"

    mono: "DejaVu Sans Mono"
    serif: "DejaVu Serif"

    // The control last touched, read back in the header.
    lastTouched: ""

    // The case: its size, corner and the lacquer's diagonal reflection.
    property int panelWidth: 1280
    property int panelHeight: 656
    property real caseRadius: 10
    property real sheenAlpha: 0.10
    property real sheenTailAlpha: 0.02

    // The layout: header, then three rows of plates.
    property real plateGap: 10
    property real headerY: 10
    headerHeight: 52
    // Between the header and the first row of plates.
    property real headerContentGap: 10
    property real rowAHeight: 268
    property real rowBHeight: 150
    property real rowCHeight: 128
    property real libraryWidth: 300

    // Plates: lacquer framed by a fine brass line, titled in gold.
    property real plateAlpha: 0.88
    property real plateFrameInset: 3
    property real plateFrameAlpha: 0.18
    // The left inset of a plate's title and text.
    property real plateInset: 14
    property real plateTitleY: 9
    property real plateTitleSpacing: 3
    property real plateNoteGap: 10
    property real plateNoteY: 11
    // Where a plate's contents start below its title, and its text below that.
    property real plateBodyY: 28
    property real plateTextY: 30
    // Small bold capitals: notes, captions and button labels.
    property real captionSpacing: 1.2

    // Knob cells: a knob over its caption.
    property real cellWidth: 78
    property real cellCaptionHeight: 34

    // The header (Kit.DeviceHeader): the gold serif wordmark over its brass
    // rule and subtitle; the preset display and, once touched, the readout.
    wordmarkFont: serif
    wordmarkSize: 28
    wordmarkWeight: Font.Normal
    wordmarkSpacing: 9
    wordmarkInk: gold
    subtitleGap: 6
    subtitleSpacing: 2.2
    subtitleInk: inkDim
    property real wordmarkInset: 2
    property real wordmarkRuleGap: 1
    property real wordmarkRuleTrim: 10
    headerBrowserX: 252
    presetDisplayWidth: 300
    presetDisplayHeight: 42
    presetDisplayRadius: 5
    presetEdge: wellEdge
    presetTextX: plateInset
    presetTagY: 5
    presetTagSpacing: captionSpacing
    presetNameY: 16
    presetNameFont: serif
    presetNameSize: typeTitle
    presetNameBold: false
    presetChevronMargin: 10
    presetChevronInk: brass
    readoutWidth: 240
    readoutRadius: 5
    readoutTextX: plateInset
    readoutNameY: 5
    readoutSpacing: 1
    readoutValueY: 17
    readoutValueInk: gold

    // The library's brass push buttons.
    property real actionPad: 20
    property real actionHeight: 24

    // The stereo plate: the width knob beside the perspective choice.
    property real stereoKnobX: 24
    property real stereoKnobY: 43
    property real stereoChoiceX: 140
    property real stereoCaptionY: 38
    property real stereoChoiceY: 52
    property real stereoChoiceHeight: 54

    // The strings view: soundboard, strings, dampers and keys.
    property color soundboardTop: "#130f0a"
    property color soundboardBottom: "#070605"
    // Wound bass strings are copper; plain treble strings steel.
    property color woundString: Qt.rgba(0.62, 0.42, 0.24, 0.55)
    property color plainString: Qt.rgba(0.72, 0.72, 0.74, 0.45)
    property real woundStringWidth: 1.6
    // A ringing string's gold halo and its lit core, widening with its glow.
    property real stringHaloAlpha: 0.22
    property real stringHaloWidth: 3
    property real stringHaloSpread: 6
    property real stringLitSpread: 1.5
    // The lit edge on a lifted damper's felt.
    property color feltSheen: Qt.rgba(1, 0.85, 0.8, 0.35)
    // The sharps' top edge.
    property color ebonyHi: "#2a2a30"
    property real stringsLabelSpacing: 1.6
    property real stringsCaptionSpacing: 1.4

    // The lyre's pedals.
    property real pedalSpacing: 26
    property real pedalWidth: 76
    property real pedalBottomGap: 6
    property real pedalTravel: 3
    property real pedalPlateWidth: 38
    property real pedalPlateHeight: 11
    property real pedalPlateRadius: 5
    property real pedalHaloSpread: 3
    property real pedalHaloAlpha: 0.45
    property real pedalLabelY: 16
    property int pedalLabelSize: typeMicro
    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        // Brass bezel.
        const bezel = ctx.createLinearGradient(0, c - radius, 0, c + radius)
        bezel.addColorStop(0, knob.theme.brassHi)
        bezel.addColorStop(0.5, knob.theme.brass)
        bezel.addColorStop(1, knob.theme.brassLo)
        ctx.fillStyle = bezel
        ctx.beginPath()
        ctx.arc(c, c, radius, 0, Math.PI * 2)
        ctx.fill()
        // Engraved scale: an inset groove with the gold value inlaid.
        ctx.lineCap = "butt"
        ctx.lineWidth = 2.5
        ctx.strokeStyle = knob.theme.brassLo
        ctx.beginPath()
        ctx.arc(c, c, radius - 2.2, start, start + sweep)
        ctx.stroke()
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            ctx.strokeStyle = knob.theme.gold
            ctx.beginPath()
            ctx.arc(c, c, radius - 2.2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Lacquered cap with a highlight across the top.
        const cap = radius - 4.5
        const fill = ctx.createRadialGradient(c - cap * 0.3, c - cap * 0.45, cap * 0.1, c, c, cap)
        fill.addColorStop(0, knob.theme.knobTop)
        fill.addColorStop(1, knob.theme.knobBottom)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, cap, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? 1.6 : 0.8
        ctx.strokeStyle = knob.activeFocus ? knob.theme.gold : knob.theme.capEdge
        ctx.stroke()
        // Ivory pointer.
        ctx.lineCap = "round"
        ctx.strokeStyle = knob.theme.pointer
        ctx.lineWidth = 2
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * cap * 0.3, c + Math.sin(at) * cap * 0.3)
        ctx.lineTo(c + Math.cos(at) * (cap - 2), c + Math.sin(at) * (cap - 2))
        ctx.stroke()
    }

    // Segment plates: brass-edged when chosen, gold lettering.
    function segmentFill(tile, stop) {
        return stop === 0 && !tile.pressed ? buttonHi : button
    }
    function segmentEdge(tile) {
        return tile.on ? brass : seam
    }
    function segmentLabelInk(tile) {
        return inkDim
    }
    function segmentBold(tile) {
        return false
    }
    function segmentCaptionInk(tile) {
        return inkFaint
    }
}
