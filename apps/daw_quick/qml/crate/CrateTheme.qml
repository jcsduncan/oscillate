import QtQuick
import "../devicekit"

// Crate design tokens: a near-black crate with warm off-white ink, amber for the instrument and
// one colour per chop kind (kick amber, snare teal, hat lilac, other bone).
// The selected chop's controls wear its kind's colour.
KitTheme {
    prefix: "crate"

    // The case and its parts.
    caseColor: "#0e1014"
    property color headerFill: "#12151a"
    property color characterFill: "#101318"
    property color line: "#22262e"
    property color line2: "#2a2f38"
    property color centreLine: "#1c2027"
    property color padFill: "#15181e"
    property color padSelected: "#1d2129"
    property color padEdge: "#262b34"
    property color cardFill: "#12151a"
    property color buttonFill: "#171a20"
    property color knobTrackInk: "#262b34"
    property color knobFace: "#171a20"
    property color segIdleInk: "#b9bdc7"
    property color subInk: "#b9bdc7"
    // Text on an amber (or kind-coloured) fill.
    property color onAccent: "#14110b"
    property color markerIdle: Qt.rgba(232 / 255, 228 / 255, 218 / 255, 0.28)
    property color playhead: "#ffffff"
    property color dropFill: "#11141a"

    accent: "#f0a23b"
    accent2: "#58c4bb"
    accentSoft: "#7a5420"
    plate: caseColor
    plateEdge: line
    plateHi: line2
    well: "#0b0d10"
    wellEdge: line2
    seam: line
    grid: centreLine
    gridStrong: line2
    ink: "#e8e4da"
    inkDim: "#8d93a0"
    inkFaint: "#767d8b"
    glacier: "#f4f1ea"
    button: buttonFill
    buttonHi: "#1f232b"
    warning: "#ff8a5c"
    live: "#7ee2a8"
    tipFill: well
    tipEdge: line2
    menuFill: headerFill
    menuEdge: line2

    sans: "IBM Plex Sans"
    mono: "IBM Plex Mono"
    tipFont: mono

    // Type: no text under 10 px.
    wordmarkSize: 20
    property int captionSize: 11
    property int labelSize: 10
    property int valueSize: 12
    property int bodySize: 12
    property int titleSize: 15
    property int heroSize: 22
    property int nameSize: 14
    property real captionSpacing: 1.5
    property real labelSpacing: 1.2

    // Geometry.
    property int gutter: 20
    property int segHeight: 26
    property int segRadius: 7
    property int buttonHeight: 32
    // The kit's Button: outlined, or filled in its colour when prime or on.
    pushButtonHeight: buttonHeight
    pushButtonPad: 28
    pushButtonRadius: radiusLg
    pushButtonTextSize: bodySize
    pushButtonLetterSpacing: 1
    pushButtonDisabledOpacity: knobDisabledOpacity
    pushButtonFace: buttonFill
    pushButtonHover: buttonHi
    pushButtonIdleEdge: line2
    pushButtonHoverEdge: line2
    pushButtonFocusEdge: ink
    pushButtonIdleInk: ink
    pushButtonHoverInk: ink
    pushButtonTipFallback: false
    accentInk: onAccent
    property int waveHeight: 150
    property int padHeight: 72
    property int padGap: 8
    property int padsWidth: 372
    property int characterWidth: 330
    property int sampleNameWidth: 240
    property int chopNameWidth: 150
    property int knobDial: 52
    property int knobDialSmall: 34
    property int knobStroke: 4
    property int knobStrokeSmall: 3
    cardRadius: 8
    property int viewHeight: 70
    // The filter curve sits a little shorter than the envelope beside it.
    property int filterCurveHeight: viewHeight - 6

    // The panel and its layout.
    property int panelWidth: 1440
    property int panelHeight: 680
    property real caseRadius: 12
    // The header (Kit.DeviceHeader) on its band: the band's padding above
    // and below; the first row's height; between its groups, between items
    // in a group; the wordmark's and tagline's tracking. ERA and TIME sit in
    // the second row. No presets: the sample sits in the browser's place,
    // and a load message gets at most messageWidth beside it. The readout
    // matches the sample box.
    property real headerY: 8
    property real headerBottom: 8
    headerHeight: 40
    headerModeGap: spaceSm
    headerGap: 20
    property real headerItemGap: 10
    wordmarkSpacing: 3
    property real taglineSpacing: 1
    property real messageWidth: 320
    presetDisplayHeight: sampleBoxHeight
    readoutFill: caseColor
    readoutEdge: line2
    readoutRadius: radiusLg
    readoutValueY: 14
    readoutValueSize: valueSize
    // The sample box and its record icon.
    property int sampleBoxHeight: 32
    property real sampleBoxInset: 12
    property real recordIconSize: 16
    property real recordIconStroke: 1.2
    property real recordIconHole: 4
    // The chop section: the mode bar's top, the gaps around the waveform,
    // and the gap between blocks (mode bar groups, chop editor rows).
    property real modeBarTop: 14
    property real waveGap: 10
    property real waveBottom: 12
    property real blockGap: 14
    property real equalCountWidth: 28
    // The lower section: its top (and the blurb's bottom) margin, the gap
    // under the pads header, the chop editor's side insets.
    property real lowerTop: 16
    property real padsHeaderGap: 10
    property real chopEditorInset: 22
    property real chopSwatchSize: 10
    property int chopButtonHeight: buttonHeight - 2
    // The envelope and filter cards' insets.
    property real cardInset: 12
    property real envelopeCardTop: 10
    property real filterCardTop: 8
    // The character knobs' grid and the era blurb's line height.
    property int characterColumns: 3
    property real characterGridTop: 12
    property real characterRowGap: 10
    property real blurbLineHeight: 1.5
    // The empty drop zone's edge and the drop highlight's wash.
    property real dropEdgeWidth: 1.5
    // The empty state's notice (the shared LibraryNotice), centred under its prompt.
    property real dropNoticeWidth: 520
    property real dropWashAlpha: 0.08
    // DragValue (SWING, SENS): its side inset and corners.
    property real dragValueInset: 8
    property real dragValueRadius: 5

    // CrateSegs: the well's inset around the tiles, its height (compact
    // shorter), corners and each tile's padding beside its text.
    property real segInset: 3
    property int segWellHeight: segHeight + 4
    property int segCompactHeight: segHeight - 6
    property real segCompactRadius: 5
    property real segTilePad: 20
    property real segTilePadCompact: 12
    property real segTileRadius: 5
    property real segTileRadiusCompact: radiusMd
    property real segLetterSpacing: 1

    // Pads: corners, the edge while selected or sounding, an empty pad's
    // dimming, the selection halo (a faint ring just outside), the wash
    // while sounding, the inner inset, the last-struck dot and the bar.
    property real padRadius: 7
    property real padActiveEdgeWidth: 1.5
    property real padEmptyOpacity: 0.45
    property real padHaloGap: 3
    property real padHaloWidth: strokeBold
    property real padHaloAlpha: 0.2
    property real padLiveAlpha: 0.16
    property real padInset: 9
    property real padLastDotX: 22
    property real padLastDotY: 4
    property real padLastDotSize: 6
    property real padBarHeight: 3

    // The waveform's chop markers: the width a marker takes the pointer.
    property real markerHitWidth: minHitSize
    // The accent edge a focused control of the panel's own draws (the
    // waveform, the chop's name).
    property real focusEdgeWidth: strokeBold
    // The waveform's time scale: the steps its ticks may take, in seconds,
    // and the most ticks it shows.
    property var waveTickSteps: [0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60]
    property int waveMostTicks: 8
    // The filter curve: inset top and bottom, and its dimming while off.
    property real filterCurvePad: 3
    property real filterCurveOffAlpha: 0.4
    // Knobs (CrateKnob over the kit Knob): the dial, then the value, then the
    // label; no easing, no macro rings (right-click opens the macro menu);
    // Ctrl+click resets; L/R pan may be typed.
    knobCaptions: false
    knobEases: false
    knobMacros: false
    knobDialPad: 0
    knobWidthPad: 8
    knobCaptionGap: 2
    knobCtrlClickResets: true
    knobTypePan: true
    knobDragPixels: 160
    knobFinePixels: 600
    // The value's gap under the dial and each text line's allowance in the
    // knob's height; the caption's padding in its width.
    property real knobValueGap: 4
    property real knobLinePad: 4
    property real knobCaptionPad: 4
    property int knobValueSizeSmall: labelSize + 1
    // The face: the arc's ring sits inside the dial's edge, the face inside
    // the arc; a value at the origin shows a short nub; the index dot sits
    // out along the face's radius.
    property real knobArcRim: 1
    property real knobFaceGap: 2
    property real knobArcNub: 0.02
    property real knobIndexRadius: 0.62
    property real knobDot: 2
    property real knobDotSmall: 1.5
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const stroke = knob.small ? knobStrokeSmall : knobStroke
        const r = c - stroke / 2 - knobArcRim
        const start = knobStartAngle
        const sweep = knobSweepAngle
        const at = start + sweep * Math.max(0, Math.min(1, knob.position))
        ctx.lineCap = "round"
        ctx.lineWidth = stroke
        ctx.strokeStyle = knobTrackInk
        ctx.beginPath()
        ctx.arc(c, c, r, start, start + sweep)
        ctx.stroke()
        if (knob.bound) {
            const from = start + sweep * knob.origin
            ctx.strokeStyle = knob.accent
            ctx.beginPath()
            if (Math.abs(at - from) > knobArcMinimum)
                ctx.arc(c, c, r, Math.min(from, at), Math.max(from, at))
            else
                ctx.arc(c, c, r, at - knobArcNub, at + knobArcNub)
            ctx.stroke()
        }
        // The face, ringed in the colour while focused, with an index dot.
        const inner = r - stroke - knobFaceGap
        ctx.fillStyle = knobFace
        ctx.beginPath()
        ctx.arc(c, c, inner, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = hairline
        ctx.strokeStyle = knob.activeFocus ? knob.accent : line2
        ctx.stroke()
        if (knob.bound) {
            ctx.fillStyle = glacier
            ctx.beginPath()
            ctx.arc(c + Math.cos(at) * inner * knobIndexRadius, c + Math.sin(at) * inner * knobIndexRadius,
                    knob.small ? knobDotSmall : knobDot, 0, Math.PI * 2)
            ctx.fill()
        }
    }
    // The selected chop bright, the others dim (waveform bars, pad bars).
    property real chopOnAlpha: 0.95
    property real chopOffAlpha: 0.38
    property real padBarOffAlpha: 0.55

    // The chop envelope: bare (no well), a bold outline over the chop's
    // colour faded to a soft fill.
    envelopeRadius: 0
    envelopeWell: "transparent"
    envelopeEdgeWidth: 0
    envelopeMargin: 0
    envelopePlotTop: 4
    envelopePlotBottom: 2
    envelopeLineWidth: 2
    envelopeFillOverLine: false
    envelopeFillOpacity: 0.12
}
