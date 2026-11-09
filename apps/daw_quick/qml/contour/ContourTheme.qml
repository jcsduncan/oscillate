import QtQuick
import "../devicekit"

// Contour design tokens: a precision lab instrument. Graphite casework, a
// near-black phosphor screen with a teal-tinted graticule, and one phosphor
// teal accent; live modulation reads in two signal colours, cool blue for
// the LFO and amber for the envelope.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "contour"
    tipRadius: 3
    menuHighlightAlpha: 0.18
    knobWidthPad: 26
    knobMinWidth: 64
    knobHeightPad: 44
    knobInactiveOpacity: 0.36
    knobLongName: "group"
    knobStacked: true
    knobCaptionGap: 0
    knobLabelFont: ""
    knobLabelSize: 9
    knobLabelBold: true
    knobLabelSpacing: 1.1
    knobLabelActiveInk: ink
    segmentWidth: 120
    segmentHeight: 24
    segmentSpacing: 2
    segmentFocusMargin: 2
    segmentFocusRadius: 4
    segmentRadius: 3
    segmentRadiusVertical: 3
    segmentFillAcross: false
    segmentLetterSpacing: 0.8
    dropdownBold: true
    arrowWidth: 26
    arrowHeight: 40
    arrowRadius: 5
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 10
    arrowInk: accent

    property color bodyTop: "#1d2023"
    property color body: "#17191c"
    property color bodyBottom: "#111315"
    seam: "#2b2f34"
    plate: "#1a1d20"
    plateEdge: "#292d32"
    plateHi: "#202428"
    well: "#080c0d"
    wellEdge: "#1d2528"
    property color screenGlow: "#0b1716"
    grid: "#11201e"
    property color gridMajor: "#1a3631"
    ink: "#e4ebe9"
    inkDim: "#8b9794"
    inkFaint: "#7a8985"

    accent: "#3df5d3"
    property color accentDim: "#1b7f6e"
    property color lfo: "#6cb6ff"
    property color envelope: "#ffb547"

    knobRing: "#060708"
    knobTop: "#2c3034"
    knobBottom: "#15171a"
    pointer: "#f2f6f5"
    button: "#1e2124"
    buttonHi: "#272b2f"
    ledOff: "#2b3033"

    warning: "#e0901f"
    live: "#3df5d3"

    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // The knob face: graduations outside the dial (ends and centre longer),
    // a phosphor value arc with a glow, a shadowed cap with a bevel ring and
    // a faint sheen, and a pointer line.
    property int knobFaceTicks: 10
    property real knobFaceTickInner: 3
    property real knobFaceTickMajor: 7.5
    property real knobFaceTickMinor: 5.5
    property real knobFaceArcOutset: 1
    property real knobFaceArcGlowWidth: 6
    property real knobFaceArcGlowAlpha: 0.22
    property real knobFaceCapInset: 3
    property string knobFaceShadow: "rgba(0,0,0,0.55)"
    property real knobFaceShadowDrop: 1.5
    property real knobFaceShadowSpread: 0.5
    property string knobFaceSheen: "rgba(255,255,255,0.07)"
    property string knobFaceSheenClear: "rgba(255,255,255,0.0)"
    property real knobFaceSheenSpan: 0.7
    property real knobFaceSheenRadius: 0.72
    property real knobFacePointerFrom: 0.35
    property real knobFacePointerInset: 2
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = knobStartAngle
        const sweep = knobSweepAngle
        const at = start + sweep * knob.position
        const ticks = knobFaceTicks
        ctx.lineCap = "butt"
        for (let i = 0; i <= ticks; ++i) {
            const a = start + sweep * i / ticks
            const major = i === 0 || i === ticks / 2 || i === ticks
            const outer = radius + (major ? knobFaceTickMajor : knobFaceTickMinor)
            ctx.strokeStyle = major ? knob.theme.inkFaint : knob.theme.ledOff
            ctx.lineWidth = hairline
            ctx.beginPath()
            ctx.moveTo(c + Math.cos(a) * (radius + knobFaceTickInner),
                       c + Math.sin(a) * (radius + knobFaceTickInner))
            ctx.lineTo(c + Math.cos(a) * outer, c + Math.sin(a) * outer)
            ctx.stroke()
        }
        // Value arc on the inner track, glowing like phosphor.
        const track = radius + knobFaceArcOutset
        ctx.lineCap = "round"
        ctx.lineWidth = strokeBold
        ctx.strokeStyle = knob.theme.grid
        ctx.beginPath()
        ctx.arc(c, c, track, start, start + sweep)
        ctx.stroke()
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > knobArcMinimum) {
            ctx.strokeStyle = Qt.rgba(knob.accent.r, knob.accent.g, knob.accent.b, knobFaceArcGlowAlpha)
            ctx.lineWidth = knobFaceArcGlowWidth
            ctx.beginPath()
            ctx.arc(c, c, track, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
            ctx.strokeStyle = knob.accent
            ctx.lineWidth = strokeBold
            ctx.beginPath()
            ctx.arc(c, c, track, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Cap: a shadowed machined body with a bevel ring.
        const body = radius - knobFaceCapInset
        ctx.fillStyle = knobFaceShadow
        ctx.beginPath()
        ctx.arc(c, c + knobFaceShadowDrop, body + knobFaceShadowSpread, 0, Math.PI * 2)
        ctx.fill()
        const fill = ctx.createLinearGradient(0, c - body, 0, c + body)
        fill.addColorStop(0, knob.theme.knobTop)
        fill.addColorStop(1, knob.theme.knobBottom)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, body, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? knobFocusEdgeWidth : hairline
        ctx.strokeStyle = knob.activeFocus ? knob.accent : knob.theme.knobRing
        ctx.stroke()
        const face = ctx.createLinearGradient(0, c - body * knobFaceSheenSpan, 0, c + body * knobFaceSheenSpan)
        face.addColorStop(0, knobFaceSheen)
        face.addColorStop(1, knobFaceSheenClear)
        ctx.fillStyle = face
        ctx.beginPath()
        ctx.arc(c, c, body * knobFaceSheenRadius, 0, Math.PI * 2)
        ctx.fill()
        // Pointer.
        ctx.strokeStyle = knob.theme.pointer
        ctx.lineWidth = strokeBold
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * body * knobFacePointerFrom, c + Math.sin(at) * body * knobFacePointerFrom)
        ctx.lineTo(c + Math.cos(at) * (body - knobFacePointerInset), c + Math.sin(at) * (body - knobFacePointerInset))
        ctx.stroke()
    }

    // Segment tiles brighten under the pointer.
    function segmentFill(tile, stop) {
        return tile.hovered ? buttonHi : button
    }
    function segmentEdge(tile) {
        return tile.on ? tile.accent : knobRing
    }

    // Dropdown: the well brightens under the pointer.
    function dropdownFill(dropdown) {
        return dropdown.hovered ? buttonHi : well
    }

    // The follower response: an amber curve with a glow (dimmer while the
    // amount is zero), a rule at rest, the burst shaded, labels underneath.
    envelopeRadius: 4
    envelopeEdge: wellEdge
    envelopeMargin: 6
    envelopeMarginBottom: 14
    envelopePlotTop: 1
    envelopePlotBottom: 1
    envelopeLineWidth: 1.5
    envelopeGlowWidth: 4
    envelopeGlowAlpha: 0.22
    property real envelopeIdleGlowAlpha: 0.25
    envelopeFilled: false
    envelopeBaseline: true
    envelopeBaselineInk: grid
    envelopeBaselineOffset: 0.5
    envelopeBaselineSnap: true
    envelopeBandFill: Qt.rgba(1, 1, 1, 0.04)
    envelopeLabelInk: inkFaint
    envelopeLabelSize: typeMicro
    envelopeLabelSpacing: 1
    envelopeLabelInset: 6
    envelopeLabelLift: 13

    // The panel: a graphite case of section plates.
    property int panelWidth: 1180
    property int panelHeight: 620
    property real panelRadius: 10
    property real sectionGap: 10
    // Small bold captions, tags and readout names.
    property real tagSpacing: 1.2
    property real captionSpacing: 1.3
    readoutSpacing: 1

    // Section plates: an engraved title with a tick before it and a note after.
    property real plateInset: 14
    property real plateContentY: 32
    property real plateTitleX: 20
    property real plateTitleY: 11
    property real plateTitleSpacing: 2.4
    property real plateTickY: 13
    property real plateTickWidth: 3
    property real plateTickHeight: 9
    property real plateTickRadius: 1
    property real plateNoteGap: 10
    property real plateNoteY: 12

    // The header (Kit.DeviceHeader): the wordmark over its subtitle, the
    // preset browser and, once a control is touched, its readout.
    property real headerY: 10
    headerHeight: 52
    wordmarkSize: 28
    wordmarkSpacing: 9
    subtitleSpacing: 1.6
    headerBrowserX: 250
    presetDisplayWidth: 250
    presetDisplayHeight: 40
    presetDisplayRadius: 5
    presetNameY: 15
    presetChevronMargin: 9
    readoutWidth: 220
    readoutValueY: 16

    // Rows of plates.
    property real rowAY: 72
    property real rowAHeight: 330
    property real screenPlateWidth: 740
    property real lfoPlateWidth: 520
    property real envelopePlateWidth: 386
    property real dialRowX: 8
    property real dialRowY: 106
    property real dialRowSpacing: 18

    // The live legend: a swatch per signal colour.
    property real legendSpacing: 14
    property real legendSwatchGap: 5
    property real legendSwatchWidth: 12
    property real legendSwatchHeight: 3
    property real legendSwatchRadius: 1.5

    // The model bank: LED keys in a grid under each family's caption.
    property real familyGap: 3
    property real modelKeyHeight: 26
    property real modelKeyStroke: 1.3
    property real modelKeyWashAlpha: 0.13
    property real modelLedX: 8
    property real modelLedSize: 5
    property real modelLabelOffset: 4
    property real bankDividerGap: 10
    property real filterDialX: 10
    property real filterDialGap: 18
    property int cutoffDiameter: 50

    // The LFO section.
    property real lfoViewX: 12
    property real lfoViewWidth: 190
    property real lfoViewHeight: 64
    property real lfoControlsX: 214
    property real lfoShapeCaptionY: 29
    property real lfoShapeY: 40
    property real lfoShapeWidth: 294
    property real lfoTimingCaptionY: 65
    property real lfoTimingY: 76
    property real lfoSyncWidth: 110
    property real lfoDivisionX: 332
    property real lfoDivisionWidth: 90
    property real compactControlHeight: 22

    // The envelope section: the source, the follower's level meter and its
    // response.
    property real sourceY: 44
    property real sourceWidth: 180
    property real lowerRowY: 74
    property real meterHeight: 24
    property real meterCaptionY: 1
    property real meterBarY: 12
    property real meterBarHeight: 10
    property real meterFadeAlpha: 0.35
    property real envelopeViewX: 206
    property real envelopeViewHeight: 66

    // The output section: the dry/wet balance bar.
    property real balanceY: 34
    property real balanceHeight: 30
    property real balanceBarY: 13
    property real balanceBarHeight: 8
    // The lit fills inside the meter and balance bars.
    property real barFillRadius: 1

    // The response screen: a phosphor glow fading to a deep centre, the
    // graticule, the set and live traces with their glows, the cutoff marker
    // and the modulation ruler along the top.
    property color screenDeep: "#050707"
    property real screenGlowStop: 0.55
    property color liveTrace: "#c9fff4"
    property real screenInset: 10
    property real screenTop: 30
    property real screenPlotLeft: 10
    property real screenPlotTop: 34
    property real screenPlotRight: 14
    property real screenPlotBottom: 10
    property var screenSetDash: [4, 3]
    property var screenCutoffDash: [2, 3]
    property real screenSetTraceAlpha: 0.45
    property real screenSetAreaAlpha: 0.26
    property real screenSetHaloWidth: 7
    property real screenSetHaloAlpha: 0.10
    property real screenSetGlowWidth: 3.5
    property real screenSetGlowAlpha: 0.22
    property real screenSetLineWidth: 1.6
    property real screenLiveAreaAlpha: 0.30
    property real screenLiveHaloWidth: 8
    property real screenLiveHaloAlpha: 0.12
    property real screenLiveGlowWidth: 3.5
    property real screenLiveGlowAlpha: 0.26
    property real screenLiveLineWidth: 1.8
    property real screenCutoffAlpha: 0.55
    property real screenRulerOffset: 9
    property real screenRulerStroke: 3
    property real screenRulerTickRise: 4
    property real screenCaptionX: 12
    property real screenCaptionY: 8
    property real screenCaptionSpacing: 1.6
    // Clear of the frame's LIVE / PREVIEW chip in the screen's corner.
    property real screenReadoutRight: 104
    property real screenReadoutY: 6
    property real screenReadoutSpacing: 14
    property real screenReadoutGap: 5
    property real cutoffHandleSize: 14
    property real cutoffHandleStroke: 1.5

    // The LFO view: the left channel's trace with a glow, the right fainter.
    property color lfoDot: "#e6f2ff"
    property real lfoHeadroom: 3
    property real lfoRightAlpha: 0.35
    property real lfoRightWidth: 1.2
    property real lfoGlowAlpha: 0.2
    property real lfoGlowWidth: 4
    property real lfoLineWidth: 1.5
    property real lfoDotInset: 5
    property real lfoDotRadius: 3
}
