import QtQuick
import "../devicekit"

// Prism design tokens. Neutrals match the DAW shell; each section gets one
// accent from a spectrum so the panel reads as one instrument at a glance.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "prism"
    accent: osc1
    menuGap: 4
    menuRadius: 8
    menuFill: raised
    menuEdge: line
    menuRowHeight: 26
    menuHeaderHeight: 24
    menuRowRadius: 5
    menuHighlightAlpha: 0.16
    menuHeaderOffset: 2
    menuHeaderInk: dim
    menuInk: text
    menuHeaderSpacing: 1.2
    knobDiameter: 40
    knobDialPad: 8
    knobDialY: 2
    knobWidthPad: 18
    knobMinWidth: 56
    knobHeightPad: 32
    knobDisabledOpacity: 0.55
    knobLongName: "none"
    knobTip: false
    knobCaptionInset: 0
    knobLabelUpper: false
    knobLabelFont: ""
    knobLabelSize: 10
    knobLabelSpacing: 0.3
    knobLabelInk: dim
    knobValueFont: ""
    knobValueBold: true
    knobNextNoteMark: true
    knobNextNoteInk: dim
    dropdownHeight: 22
    dropdownRadius: 6
    dropdownEdge: line
    dropdownDisabledOpacity: 0.55
    dropdownSpeaksGroup: false
    dropdownTextRight: 26
    dropdownFont: ""
    dropdownCaret: true
    dropdownChevronInk: dim
    switchHeight: 22
    switchPad: 36
    switchRadius: 11
    switchDisabledOpacity: 0.55
    switchLedX: 6
    switchLedSize: 8
    switchTextX: 20
    switchTextSize: 10
    switchLetterSpacing: 0.4
    arrowWidth: 30
    arrowHeight: 30
    arrowRadius: 8
    arrowInk: text
    arrowGlyphSize: 18
    statusLampSize: 7
    statusEdgeAlpha: 0.45
    statusPendingInk: dim

    property color canvas: "#0d1015"
    property color panel: "#151a22"
    property color raised: "#1c222c"
    well: "#0f1319"
    property color line: "#2a313d"
    property color text: "#e9eef5"
    property color dim: "#8995a5"
    property color faint: "#788497"
    ink: text
    inkDim: dim
    inkFaint: faint
    inkGround: panel
    warning: "#e0ae75"
    live: "#72e3c0"

    property color osc1: "#72e3c0"
    property color osc2: "#6cc6ff"
    property color mixer: "#a99bff"
    property color filter: "#ffb35c"
    property color amp: "#ff7d9c"
    property color env2: "#f59bd6"
    property color lfo1: "#a6e36c"
    property color lfo2: "#e8d56a"
    property color mod: "#c5acfa"
    property color voice: "#8bcbdc"
    property color fx: "#ec9a6e"

    // Panel layout: the case, the gap between sections and the three rows
    // of sections under the header.
    property int panelWidth: 1240
    property real panelRadius: 12
    property real sectionGap: 10
    property real headerHeight: 54
    property real sourceRowHeight: 266
    property real shapeRowHeight: 206
    property real playRowHeight: 180

    // Header: the prism glyph, the wordmark and its tagline, the preset box
    // with its category chip, and the next-note legend.
    property real glyphWidth: 34
    property real glyphHeight: 30
    property real glyphStroke: 1.6
    property real wordmarkY: 9
    property int wordmarkSize: 21
    property real wordmarkSpacing: 5
    property real taglineX: 1
    property real taglineLift: 2
    property real taglineSpacing: 1.6
    property real presetBoxWidth: 300
    property real presetBoxHeight: 30
    property real presetChipHeight: 18
    property color presetChipFill: Qt.rgba(osc1.r, osc1.g, osc1.b, 0.14)
    // Letter spacing of the small capital tags (category, EDITED).
    property real presetTagSpacing: 1
    // Width the preset name leaves for the chip and the tags beside it.
    property real presetNameClearance: 150
    property real presetChevronRight: 9
    property real legendSpacing: 5
    property real nextNoteMarkRotation: 45

    // Sections: a rounded card with an accent bar beside its title, trailing
    // controls at the header's right and the body below.
    property real sectionRadius: 10
    property real sectionPad: 10
    property real sectionBarX: 12
    property real sectionBarY: 13
    property real sectionBarWidth: 3
    property real sectionBarHeight: 12
    property real sectionTitleX: 21
    property real sectionTitleY: 11
    property real sectionTitleSpacing: 1.4
    property real sectionTrailingY: 7
    property real sectionTrailingHeight: 22
    property real sectionBodyY: 36
    property real sectionBodyBottom: 8
    // Small capital captions over a group, and the row they head.
    property real captionSpacing: 1.1
    property real captionRowHeight: 14

    // Control sizes inside the sections.
    property real segmentedCompactHeight: 22
    property real sectionKnobDiameter: 30
    property real compactKnobDiameter: 28
    property real levelKnobDiameter: 36
    property real filterKnobDiameter: 44
    property real effectKnobDiameter: 34
    property real phaseDropdownWidth: 76
    property real modeDropdownWidth: 104

    // Mod matrix: the slot's number badge.
    property real slotBadgeWidth: 20
    property real slotBadgeHeight: 22
    property real slotBadgeRadius: 5
    property real slotActiveAlpha: 0.2

    // Amount bar: a bipolar fill from the centre, inset in the well.
    property real amountWidth: 120
    property real amountHeight: 22
    property real amountInactiveOpacity: 0.45
    property real amountInset: 2
    property real amountFillInset: 3
    property real amountFillAlpha: 0.45
    property real amountTextRight: 7

    // Pictures (wave, LFO, filter): the plotted trace, its fill and the
    // value badges over them.
    property real traceLineWidth: 1.8
    property real wavePlotInset: 6
    property real waveFillAlpha: 0.12
    property real ghostAlpha: 0.16
    property real lfoPlotInset: 6
    property var lfoFadeDash: [2, 3]
    property real badgePad: 10
    property real badgeLetterSpacing: 0.8
    property real lfoBadgeHeight: 16
    property color lfoBadgeFill: Qt.rgba(panel.r, panel.g, panel.b, 0.85)
    property real voicesBadgeInset: 5
    property real voicesBadgeHeight: 15
    property color voicesBadgeFill: Qt.rgba(panel.r, panel.g, panel.b, 0.88)
    property real filterPlotInset: 4
    property real filterDragInset: 6
    property real filterGridAlpha: 0.7
    property real filterZeroAlpha: 0.9
    property var filterZeroDash: [3, 3]
    property real filterLabelGap: 3
    property real filterLabelLift: 6
    property real filterFillAlpha: 0.30
    property real filterFillEndAlpha: 0.02
    property real filterLineWidth: 2
    property real filterHandleSize: 12
    property real filterHandleEdgeWidth: 2

    // Icons: small drawn wave shapes inside segmented cells.
    property real iconWidth: 22
    property real iconHeight: 12
    property real iconStroke: 1.6
    property real iconInsetX: 1
    property real iconInsetY: 1.5

    // Performance wheels: a ridged slot with a sprung thumb and captions.
    property real wheelWidth: 44
    property real wheelHeight: 120
    property real wheelSlotWidth: 24
    property real wheelSlotRadius: 7
    // Height under the slot for the label and value.
    property real wheelCaptionSpace: 30
    property int wheelRidges: 9
    property real wheelRidgeInset: 5
    property real wheelRidgeTop: 6
    property real wheelThumbInset: 2
    property real wheelThumbHeight: 14
    property real wheelThumbAlpha: 0.6
    property real wheelThumbPressedAlpha: 0.85
    property int wheelReturnDuration: 140
    property real wheelLabelSpacing: 0.6
    property real wheelValueY: 16

    // The knob face: a ring track and value arc around a shaded body.
    property real knobRingWidth: 3
    property real knobBodyInset: 5
    property color knobBodyTop: Qt.lighter(raised, 1.45)
    property color knobBodyEdge: Qt.darker(line, 1.1)
    knobIndexInset: 2
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = knobStartAngle
        const sweep = knobSweepAngle
        const at = start + sweep * knob.position
        ctx.lineCap = "round"
        // Track.
        ctx.lineWidth = knobRingWidth
        ctx.strokeStyle = knob.theme.line
        ctx.beginPath()
        ctx.arc(c, c, radius, start, start + sweep)
        ctx.stroke()
        // Value arc, from the centre for bipolar parameters.
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > knobArcMinimum) {
            ctx.strokeStyle = knob.accent
            ctx.lineWidth = knobRingWidth
            ctx.beginPath()
            ctx.arc(c, c, radius, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Body.
        const body = radius - knobBodyInset
        const fill = ctx.createLinearGradient(0, c - body, 0, c + body)
        fill.addColorStop(0, knobBodyTop)
        fill.addColorStop(1, knob.theme.raised)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, body, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = hairline
        ctx.strokeStyle = knob.activeFocus ? knob.accent : knobBodyEdge
        ctx.stroke()
        // Pointer.
        ctx.strokeStyle = knob.theme.text
        ctx.lineWidth = knobIndexStroke
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * body * knobIndexFrom, c + Math.sin(at) * body * knobIndexFrom)
        ctx.lineTo(c + Math.cos(at) * (body - knobIndexInset), c + Math.sin(at) * (body - knobIndexInset))
        ctx.stroke()
    }

    // Dropdown and switch: rounded wells lit in the section colour.
    property color dropdownHoverFill: Qt.lighter(well, 1.35)
    property real switchWashAlpha: 0.18
    property real switchRimAlpha: 0.6
    function dropdownFill(dropdown) {
        return dropdown.hovered ? dropdownHoverFill : well
    }
    function dropdownInk(dropdown) {
        return dropdown.current === 0 && dropdown.choices[0] === "Off" ? dim : text
    }
    function switchFill(toggle, stop) {
        return toggle.on ? Qt.rgba(toggle.accent.r, toggle.accent.g, toggle.accent.b, switchWashAlpha) : well
    }
    function switchEdge(toggle) {
        return toggle.activeFocus ? toggle.accent
            : toggle.on ? Qt.rgba(toggle.accent.r, toggle.accent.g, toggle.accent.b, switchRimAlpha) : line
    }
    function switchLed(toggle) {
        return toggle.on ? toggle.accent : "transparent"
    }
    function switchLedEdge(toggle) {
        return toggle.on ? toggle.accent : dim
    }
    function switchInk(toggle) {
        return toggle.on ? toggle.accent : dim
    }

    // Segmented choices (the kit's SegmentedControl as PrismSegmented):
    // equal cells in a rounded well, the chosen one washed and edged in the
    // section colour.
    segmentedHeight: 24
    segmentedCellWidth: 34
    segmentedEqualCells: true
    segmentedMargin: 2
    segmentedRadius: radiusLg
    segmentedCellRadius: radiusMd
    segmentedFrame: well
    segmentedFrameEdge: line
    segmentedDisabledOpacity: 0.55
    function segmentedFill(cell, stop) {
        return cell.hovered ? raised : "transparent"
    }
    function segmentedEdge(cell) {
        return "transparent"
    }
    function segmentedInk(cell) {
        return dim
    }
    function segmentedFont(cell) {
        return Qt.font({ pixelSize: typeSmall, bold: cell.on })
    }

    // Header: rounded preset arrows and a toned status pill.
    function arrowFill(arrow, stop) {
        return arrow.hovered ? raised : panel
    }
    function arrowEdge(arrow) {
        return arrow.activeFocus ? osc1 : line
    }
    property real statusWashAlpha: 0.12
    function statusFill(tone) {
        return Qt.rgba(tone.r, tone.g, tone.b, statusWashAlpha)
    }
    function statusInk(tone) {
        return tone
    }

    // Envelope pictures: a rounded outline over a fading fill, the sustain
    // hold dashed over it, ringed drag handles on the nodes.
    envelopeRadius: 6
    envelopeEdge: line
    envelopeMargin: 0
    envelopePlotSide: 8
    envelopePlotTop: 8
    envelopePlotBottom: 8
    envelopeLineWidth: 2
    envelopeLineJoin: "round"
    envelopeFillOverLine: false
    envelopeFillAlpha: 0.28
    envelopeGradient: true
    envelopeFillEndAlpha: 0.02
    envelopeMarkerInk: faint
    envelopeMarkerDash: [2, 3]
    envelopeKnotFill: panel
    envelopeKnotRadius: 5.5
    envelopeKnotLineWidth: 2
    envelopeKnotGrab: 6
}
