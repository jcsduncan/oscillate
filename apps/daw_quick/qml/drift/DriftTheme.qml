import QtQuick
import "../devicekit"

// Drift design tokens, "Nebula": a deep-space body with soft aurora light
// (teal, violet, magenta), glass plates and glowing controls.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "drift"
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
    dropdownHeight: 22
    dropdownTextX: 7
    dropdownTextRight: 22
    dropdownChevronMargin: 6
    arrowWidth: 26
    arrowRadius: 8
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 10
    arrowInk: accent

    property color spaceTop: "#0b1024"
    property color space: "#070a17"
    property color spaceBottom: "#04060e"
    seam: "#1a2142"
    plate: "#0c1129"
    plateEdge: "#1c2550"
    well: "#060915"
    wellEdge: "#18204a"
    grid: "#141b3a"
    ink: "#e9edff"
    inkDim: "#8d95bd"
    inkFaint: "#747ca8"

    // Aurora: teal into violet, magenta for reversed grains, ice for frost.
    accent: "#46e6cf"
    accent2: "#9b7bff"
    property color magenta: "#ff6bd6"
    property color frost: "#bfe8ff"

    knobRing: "#03050c"
    knobTop: "#1d2546"
    knobBottom: "#0b1026"
    pointer: "#f2f5ff"
    button: "#0f1530"
    buttonHi: "#172042"
    ledOff: "#232b52"

    warning: "#e0901f"
    live: "#46e6cf"

    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        ctx.lineCap = "round"
        // Track.
        ctx.lineWidth = 3
        ctx.strokeStyle = knob.theme.grid
        ctx.beginPath()
        ctx.arc(c, c, radius + 2, start, start + sweep)
        ctx.stroke()
        // Value arc with a soft glow underneath.
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            const aurora = ctx.createLinearGradient(0, size, size, 0)
            aurora.addColorStop(0, knob.accent)
            aurora.addColorStop(1, knob.accent2)
            ctx.strokeStyle = Qt.rgba(knob.accent.r, knob.accent.g, knob.accent.b, 0.18)
            ctx.lineWidth = 8
            ctx.beginPath()
            ctx.arc(c, c, radius + 2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
            ctx.strokeStyle = aurora
            ctx.lineWidth = 3
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
        // A faint inner glow, brighter as the value rises.
        const glow = ctx.createRadialGradient(c, c, 0, c, c, body)
        glow.addColorStop(0, Qt.rgba(knob.accent.r, knob.accent.g, knob.accent.b, 0.05 + 0.14 * knob.position))
        glow.addColorStop(1, "transparent")
        ctx.fillStyle = glow
        ctx.beginPath()
        ctx.arc(c, c, body, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? 1.8 : 1
        ctx.strokeStyle = knob.activeFocus ? knob.accent : knob.theme.knobRing
        ctx.stroke()
        // Pointer: a short bright line with a dot at the rim.
        ctx.strokeStyle = knob.theme.pointer
        ctx.lineWidth = 2
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
        return tile.on ? tile.accent : knobRing
    }

    // The amp envelope: the kernel's samples under an aurora fade, with the
    // release point dashed and the length in the corner.
    property int envelopeSamples: 160
    envelopeRadius: 6
    envelopeEdge: wellEdge
    envelopeMargin: 6
    envelopeMarginTop: 16
    envelopePlotTop: 0
    envelopePlotBottom: 0
    envelopeHeadroom: 2
    envelopeLineWidth: 1.8
    envelopeFillOverLine: false
    envelopeFillAlpha: 0.3
    envelopeGradient: true
    envelopeFadeToClear: true
    envelopeMarkerInk: inkFaint
    envelopeMarkerWidth: 1.8
    envelopeLabelInk: inkDim
    envelopeLabelFont: mono
    envelopeLabelSize: 9
    envelopeLabelInset: 8
    envelopeLabelTop: 3

    // The case: rounded, its gradient passing through `space` here.
    property real caseRadius: 12
    property real spaceStop: 0.55
    // Aurora light behind the glass: soft radial glows, each placed as a
    // fraction of the panel, with a radius in pixels and a strength.
    property var auroraLights: [
        { x: 0.18, y: 0.12, radius: 420, colour: accent, alpha: 0.10 },
        { x: 0.72, y: 0.05, radius: 460, colour: accent2, alpha: 0.12 },
        { x: 0.92, y: 0.55, radius: 380, colour: magenta, alpha: 0.07 },
        { x: 0.3, y: 0.85, radius: 420, colour: accent2, alpha: 0.06 }
    ]

    // Glass plates: the section cards, their title and the note beside it.
    property real plateRadius: 10
    property real plateAlpha: 0.82
    property real plateTitleX: 14
    property real plateTitleY: 10
    property int plateTitleSize: typeSmall
    property real plateTitleSpacing: 3
    property real plateNoteGap: 10
    property real plateNoteY: 11

    // Small bold captions under and beside controls.
    property int captionSize: typeMicro
    property real captionSpacing: 1.2

    // The wordmark, its aurora rule and the tagline under it.
    property int wordmarkSize: 32
    property real wordmarkSpacing: 12
    property real wordmarkInset: 2
    property real wordmarkRuleTrim: 14
    property real taglineGap: 6
    property real taglineSpacing: 2

    // The preset display: name, the touched control's readout and a chevron.
    property real litEdgeWidth: 1.5
    property int presetNameSize: typeTitle
    property int readoutValueSize: typeSubtitle
    property real readoutLabelSpacing: 1
    property real chevronInset: 10
    property int chevronSize: typeBody

    // Knob cells: a knob with its caption, at least cellMinWidth wide.
    property int cellDiameter: 30
    property real cellWidthPad: 22
    property real cellMinWidth: 62
    property real cellHeightPad: 30
    // Between the Blur macro and the big switches.
    property real motionSpacing: 10
    property int stepperHeight: 22
    // A control waiting on another (a division while free-running).
    property real unsyncedOpacity: 0.45
    // The grain window drawn on a shape key.
    property int glyphSamples: 24
    property real glyphStroke: 1.2

    // The big switches (Freeze, Sync): a glowing LED, a halo while on.
    property real toggleWidth: 108
    property real toggleHeight: 52
    property real toggleRadius: 10
    property real toggleDisabledOpacity: 0.5
    property real toggleOnAlpha: 0.16
    property real toggleHaloSpread: 4
    property real toggleHaloAlpha: 0.18
    property real toggleLedX: 12
    property real toggleLedSize: 10
    property real toggleLedRingSize: 18
    property real toggleLedRingWidth: 3
    property real toggleLedRingAlpha: 0.3
    property real toggleTextX: 30
    property int toggleTitleSize: typeEmphasis
    property real toggleTitleSpacing: 2.2

    // The small views (partials, tail): padding around the plot and labels.
    property real viewPad: 6
    property real viewLabelX: 8
    property real viewLabelY: 3
    property real spectrumLabelY: 4
    property real spectrumTop: 18
    property real hazeAlpha: 0.45
    property real partialStroke: 2.5
    property real decayTop: 16
    property real tailAlpha: 0.45
    property real shimmerLineAlpha: 0.85
    property real shimmerLineWidth: 1.5
    property var rtDash: [3, 3]
    property int decayReadoutSize: typeCaption

    // The grain cloud.
    property real cloudWashOpacity: 0.55
    property color cloudWashLeft: Qt.rgba(0.36, 0.25, 0.85, 0.10)
    property color cloudWashMiddle: Qt.rgba(0.10, 0.55, 0.62, 0.08)
    property color cloudWashRight: Qt.rgba(0.95, 0.35, 0.78, 0.08)
    property real cloudPlotLeft: 34
    property real cloudPlotRight: 12
    property real cloudPlotVertical: 26
    property color star: Qt.rgba(1, 1, 1, 0.22)
    property real starLarge: 1.5
    property real starSmall: 1
    property color cloudZeroLine: Qt.rgba(1, 1, 1, 0.16)
    property real sprayAlpha: 0.08
    property real grainGlowAlpha: 0.18
    property real grainGlowWidth: 7
    property color scanLine: Qt.rgba(1, 1, 1, 0.55)
    property var scanDash: [4, 4]
    property real syncLaneAlpha: 0.08
    property real syncLaneHeight: 14
    property real syncTickAlpha: 0.9
    property real syncTickWidth: 1.5
    property real syncTickMajor: 13
    property real syncTickMinor: 8
    property real syncTickInset: 2
    property color frostTop: Qt.rgba(0.75, 0.91, 1.0, 0.16)
    property color frostBottom: Qt.rgba(0.75, 0.91, 1.0, 0.05)
    property color frostEdge: Qt.rgba(0.75, 0.91, 1.0, 0.35)
    property color frostCrystal: Qt.rgba(0.85, 0.95, 1.0, 0.35)
    property real frostLabelInset: 10
    property real frostLabelY: 8
    property real frostLabelSpacing: 2
    property real cloudTitleX: 12
    property real cloudTitleY: 7
    property real cloudTitleSpacing: 1.6
    property real syncCaptionInset: 4
    property real syncCaptionLift: 26
    property real axisLabelX: 6
    property real axisLabelLift: 6
    property real timeLabelLift: 15
}
